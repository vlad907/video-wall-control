// Capture devices on Windows through Media Foundation. PCIe and USB capture cards install as
// video capture devices (like webcams); Media Foundation's video processor converts whatever
// their driver delivers (YUY2, NV12, MJPEG...) to BGRA. The device's largest format is used, at
// its highest frame rate up to 60.
//
// STATUS: written against the Windows SDK and compile-checked with MinGW, not yet run on the
// venue's card. If a card offers nothing Media Foundation can convert, its error says so.
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "io/capture.hpp"

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

std::string hr(const char* what, HRESULT h) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s (0x%08lx)", what, static_cast<unsigned long>(h));
    return buf;
}

// COM and Media Foundation for the calling thread.
struct MfScope {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    ~MfScope() {
        if (SUCCEEDED(mf)) MFShutdown();
        if (SUCCEEDED(co)) CoUninitialize();
    }
};

std::vector<ComPtr<IMFActivate>> videoDevices(std::string& error) {
    std::vector<ComPtr<IMFActivate>> out;
    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(&attr, 1))) return error = "Media Foundation isn't available", out;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** list = nullptr;
    UINT32 n = 0;
    HRESULT h = MFEnumDeviceSources(attr.Get(), &list, &n);
    if (FAILED(h)) return error = hr("listing capture devices failed", h), out;
    for (UINT32 i = 0; i < n; ++i) {
        out.emplace_back(list[i]);  // AddRef
        list[i]->Release();
    }
    CoTaskMemFree(list);
    return out;
}

std::string friendlyName(IMFActivate* a) {
    WCHAR* name = nullptr;
    UINT32 len = 0;
    std::string s;
    if (SUCCEEDED(a->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len))) {
        s = narrow(name);
        CoTaskMemFree(name);
    }
    return s;
}

class MfCapture final : public CaptureStream {
public:
    MfCapture(std::string name, CaptureFrameFn onFrame) : name_(std::move(name)), onFrame_(std::move(onFrame)) {
        thread_ = std::thread([this] { run(); });
    }
    ~MfCapture() override {
        stop_ = true;
        thread_.join();
    }
    std::string error() const override {
        std::lock_guard l(m_);
        return error_;
    }
    // Waits until the device opened (true) or failed (false, error set).
    bool waitOpen(std::string& error) {
        std::unique_lock l(m_);
        cv_.wait_for(l, std::chrono::seconds(8), [this] { return state_ != 0; });
        if (state_ == 1) return true;
        error = state_ == 0 ? "the capture device didn't open in time" : error_;
        return false;
    }

private:
    void setState(int s, const std::string& err) {
        std::lock_guard l(m_);
        state_ = s;
        error_ = err;
        cv_.notify_all();
    }

    std::string open() {
        std::string err;
        auto devs = videoDevices(err);
        if (!err.empty()) return err;
        ComPtr<IMFActivate> act;
        for (auto& d : devs)
            if (friendlyName(d.Get()) == name_) act = d;
        if (!act) return "no capture device named '" + name_ + "' (unplugged, or its driver isn't installed)";
        HRESULT h = act->ActivateObject(IID_PPV_ARGS(&source_));
        if (FAILED(h)) return hr(("can't open " + name_ + ": another program may be using it (close OBS)").c_str(), h);
        ComPtr<IMFAttributes> ra;
        MFCreateAttributes(&ra, 2);
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // any format -> RGB32
        ra->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        h = MFCreateSourceReaderFromMediaSource(source_.Get(), ra.Get(), &reader_);
        if (FAILED(h)) return hr("can't read from the capture device", h);
        const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        // The largest format, then the highest frame rate up to 60.
        ComPtr<IMFMediaType> best;
        uint64_t bestScore = 0;
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(reader_->GetNativeMediaType(stream, i, &t))) break;
            UINT32 w = 0, hgt = 0, num = 0, den = 1;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &hgt);
            MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
            double fps = den ? double(num) / den : 0;
            uint64_t score = static_cast<uint64_t>(w) * hgt * 1000 + static_cast<uint64_t>(std::min(fps, 60.0) * 10);
            if (score > bestScore) bestScore = score, best = t;
        }
        if (best) reader_->SetCurrentMediaType(stream, nullptr, best.Get());
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        h = reader_->SetCurrentMediaType(stream, nullptr, out.Get());
        if (FAILED(h)) return hr(("Windows can't convert " + name_ + "'s video to RGB").c_str(), h);
        readSize();
        return "";
    }

    void readSize() {
        ComPtr<IMFMediaType> cur;
        if (SUCCEEDED(reader_->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur))) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
            width_ = static_cast<int>(w);
            height_ = static_cast<int>(h);
            defaultStride_ = static_cast<LONG>(MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, w * 4));
        }
    }

    void run() {
        MfScope mf;
        std::string err = open();
        if (!err.empty()) {
            setState(2, err);
            return;
        }
        setState(1, "");
        while (!stop_) {
            DWORD streamIndex = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            HRESULT h = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &streamIndex, &flags, &ts, &sample);
            if (FAILED(h)) {
                setState(1, hr("the capture device stopped", h));
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                setState(1, "the capture device stopped sending");
                break;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) readSize();
            if (!sample || width_ <= 0 || height_ <= 0) continue;
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
            ComPtr<IMF2DBuffer> b2;
            if (SUCCEEDED(buf.As(&b2))) {
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
                    onFrame_(scan0, width_, height_, pitch);  // scan0 is the top row; pitch < 0 = bottom-up
                    b2->Unlock2D();
                }
            } else {
                BYTE* p = nullptr;
                DWORD max = 0, len = 0;
                if (SUCCEEDED(buf->Lock(&p, &max, &len))) {
                    LONG stride = defaultStride_;
                    const BYTE* top = stride < 0 ? p + static_cast<size_t>(-stride) * (height_ - 1) : p;
                    if (len >= static_cast<DWORD>(std::abs(stride)) * static_cast<DWORD>(height_)) onFrame_(top, width_, height_, stride);
                    buf->Unlock();
                }
            }
        }
        reader_.Reset();
        if (source_) source_->Shutdown();
        source_.Reset();
    }

    std::string name_;
    CaptureFrameFn onFrame_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex m_;
    std::condition_variable cv_;
    int state_ = 0;  // 0 opening, 1 open (error_ set if it stopped since), 2 failed to open
    std::string error_;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;
    int width_ = 0, height_ = 0;
    LONG defaultStride_ = 0;
};

}  // namespace

std::vector<CaptureDevice> platformCaptureDevices(std::string& error) {
    MfScope mf;
    std::vector<CaptureDevice> out;
    for (auto& d : videoDevices(error)) {
        std::string n = friendlyName(d.Get());
        if (!n.empty()) out.push_back({n, "device"});
    }
    return out;
}

std::unique_ptr<CaptureStream> openPlatformCapture(const std::string& name, CaptureFrameFn onFrame, std::string& error) {
    auto c = std::make_unique<MfCapture>(name, std::move(onFrame));
    if (!c->waitOpen(error)) return nullptr;
    return c;
}

}  // namespace mosaic
