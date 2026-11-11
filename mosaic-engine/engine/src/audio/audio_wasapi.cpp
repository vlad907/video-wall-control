// Windows sound outputs through WASAPI, shared mode (other programs keep playing too).
// Each stream runs its own thread: it opens the device, asks for a buffer every period
// (event driven, 50 ms buffer) and fills it through the render callback, telling it when the
// first frame will be heard (now + what's already queued + the device's own latency).
#include "audio/audio_device.hpp"

#include <windows.h>
// WIN32_LEAN_AND_MEAN leaves out the COM/property types the device-name key needs.
#include <objbase.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
// Define (not just declare) PKEY_Device_FriendlyName in this file, so it links without propsys tricks.
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

#include "audio/audio_ring.hpp"

namespace mosaic {

using Microsoft::WRL::ComPtr;

namespace {

std::string narrow(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string hr(const char* what, HRESULT h) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s failed (0x%08lx)", what, static_cast<unsigned long>(h));
    return buf;
}

// COM for the calling thread (multithreaded; fine if it was already initialised either way).
struct ComScope {
    HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(h)) CoUninitialize();
    }
};

ComPtr<IMMDeviceEnumerator> enumerator(std::string& error) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT h = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    if (FAILED(h)) error = hr("opening Windows audio", h);
    return e;
}

std::string friendlyName(IMMDevice* d) {
    ComPtr<IPropertyStore> props;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &props))) return "";
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = narrow(v.pwszVal);
    PropVariantClear(&v);
    return name;
}

std::string deviceId(IMMDevice* d) {
    LPWSTR id = nullptr;
    if (FAILED(d->GetId(&id))) return "";
    std::string s = narrow(id);
    CoTaskMemFree(id);
    return s;
}

bool isFloat(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return false;
}

class WasapiStream final : public AudioDeviceStream {
public:
    WasapiStream(std::string id, AudioRender render) : id_(std::move(id)), render_(std::move(render)) {
        thread_ = std::thread([this] { run(); });
    }
    ~WasapiStream() override {
        stop_ = true;
        thread_.join();
    }
    std::string error() const override {
        std::lock_guard l(m_);
        return error_;
    }
    std::string deviceName() const override {
        std::lock_guard l(m_);
        return name_;
    }
    int channels() const override { return channels_; }
    double rate() const override { return rate_; }

private:
    void setError(const std::string& e) {
        std::lock_guard l(m_);
        error_ = e;
    }

    void run() {
        ComScope com;
        DWORD task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        while (!stop_) {
            std::string err = play(event);
            if (stop_) break;
            setError(err.empty() ? "the device stopped" : err);
            channels_ = 0;
            for (int i = 0; i < 20 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));  // then try again
        }
        CloseHandle(event);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    // Plays until stopped or the device fails; returns why.
    std::string play(HANDLE event) {
        std::string err;
        auto e = enumerator(err);
        if (!e) return err;
        ComPtr<IMMDevice> dev;
        HRESULT h = id_.empty() ? e->GetDefaultAudioEndpoint(eRender, eConsole, &dev) : e->GetDevice(widen(id_).c_str(), &dev);
        if (FAILED(h)) return "the sound device isn't connected";
        {
            std::lock_guard l(m_);
            name_ = friendlyName(dev.Get());
        }
        ComPtr<IAudioClient> client;
        if (FAILED(h = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return hr("opening the device", h);
        WAVEFORMATEX* fmt = nullptr;
        if (FAILED(h = client->GetMixFormat(&fmt))) return hr("reading the device format", h);
        std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> fmtHold(fmt, CoTaskMemFree);
        const bool fl = isFloat(fmt);
        if (!fl && fmt->wBitsPerSample != 16) return "the device's format isn't supported (" + std::to_string(fmt->wBitsPerSample) + "-bit)";
        h = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 500000 /* 50 ms */, 0, fmt, nullptr);
        if (FAILED(h)) return hr("starting the device", h);
        client->SetEventHandle(event);
        UINT32 bufferFrames = 0;
        client->GetBufferSize(&bufferFrames);
        REFERENCE_TIME latency = 0;
        client->GetStreamLatency(&latency);
        ComPtr<IAudioRenderClient> out;
        if (FAILED(h = client->GetService(IID_PPV_ARGS(&out)))) return hr("opening the device's buffer", h);
        const int ch = fmt->nChannels;
        const double rate = fmt->nSamplesPerSec;
        channels_ = ch;
        rate_ = rate;
        std::vector<float> tmp;
        if (FAILED(h = client->Start())) return hr("starting playback", h);
        setError("");
        while (!stop_) {
            WaitForSingleObject(event, 200);
            UINT32 padding = 0;
            if (FAILED(h = client->GetCurrentPadding(&padding))) return h == AUDCLNT_E_DEVICE_INVALIDATED ? "the sound device was unplugged or changed" : hr("playing", h);
            UINT32 frames = bufferFrames - padding;
            if (!frames) continue;
            BYTE* data = nullptr;
            if (FAILED(h = out->GetBuffer(frames, &data))) return h == AUDCLNT_E_DEVICE_INVALIDATED ? "the sound device was unplugged or changed" : hr("playing", h);
            double heardAt = steadySeconds() + padding / rate + latency / 1e7;
            float* dst = fl ? reinterpret_cast<float*>(data) : (tmp.resize(static_cast<size_t>(frames) * ch), tmp.data());
            render_(dst, static_cast<int>(frames), ch, rate, heardAt);
            if (!fl) {
                auto* s16 = reinterpret_cast<int16_t*>(data);
                for (size_t i = 0; i < tmp.size(); ++i) s16[i] = static_cast<int16_t>(std::clamp(tmp[i], -1.f, 1.f) * 32767.f);
            }
            out->ReleaseBuffer(frames, 0);
        }
        client->Stop();
        return "";
    }

    std::string id_;
    AudioRender render_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> channels_{0};
    std::atomic<double> rate_{0};
    mutable std::mutex m_;
    std::string error_ = "starting…", name_;
};

// Records a sound input (capture card audio, line in) into a ring, in shared mode.
class WasapiInput final : public AudioInputStream {
public:
    WasapiInput(std::string id, std::shared_ptr<AudioRing> ring) : id_(std::move(id)), ring_(std::move(ring)) {
        thread_ = std::thread([this] { run(); });
    }
    ~WasapiInput() override {
        stop_ = true;
        thread_.join();
    }
    std::string error() const override {
        std::lock_guard l(m_);
        return error_;
    }
    std::string deviceName() const override {
        std::lock_guard l(m_);
        return name_;
    }
    int channels() const override { return channels_; }

private:
    void setError(const std::string& e) {
        std::lock_guard l(m_);
        error_ = e;
    }
    void run() {
        ComScope com;
        DWORD task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        while (!stop_) {
            std::string err = record(event);
            if (stop_) break;
            setError(err.empty() ? "the sound input stopped" : err);
            channels_ = 0;
            for (int i = 0; i < 20 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CloseHandle(event);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }
    std::string record(HANDLE event) {
        std::string err;
        auto e = enumerator(err);
        if (!e) return err;
        ComPtr<IMMDevice> dev;
        HRESULT h = id_.empty() ? e->GetDefaultAudioEndpoint(eCapture, eConsole, &dev) : e->GetDevice(widen(id_).c_str(), &dev);
        if (FAILED(h)) return "the sound input isn't connected";
        {
            std::lock_guard l(m_);
            name_ = friendlyName(dev.Get());
        }
        ComPtr<IAudioClient> client;
        if (FAILED(h = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return hr("opening the input", h);
        WAVEFORMATEX* fmt = nullptr;
        if (FAILED(h = client->GetMixFormat(&fmt))) return hr("reading the input format", h);
        std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> fmtHold(fmt, CoTaskMemFree);
        const bool fl = isFloat(fmt);
        if (!fl && fmt->wBitsPerSample != 16) return "the input's format isn't supported (" + std::to_string(fmt->wBitsPerSample) + "-bit)";
        h = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000 /* 20 ms */, 0, fmt, nullptr);
        if (FAILED(h)) return hr("starting the input", h);
        client->SetEventHandle(event);
        ComPtr<IAudioCaptureClient> cap;
        if (FAILED(h = client->GetService(IID_PPV_ARGS(&cap)))) return hr("opening the input's buffer", h);
        const int ch = fmt->nChannels;
        const int rate = static_cast<int>(fmt->nSamplesPerSec);
        channels_ = ch;
        std::vector<float> l, r;
        if (FAILED(h = client->Start())) return hr("starting the recording", h);
        setError("");
        while (!stop_) {
            WaitForSingleObject(event, 200);
            UINT32 packet = 0;
            while (SUCCEEDED(h = cap->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devPos = 0, qpcPos = 0;
                if (FAILED(h = cap->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos))) break;
                l.assign(frames, 0.f);
                r.assign(frames, 0.f);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                    for (UINT32 i = 0; i < frames; ++i) {
                        float a, b;
                        if (fl) {
                            const float* f = reinterpret_cast<const float*>(data) + static_cast<size_t>(i) * ch;
                            a = f[0];
                            b = ch > 1 ? f[1] : f[0];
                        } else {
                            const int16_t* s16 = reinterpret_cast<const int16_t*>(data) + static_cast<size_t>(i) * ch;
                            a = s16[0] / 32768.f;
                            b = (ch > 1 ? s16[1] : s16[0]) / 32768.f;
                        }
                        l[i] = a;
                        r[i] = b;
                    }
                }
                // When it was recorded (QPC, the steady clock's base on Windows), else now.
                double at = qpcPos ? static_cast<double>(qpcPos) / 1e7 : steadySeconds();
                ring_->push(l.data(), r.data(), static_cast<int>(frames), rate, at);
                cap->ReleaseBuffer(frames);
            }
            if (FAILED(h)) return h == AUDCLNT_E_DEVICE_INVALIDATED ? "the sound input was unplugged or changed" : hr("recording", h);
        }
        client->Stop();
        return "";
    }

    std::string id_;
    std::shared_ptr<AudioRing> ring_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> channels_{0};
    mutable std::mutex m_;
    std::string error_ = "starting…", name_;
};

std::vector<AudioDevice> listEndpoints(EDataFlow flow, std::string& error) {
    ComScope com;
    std::vector<AudioDevice> list;
    auto e = enumerator(error);
    if (!e) return list;
    std::string def;
    ComPtr<IMMDevice> d;
    if (SUCCEEDED(e->GetDefaultAudioEndpoint(flow, eConsole, &d))) def = deviceId(d.Get());
    ComPtr<IMMDeviceCollection> all;
    HRESULT h = e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &all);
    if (FAILED(h)) {
        error = hr("listing sound devices", h);
        return list;
    }
    UINT n = 0;
    all->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(all->Item(i, &dev))) continue;
        AudioDevice a;
        a.id = deviceId(dev.Get());
        a.name = friendlyName(dev.Get());
        a.isDefault = a.id == def;
        ComPtr<IAudioClient> client;
        WAVEFORMATEX* fmt = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) && SUCCEEDED(client->GetMixFormat(&fmt))) {
            a.channels = fmt->nChannels;
            a.rate = static_cast<int>(fmt->nSamplesPerSec);
            CoTaskMemFree(fmt);
        }
        list.push_back(a);
    }
    return list;
}

}  // namespace

std::vector<AudioDevice> listAudioDevices(std::string& error) { return listEndpoints(eRender, error); }
std::vector<AudioDevice> listAudioInputs(std::string& error) { return listEndpoints(eCapture, error); }

std::unique_ptr<AudioInputStream> openAudioInput(const std::string& deviceId, std::shared_ptr<AudioRing> ring) {
    return std::make_unique<WasapiInput>(deviceId, std::move(ring));
}

std::unique_ptr<AudioDeviceStream> openAudioDevice(const std::string& deviceId, AudioRender render) {
    return std::make_unique<WasapiStream>(deviceId, std::move(render));
}

}  // namespace mosaic
