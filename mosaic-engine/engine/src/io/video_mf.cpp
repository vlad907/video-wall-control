// Windows video decoding with Media Foundation's Source Reader. With the renderer's D3D11
// device it decodes on the GPU (DXVA: H.264, HEVC, AV1, VP9 as far as the GPU and installed
// codec extensions go) and hands out NV12 textures, which the renderer converts on the GPU,
// so even 8K frames never touch the CPU. Without a hardware decoder for the file it falls
// back to software decoding into BGRA.
#include "io/video_file.hpp"

#include <windows.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

namespace mosaic {

using Microsoft::WRL::ComPtr;

namespace {

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string codecName(const GUID& g) {
    if (g == MFVideoFormat_H264) return "H.264";
    if (g == MFVideoFormat_HEVC) return "HEVC";
    if (g == MFVideoFormat_AV1) return "AV1";
    if (g == MFVideoFormat_VP90) return "VP9";
    if (g == MFVideoFormat_MPEG2) return "MPEG-2";
    if (g == MFVideoFormat_MP4V) return "MPEG-4";
    if (g == MFVideoFormat_MJPG) return "Motion JPEG";
    char buf[5] = {};
    std::memcpy(buf, &g.Data1, 4);  // FourCC-based subtypes keep the code in Data1
    return buf;
}

std::string hr(const char* what, HRESULT h) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed (0x%08lx)", what, static_cast<unsigned long>(h));
    return buf;
}

constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

class MfBackend final : public VideoBackend {
public:
    bool open(const std::string& path, ID3D11Device* device, std::string& error) {
        static const HRESULT started = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(started)) return error = "Media Foundation isn't available on this Windows (install the Media Feature Pack)", false;
        path_ = widen(path);
        if (!create(device, error)) return false;
        return true;
    }

    bool read(DecodedFrame& out, bool& endOfStream, std::string& error) override {
        DWORD index = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT h = reader_->ReadSample(kVideo, 0, &index, &flags, &ts, &sample);
        if (FAILED(h)) return error = hr("ReadSample", h), false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return endOfStream = true, true;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) readFormat();
        if (!sample) return true;

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->GetBufferByIndex(0, &buf))) return true;
        out.pts = ts / 1e7;
        out.width = info_.width;
        out.height = info_.height;

        ComPtr<IMFDXGIBuffer> dxgi;
        if (SUCCEEDED(buf.As(&dxgi))) {
            ComPtr<ID3D11Texture2D> tex;
            UINT sub = 0;
            if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dxgi->GetSubresourceIndex(&sub))) return true;
            out.gpuTexture = tex.Get();
            out.arraySlice = sub;
            out.colorSpace = colorSpace_;
            // Holding the sample keeps the decoder's surface from being reused until we're done.
            auto* held = new ComPtr<IMFSample>(sample);
            out.hold = std::shared_ptr<void>(held, [](void* p) { delete static_cast<ComPtr<IMFSample>*>(p); });
            info_.hardware = true;
            info_.decoder = std::string("GPU (DXVA), ") + (info_.bitDepth == 10 ? "P010" : "NV12") + (attempts_.empty() ? "" : " · tried: " + attempts_);
            return true;
        }
        if (device_) {
            attempts_ += std::string(attempts_.empty() ? "" : ", ") + (forceP010_ ? "P010" : "NV12") + " gave CPU frames";
            // Asked for GPU frames but got memory. With 10-bit video (most 8K / HDR files), Windows
            // accepts NV12 but then decodes in software to convert: ask the GPU for P010 instead.
            // Only if that gives memory frames too is there no hardware decoder: decode in software.
            std::string err;
            ID3D11Device* dev = device_;
            if (!forceP010_) {
                forceP010_ = true;
                if (create(dev, err) && info_.bitDepth == 10) {
                    seek(ts);
                    return true;
                }
            }
            if (!create(nullptr, err)) return error = err, false;
            seek(ts);
            return true;
        }
        // Software: BGRA (MF's RGB32) in memory; copied so MF can reuse its buffer.
        auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(info_.width) * info_.height * 4);
        BYTE* src = nullptr;
        LONG pitch = 0;
        ComPtr<IMF2DBuffer> b2;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&src, &pitch))) {
            copyRows(src, pitch, *pixels);
            b2->Unlock2D();
        } else {
            DWORD len = 0;
            if (FAILED(buf->Lock(&src, nullptr, &len))) return true;
            copyRows(src, stride_, *pixels);
            buf->Unlock();
        }
        out.bgra = pixels->data();
        out.stride = info_.width * 4;
        out.hold = pixels;
        info_.hardware = false;
        info_.decoder = "software (CPU)" + (attempts_.empty() ? std::string(": no GPU device for decoding") : " · GPU attempts: " + attempts_) +
                        " · see GET /api/system/decoders for the " + info_.codec + " decoders installed";
        return true;
    }

    bool rewind(std::string& error) override {
        if (!seek(0)) return error = "cannot go back to the start", false;
        return true;
    }

    VideoInfo info() const override { return info_; }

private:
    bool create(ID3D11Device* device, std::string& error) {
        reader_.Reset();
        device_ = device;
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 4);
        if (device) {
            UINT token = 0;
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &manager_)) && SUCCEEDED(manager_->ResetDevice(device, token))) {
                attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager_.Get());
                attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            } else {
                device_ = nullptr;
            }
        }
        if (!device_) attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // software YUV -> RGB32
        HRESULT h = MFCreateSourceReaderFromURL(path_.c_str(), attrs.Get(), &reader_);
        if (FAILED(h)) return error = h == MF_E_UNSUPPORTED_BYTESTREAM_TYPE ? "unsupported file type" : hr("opening the file", h), false;
        reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader_->SetStreamSelection(kVideo, TRUE);

        ComPtr<IMFMediaType> native;
        if (FAILED(reader_->GetNativeMediaType(kVideo, 0, &native))) return error = "the file has no video track", false;
        GUID sub{};
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        info_.codec = codecName(sub);
        UINT32 num = 0, den = 0;
        if (SUCCEEDED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den)) && den) info_.fps = double(num) / den;
        // HDR: PQ (HDR10) or HLG transfer. The GPU's video processor maps it to the canvas's SDR.
        UINT32 transfer = 0;
        native->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transfer);
        colorSpace_ = transfer == MFVideoTransFunc_2084 ? 1 : transfer == MFVideoTransFunc_HLG ? 2 : 0;
        info_.hdr = colorSpace_ != 0;

        // Hardware: 8-bit video comes out as NV12, 10-bit (most HDR / 8K HEVC) as P010.
        // Software: always RGB32 (Media Foundation converts).
        auto tryFormat = [&](const GUID& subtype) {
            ComPtr<IMFMediaType> want;
            MFCreateMediaType(&want);
            want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            want->SetGUID(MF_MT_SUBTYPE, subtype);
            return reader_->SetCurrentMediaType(kVideo, nullptr, want.Get());
        };
        h = forceP010_ && device_ ? E_FAIL : tryFormat(device_ ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
        info_.bitDepth = 8;
        if (FAILED(h) && device_) {
            if (!forceP010_) attempts_ += std::string(attempts_.empty() ? "" : ", ") + hr("NV12 refused", h);
            h = tryFormat(MFVideoFormat_P010);
            if (SUCCEEDED(h)) info_.bitDepth = 10;
            else attempts_ += std::string(attempts_.empty() ? "" : ", ") + hr("P010 refused", h);
        }
        if (FAILED(h))
            return error = h == MF_E_TOPO_CODEC_NOT_FOUND || h == MF_E_INVALIDMEDIATYPE
                               ? "no decoder for " + info_.codec + " (for HEVC / AV1 install the free HEVC / AV1 Video Extensions from the Microsoft Store)"
                               : hr("setting up the decoder", h),
                   false;
        readFormat();

        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(reader_->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)))
            info_.durationS = var.uhVal.QuadPart / 1e7;
        PropVariantClear(&var);
        return true;
    }

    void readFormat() {
        ComPtr<IMFMediaType> cur;
        if (FAILED(reader_->GetCurrentMediaType(kVideo, &cur))) return;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
        info_.width = static_cast<int>(w);
        info_.height = static_cast<int>(h);
        UINT32 stride = 0;
        stride_ = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<LONG>(static_cast<INT32>(stride)) : static_cast<LONG>(w * 4);
    }

    // RGB32 can come bottom-up (negative pitch); rows are written top-down.
    void copyRows(const BYTE* src, LONG pitch, std::vector<uint8_t>& dst) const {
        const size_t row = static_cast<size_t>(info_.width) * 4;
        for (int y = 0; y < info_.height; ++y) std::memcpy(dst.data() + y * row, src + static_cast<ptrdiff_t>(y) * pitch, row);
    }

    bool seek(LONGLONG hns) {
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = hns;
        HRESULT h = reader_->SetCurrentPosition(GUID_NULL, var);
        PropVariantClear(&var);
        return SUCCEEDED(h);
    }

    std::wstring path_;
    ID3D11Device* device_ = nullptr;
    ComPtr<IMFDXGIDeviceManager> manager_;
    ComPtr<IMFSourceReader> reader_;
    VideoInfo info_;
    LONG stride_ = 0;
    int colorSpace_ = 0;
    bool forceP010_ = false;  // NV12 gave memory frames: the video is 10-bit; ask the GPU for P010
    std::string attempts_;    // GPU formats tried and what happened (for VideoInfo::decoder)
};

}  // namespace

std::unique_ptr<VideoBackend> openVideoBackend(const std::string& path, void* nativeDevice, std::string& error) {
    auto b = std::make_unique<MfBackend>();
    if (!b->open(path, static_cast<ID3D11Device*>(nativeDevice), error)) return nullptr;
    return b;
}

std::vector<DecoderInfo> listVideoDecoders() {
    std::vector<DecoderInfo> out;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const std::pair<const char*, GUID> codecs[] = {{"H.264", MFVideoFormat_H264}, {"HEVC", MFVideoFormat_HEVC}, {"AV1", MFVideoFormat_AV1}, {"VP9", MFVideoFormat_VP90}};
    for (auto& [name, sub] : codecs) {
        for (UINT32 hwFlag : {static_cast<UINT32>(MFT_ENUM_FLAG_HARDWARE), static_cast<UINT32>(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT)}) {
            MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, sub};
            IMFActivate** list = nullptr;
            UINT32 n = 0;
            if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, hwFlag | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr, &list, &n))) continue;
            for (UINT32 i = 0; i < n; ++i) {
                WCHAR* fn = nullptr;
                UINT32 len = 0;
                std::string friendly = "(unnamed)";
                if (SUCCEEDED(list[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fn, &len))) {
                    int k = WideCharToMultiByte(CP_UTF8, 0, fn, -1, nullptr, 0, nullptr, nullptr);
                    friendly.assign(k > 0 ? k - 1 : 0, '\0');
                    if (k > 0) WideCharToMultiByte(CP_UTF8, 0, fn, -1, friendly.data(), k, nullptr, nullptr);
                    CoTaskMemFree(fn);
                }
                out.push_back({name, friendly, hwFlag == static_cast<UINT32>(MFT_ENUM_FLAG_HARDWARE)});
                list[i]->Release();
            }
            CoTaskMemFree(list);
        }
    }
    if (SUCCEEDED(mf)) MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

}  // namespace mosaic
