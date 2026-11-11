// Direct3D 11 backend (Windows).
//
// STATUS: written against the Windows SDK headers and compile-checked with
// MinGW-w64, but NOT yet run on Windows hardware. See docs/WINDOWS.md.
//
// Readback: each output has a ring of STAGING textures. After the GPU copy
// completes the staging texture is mapped and the mapped pointer is handed to
// the sender directly (NDI reads it with RowPitch as the stride); it is only
// unmapped when the sender releases the slot. No CPU-side pixel copy.
//
// Completion: with D3D11.4 (Windows 10 1703+) an ID3D11Fence signals an
// event that a completion thread waits on, so frames are delivered as soon
// as the GPU finishes. Older runtimes fall back to polling in beginFrame().
#include <windows.h>

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "render/renderer.hpp"
#include "render/shaders.hpp"

using Microsoft::WRL::ComPtr;

namespace mosaic {

namespace {

constexpr int kMaxFramesInFlight = 2;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s failed (hr=0x%08lx)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }
}

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

ComPtr<ID3DBlob> compile(const std::string& src, const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src.data(), src.size(), "mosaic.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::string msg = std::string("HLSL compile failed for ") + entry;
        if (errors) msg += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        throw std::runtime_error(msg);
    }
    return code;
}

// HDR video (P010, BT.2020 PQ or HLG) to the SDR canvas. The video processor in most drivers
// doesn't tone-map: everything brighter than SDR white clips per channel, which blows out
// highlights and shifts colours (neon greens, yellow skin). So HDR frames are converted here:
// YCbCr -> R'G'B' -> linear nits -> BT.709 -> a soft roll-off above a knee -> gamma 2.2.
const char* kHdrShader = R"(
Texture2D<float> lumaTex : register(t0);
Texture2D<float2> chromaTex : register(t1);
SamplerState samp : register(s0);
cbuffer Params : register(b0) { uint transfer; uint3 pad; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_full(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 pqToNits(float3 e) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0) / (c2 - c3 * p), 1.0 / m1);
}
float3 hlgToNits(float3 e) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    e = saturate(e);
    float3 s = lerp((exp((e - c) / a) + b) / 12.0, e * e / 3.0, step(e, 0.5));  // scene light 0..1
    float y = dot(s, float3(0.2627, 0.6780, 0.0593));
    return 1000.0 * s * pow(max(y, 1e-6), 0.2);  // OOTF for a 1000-nit display
}
float4 ps_hdr(V i) : SV_Target {
    // P010 keeps 10 bits in the top of 16; studio range.
    float y = (lumaTex.Sample(samp, i.uv) * 65535.0 / 64.0 - 64.0) / 876.0;
    float2 c = (chromaTex.Sample(samp, i.uv) * 65535.0 / 64.0 - 512.0) / 896.0;
    float3 e = float3(y + 1.4746 * c.y, y - 0.16455 * c.x - 0.57135 * c.y, y + 1.8814 * c.x);
    float3 nits = transfer == 2 ? hlgToNits(e) : pqToNits(e);
    float3 lin = mul(float3x3(1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187), nits) / 203.0;
    // Out of the BT.709 gamut: pull toward grey instead of clipping a channel.
    float l = dot(lin, float3(0.2126, 0.7152, 0.0722));
    float lo = min(lin.r, min(lin.g, lin.b));
    if (lo < 0) lin = l + (lin - l) * (l / max(l - lo, 1e-6));
    // Tone map on the brightest channel (keeps hue): linear up to the knee, then eases into white.
    const float knee = 0.6;
    float m = max(lin.r, max(lin.g, lin.b));
    if (m > knee) lin *= (knee + (1 - knee) * (1 - exp(-(m - knee) / (1 - knee)))) / m;
    return float4(pow(saturate(lin), 1.0 / 2.2), 1);
}
)";

// Total 3D-engine utilisation from the "GPU Engine" performance counters.
class GpuUtilization {
public:
    GpuUtilization() {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return;
        if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &counter_) != ERROR_SUCCESS) {
            PdhCloseQuery(query_);
            query_ = nullptr;
            return;
        }
        PdhCollectQueryData(query_);
    }
    ~GpuUtilization() {
        if (query_) PdhCloseQuery(query_);
    }
    double sample() {
        if (!query_) return -1;
        auto now = std::chrono::steady_clock::now();
        if (now - at_ < std::chrono::seconds(1)) return value_;
        at_ = now;
        if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return value_;
        DWORD bytes = 0, count = 0;
        PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        std::vector<unsigned char> buf(bytes);
        auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return value_;
        double total = 0;
        for (DWORD i = 0; i < count; ++i)
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) total += items[i].FmtValue.doubleValue;
        value_ = total > 100.0 ? 100.0 : total;
        return value_;
    }

private:
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    std::chrono::steady_clock::time_point at_{};
    double value_ = -1;
};

// Display output windows: no pointer, never activated or closed by anyone but the engine.
LRESULT CALLBACK displayWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_SETCURSOR:
            SetCursor(nullptr);
            return TRUE;
        case WM_CLOSE:
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_DPICHANGED:
            return 0;  // stay exactly on the monitor's rectangle (physical pixels)
    }
    return DefWindowProcW(h, msg, w, l);
}

// Windows would scale an unaware program's windows on a monitor at 150 % / 200 %; display outputs
// are placed in physical pixels, so the engine says it handles scaling itself.
void usePhysicalPixels() {
    static bool done = false;
    if (done) return;
    done = true;
    using SetContext = BOOL(WINAPI*)(HANDLE);
    auto fn = reinterpret_cast<SetContext>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")));
    if (!fn || !fn(reinterpret_cast<HANDLE>(-4)))  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        SetProcessDPIAware();
}

// The hardware GPUs (no software rasterizers), in DXGI's order: the numbering canvases use.
std::vector<ComPtr<IDXGIAdapter1>> hardwareAdapters() {
    std::vector<ComPtr<IDXGIAdapter1>> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        out.push_back(a);
    }
    return out;
}

ComPtr<IDXGIAdapter1> hardwareAdapter(int index) {
    auto all = hardwareAdapters();
    return index >= 0 && index < static_cast<int>(all.size()) ? all[static_cast<size_t>(index)] : nullptr;
}

class D3D11Renderer final : public Renderer {
public:
    D3D11Renderer(bool warp, int gpu) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        // A chosen GPU (canvas setting): its adapter, with the "unknown" driver type that requires.
        ComPtr<IDXGIAdapter1> chosen;
        if (!warp && gpu >= 0) {
            chosen = hardwareAdapter(gpu);
            if (!chosen) throw std::runtime_error("there's no GPU number " + std::to_string(gpu + 1) + " on this PC");
        }
        IDXGIAdapter* adapterArg = chosen.Get();
        D3D_DRIVER_TYPE type = chosen ? D3D_DRIVER_TYPE_UNKNOWN : warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        // Video support lets Media Foundation decode video files on this device (hardware decoding,
        // frames stay on the GPU). Some drivers / WARP refuse it; then videos decode in software.
        HRESULT hr = D3D11CreateDevice(adapterArg, type, nullptr, flags | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                       &device_, nullptr, &ctx_);
        if (FAILED(hr))
            check(D3D11CreateDevice(adapterArg, type, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &ctx_), "D3D11CreateDevice");

        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
            deviceName_ = narrow(desc.Description);

        // The completion thread maps staging textures on the immediate context.
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);

        ComPtr<ID3D11Device5> dev5;
        if (SUCCEEDED(device_.As(&dev5)) && SUCCEEDED(ctx_.As(&ctx4_)) &&
            SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            completionThread_ = std::thread([this] { completionLoop(); });
        }

        std::string src = std::string(shaders::kShaderCommon) + shaders::kHlslEntryPoints;
        auto vs = compile(src, "vs_main", "vs_5_0");
        auto psPattern = compile(src, "ps_pattern", "ps_5_0");
        auto psTextured = compile(src, "ps_textured", "ps_5_0");
        auto psUyvy = compile(src, "ps_textured_uyvy", "ps_5_0");
        check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_), "CreateVertexShader");
        check(device_->CreatePixelShader(psPattern->GetBufferPointer(), psPattern->GetBufferSize(), nullptr, &psPattern_), "CreatePixelShader");
        check(device_->CreatePixelShader(psTextured->GetBufferPointer(), psTextured->GetBufferSize(), nullptr, &psTextured_), "CreatePixelShader");
        check(device_->CreatePixelShader(psUyvy->GetBufferPointer(), psUyvy->GetBufferSize(), nullptr, &psUyvy_), "CreatePixelShader");

        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(Uniforms);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(device_->CreateBuffer(&cb, nullptr, &cbuffer_), "CreateBuffer");

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        check(device_->CreateSamplerState(&sd, &sampler_), "CreateSamplerState");

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        rd.DepthClipEnable = TRUE;
        check(device_->CreateRasterizerState(&rd, &raster_), "CreateRasterizerState");

        // Translucent textured draws blend by their alpha (opacity).
        D3D11_BLEND_DESC bd{};
        auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device_->CreateBlendState(&bd, &blendOver_), "CreateBlendState");
    }

    ~D3D11Renderer() override {
        while (!displays_.empty()) destroyDisplay(displays_.begin()->first);
        waitIdle();
        if (completionThread_.joinable()) {
            {
                std::lock_guard l(m_);
                quit_ = true;
            }
            cv_.notify_all();
            SetEvent(fenceEvent_);
            completionThread_.join();
        }
        if (fenceEvent_) CloseHandle(fenceEvent_);
    }

    std::string backendName() const override { return fence_ ? "Direct3D 11.4" : "Direct3D 11"; }
    std::string deviceName() const override { return deviceName_; }
    std::string deviceLost() override {
        HRESULT r = device_->GetDeviceRemovedReason();
        if (r == S_OK) return "";
        char buf[160];
        std::snprintf(buf, sizeof buf, "the GPU device was lost (0x%08lx: %s)", static_cast<unsigned long>(r),
                      r == DXGI_ERROR_DEVICE_HUNG ? "hung" : r == DXGI_ERROR_DEVICE_RESET ? "reset" : r == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "driver error" : r == DXGI_ERROR_DEVICE_REMOVED ? "removed or the driver restarted" : "other");
        return buf;
    }
    double gpuUtilization() override { return util_.sample(); }

    TexId createTexture(int w, int h, TexUsage usage) override {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        Texture t;
        t.w = w;
        t.h = h;
        if (usage == TexUsage::RenderTarget) {
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        } else {
            d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        }
        check(device_->CreateTexture2D(&d, nullptr, &t.tex), "CreateTexture2D");
        check(device_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv), "CreateShaderResourceView");
        if (usage == TexUsage::RenderTarget) check(device_->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv), "CreateRenderTargetView");
        TexId id = nextId_++;
        textures_[id] = t;
        return id;
    }

    void destroyTexture(TexId id) override {
        textures_.erase(id);
        videoOutViews_.erase(id);
    }

    // ---- display outputs ----
    DisplayId createDisplay(const std::string& monitor, std::string& error) override {
        usePhysicalPixels();
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(device_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter))) return error = "no DXGI adapter", 0;
        ComPtr<IDXGIOutput> output;
        DXGI_OUTPUT_DESC od{};
        for (UINT k = 0;; ++k) {
            ComPtr<IDXGIOutput> o;
            if (adapter->EnumOutputs(k, &o) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d{};
            if (SUCCEEDED(o->GetDesc(&d)) && d.AttachedToDesktop && narrow(d.DeviceName) == monitor) {
                output = o;
                od = d;
                break;
            }
        }
        if (!output) {
            for (auto& g : listGpus())
                for (auto& m : g.outputs)
                    if (m.name == monitor)
                        return error = monitor + " is connected to " + g.name + " (GPU " + std::to_string(g.index + 1) + "): render this canvas on that GPU", 0;
            return error = "no monitor " + monitor + " (unplugged, or turned off in Windows' display settings)", 0;
        }
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = displayWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"MosaicDisplayOutput";
            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            RegisterClassW(&wc);
            registered = true;
        }
        Display d;
        d.monitor = monitor;
        d.output = output;
        const RECT r = od.DesktopCoordinates;
        d.w = r.right - r.left;
        d.h = r.bottom - r.top;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) d.hz = dm.dmDisplayFrequency;
        d.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"MosaicDisplayOutput", L"Mosaic display output", WS_POPUP | WS_VISIBLE,
                                 r.left, r.top, d.w, d.h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!d.hwnd) return error = "cannot open a window on " + monitor, 0;
        ComPtr<IDXGIFactory2> factory;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = static_cast<UINT>(d.w);
        sd.Height = static_cast<UINT>(d.h);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // flip model: independent flip, no frame added by Windows
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) || FAILED(factory->CreateSwapChainForHwnd(device_.Get(), d.hwnd, &sd, nullptr, nullptr, &sc1)) ||
            FAILED(sc1.As(&d.swap))) {
            DestroyWindow(d.hwnd);
            return error = "cannot create a swap chain on " + monitor, 0;
        }
        factory->MakeWindowAssociation(d.hwnd, DXGI_MWA_NO_ALT_ENTER);
        d.swap->SetMaximumFrameLatency(1);
        d.waitable = d.swap->GetFrameLatencyWaitableObject();
        // Buffer 0 is always the current back buffer in Direct3D 11 (the runtime rotates them).
        Texture t;
        t.w = d.w;
        t.h = d.h;
        if (FAILED(d.swap->GetBuffer(0, IID_PPV_ARGS(&t.tex))) || FAILED(device_->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv))) {
            if (d.waitable) CloseHandle(d.waitable);
            d.swap.Reset();
            DestroyWindow(d.hwnd);
            return error = "cannot draw to " + monitor, 0;
        }
        d.target = nextId_++;
        textures_[d.target] = t;
        DisplayId id = nextDisplay_++;
        displays_[id] = std::move(d);
        return id;
    }

    void destroyDisplay(DisplayId id) override {
        auto it = displays_.find(id);
        if (it == displays_.end()) return;
        Display& d = it->second;
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        textures_.erase(d.target);
        ctx_->Flush();
        if (d.waitable) CloseHandle(d.waitable);
        d.swap.Reset();
        if (d.hwnd) DestroyWindow(d.hwnd);
        displays_.erase(it);
    }

    TexId displayTarget(DisplayId id) override {
        auto it = displays_.find(id);
        if (it == displays_.end()) return 0;
        Display& d = it->second;
        pumpDisplayMessages();
        // Can it take a frame (its last one is on screen)? If not, skip this one rather than wait.
        if (WaitForSingleObjectEx(d.waitable, 0, FALSE) != WAIT_OBJECT_0) {
            ++d.busy;
            return 0;
        }
        d.ready = true;
        return d.target;
    }

    void presentDisplay(DisplayId id) override {
        auto it = displays_.find(id);
        if (it == displays_.end() || !it->second.ready) return;
        Display& d = it->second;
        d.ready = false;
        HRESULT hr = d.swap->Present(1, 0);
        if (FAILED(hr)) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "present failed (0x%08lx)", static_cast<unsigned long>(hr));
            d.error = buf;
            return;
        }
        d.error.clear();
        ++d.presents;
        DXGI_FRAME_STATISTICS st{};
        if (SUCCEEDED(d.swap->GetFrameStatistics(&st)) && st.SyncRefreshCount) {
            if (d.seen) {
                UINT presents = st.PresentCount - d.lastPresent, refreshes = st.SyncRefreshCount - d.lastRefresh;
                if (refreshes > presents && refreshes - presents < 600) d.repeats += refreshes - presents;
            }
            d.seen = true;
            d.lastPresent = st.PresentCount;
            d.lastRefresh = st.SyncRefreshCount;
        }
    }

    DisplayStats displayStats(DisplayId id) override {
        DisplayStats s;
        auto it = displays_.find(id);
        if (it == displays_.end()) return s;
        const Display& d = it->second;
        s.monitor = d.monitor;
        s.width = d.w;
        s.height = d.h;
        s.refreshHz = d.hz;
        s.presents = d.presents;
        s.busy = d.busy;
        s.repeats = d.repeats;
        s.error = d.error;
        return s;
    }

    bool waitDisplayVBlank(DisplayId id) override {
        auto it = displays_.find(id);
        if (it == displays_.end() || !it->second.output) return false;
        pumpDisplayMessages();
        return SUCCEEDED(it->second.output->WaitForVBlank());
    }

    void* nativeDevice() override { return device_.Get(); }

    // NV12 (from the decoder) to BGRA with the GPU's video processor: hardware colour conversion
    // and scaling, no shader or CPU work, and fine at 8K.
    bool blitVideoFrame(TexId dst, void* nativeNV12Texture, unsigned arraySlice, int colorSpace) override {
        auto it = textures_.find(dst);
        if (it == textures_.end() || !it->second.rtv || !nativeNV12Texture) return false;
        Texture& out = it->second;
        if (colorSpace != 0 && toneMapHdr(out, static_cast<ID3D11Texture2D*>(nativeNV12Texture), arraySlice, colorSpace)) return true;
        if (!videoDev_ && (FAILED(device_.As(&videoDev_)) || FAILED(ctx_.As(&videoCtx_)))) return false;
        auto* src = static_cast<ID3D11Texture2D*>(nativeNV12Texture);
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (!vp_ || vpIn_[0] != sd.Width || vpIn_[1] != sd.Height || vpOut_[0] != UINT(out.w) || vpOut_[1] != UINT(out.h) || vpFormat_ != sd.Format) {
            vp_.Reset();
            vpe_.Reset();
            videoInViews_.clear();
            videoOutViews_.clear();
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
            cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            cd.InputWidth = sd.Width;
            cd.InputHeight = sd.Height;
            cd.OutputWidth = out.w;
            cd.OutputHeight = out.h;
            cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            if (FAILED(videoDev_->CreateVideoProcessorEnumerator(&cd, &vpe_)) || FAILED(videoDev_->CreateVideoProcessor(vpe_.Get(), 0, &vp_))) return false;
            vpIn_[0] = sd.Width;
            vpIn_[1] = sd.Height;
            vpOut_[0] = out.w;
            vpOut_[1] = out.h;
            vpFormat_ = sd.Format;
            vpColorSpace_ = -1;
            ComPtr<ID3D11VideoContext1> vc1;
            if (SUCCEEDED(videoCtx_.As(&vc1))) vc1->VideoProcessorSetOutputColorSpace1(vp_.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            videoCtx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            videoCtx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
        }
        // Views are kept per decoder surface (the decoder reuses a small pool of them).
        auto key = std::make_pair(static_cast<void*>(src), arraySlice);
        if (!videoInViews_.count(key)) {
            if (videoInViews_.size() > 64) videoInViews_.clear();  // the decoder moved to new surfaces
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
            iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            iv.Texture2D.ArraySlice = arraySlice;
            ComPtr<ID3D11VideoProcessorInputView> view;
            if (FAILED(videoDev_->CreateVideoProcessorInputView(src, vpe_.Get(), &iv, &view))) return false;
            videoInViews_[key] = view;
        }
        auto& outView = videoOutViews_[dst];
        if (!outView) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
            ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (FAILED(videoDev_->CreateVideoProcessorOutputView(out.tex.Get(), vpe_.Get(), &ov, &outView))) return false;
        }
        if (colorSpace != vpColorSpace_) {
            // Studio-range YUV in; full-range BT.709 RGB out (the canvas). For HDR the driver maps PQ / HLG to SDR.
            ComPtr<ID3D11VideoContext1> vc1;
            if (SUCCEEDED(videoCtx_.As(&vc1))) {
                DXGI_COLOR_SPACE_TYPE in = colorSpace == 1 ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020
                                           : colorSpace == 2 ? static_cast<DXGI_COLOR_SPACE_TYPE>(18)  // YCBCR_STUDIO_GHLG_TOPLEFT_P2020
                                                             : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
                vc1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0, in);
            }
            vpColorSpace_ = colorSpace;
        }
        RECT r{0, 0, out.w, out.h};
        videoCtx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &r);
        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = videoInViews_[key].Get();
        return SUCCEEDED(videoCtx_->VideoProcessorBlt(vp_.Get(), outView.Get(), 0, 1, &st));
    }

    ReadbackId createReadback(int w, int h, int slots) override {
        auto rb = std::make_shared<Readback>();
        rb->w = w;
        rb->h = h;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        rb->staging.resize(slots);
        rb->mapped.assign(slots, false);
        for (auto& s : rb->staging) check(device_->CreateTexture2D(&d, nullptr, &s), "CreateTexture2D(staging)");
        std::lock_guard l(m_);
        ReadbackId id = nextId_++;
        readbacks_[id] = rb;
        return id;
    }

    void destroyReadback(ReadbackId id) override {
        std::lock_guard l(m_);
        auto it = readbacks_.find(id);
        if (it == readbacks_.end()) return;
        for (size_t i = 0; i < it->second->staging.size(); ++i)
            if (it->second->mapped[i]) ctx_->Unmap(it->second->staging[i].Get(), 0);
        readbacks_.erase(it);
    }

    void waitIdle() override {
        ctx_->Flush();
        if (fence_) {
            std::unique_lock l(m_);
            cv_.wait(l, [this] { return pending_.empty(); });
        } else {
            while (!pending_.empty()) pollCompleted(true);
        }
    }

    void beginFrame(uint64_t token) override {
        if (fence_) {
            std::unique_lock l(m_);
            cv_.wait(l, [this] { return static_cast<int>(pending_.size()) < kMaxFramesInFlight; });
        } else {
            pollCompleted(false);
            while (static_cast<int>(pending_.size()) >= kMaxFramesInFlight) pollCompleted(true);
        }
        frame_ = Frame{};
        frame_.token = token;
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        device_->CreateQuery(&qd, &frame_.disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        device_->CreateQuery(&qd, &frame_.tsBegin);
        device_->CreateQuery(&qd, &frame_.tsEnd);
        if (!fence_) {
            qd.Query = D3D11_QUERY_EVENT;
            device_->CreateQuery(&qd, &frame_.done);
        }
        if (frame_.disjoint) ctx_->Begin(frame_.disjoint.Get());
        if (frame_.tsBegin) ctx_->End(frame_.tsBegin.Get());
    }

    void uploadBGRA(TexId dst, const uint8_t* data, int stride) override {
        Texture& t = textures_.at(dst);
        D3D11_MAPPED_SUBRESOURCE m{};
        check(ctx_->Map(t.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map(upload)");
        auto* out = static_cast<uint8_t*>(m.pData);
        size_t row = static_cast<size_t>(t.w) * 4;
        for (int y = 0; y < t.h; ++y) std::memcpy(out + y * m.RowPitch, data + static_cast<size_t>(y) * stride, row);
        ctx_->Unmap(t.tex.Get(), 0);
    }

    void beginPass(TexId dst, const float* clear) override {
        Texture& t = textures_.at(dst);
        passW_ = t.w;
        passH_ = t.h;
        ID3D11ShaderResourceView* none[1] = {nullptr};
        ctx_->PSSetShaderResources(0, 1, none);  // dst may have been bound as input
        ID3D11RenderTargetView* rtv = t.rtv.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        if (clear) ctx_->ClearRenderTargetView(rtv, clear);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->RSSetState(raster_.Get());
        ID3D11Buffer* cb = cbuffer_.Get();
        ctx_->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* s = sampler_.Get();
        ctx_->PSSetSamplers(0, 1, &s);
    }

    void drawTestPattern(const Uniforms& u) override { draw(psPattern_.Get(), nullptr, u); }
    void drawTextured(TexId src, const Uniforms& u) override {
        // Blending reads the target back; skip it for opaque draws (bandwidth matters on integrated GPUs).
        draw(psTextured_.Get(), textures_.at(src).srv.Get(), u, u.fparams[3] < 0.999f || (u.iparams[3] & 2u));
    }
    void drawTexturedUYVY(TexId src, const Uniforms& u) override { draw(psUyvy_.Get(), textures_.at(src).srv.Get(), u); }

    void endPass() override { ctx_->OMSetRenderTargets(0, nullptr, nullptr); }

    void copyToReadback(TexId src, ReadbackId dst, int slot) override {
        std::shared_ptr<Readback> rb;
        {
            std::lock_guard l(m_);
            rb = readbacks_.at(dst);
        }
        ctx_->CopyResource(rb->staging.at(slot).Get(), textures_.at(src).tex.Get());
        frame_.copies.push_back({dst, slot, rb});
    }

    void endFrame() override {
        if (frame_.tsEnd) ctx_->End(frame_.tsEnd.Get());
        if (frame_.disjoint) ctx_->End(frame_.disjoint.Get());
        {
            std::lock_guard l(m_);
            if (fence_) {
                frame_.fenceValue = ++fenceValue_;
                ctx4_->Signal(fence_.Get(), frame_.fenceValue);
            } else if (frame_.done) {
                ctx_->End(frame_.done.Get());
            }
            pending_.push_back(std::move(frame_));
        }
        ctx_->Flush();
        cv_.notify_all();
    }

    // Called on the render thread by the engine.
    void releaseSlot(ReadbackId id, int slot) override {
        std::lock_guard l(m_);
        auto it = readbacks_.find(id);
        if (it == readbacks_.end() || !it->second->mapped[slot]) return;
        ctx_->Unmap(it->second->staging[slot].Get(), 0);
        it->second->mapped[slot] = false;
    }

private:
    struct Texture {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11RenderTargetView> rtv;
        int w = 0, h = 0;
    };
    // Video processor (blitVideoFrame), created for one input / output size at a time.
    ComPtr<ID3D11VideoDevice> videoDev_;
    ComPtr<ID3D11VideoContext> videoCtx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpe_;
    ComPtr<ID3D11VideoProcessor> vp_;
    UINT vpIn_[2] = {0, 0}, vpOut_[2] = {0, 0};
    DXGI_FORMAT vpFormat_ = DXGI_FORMAT_UNKNOWN;
    int vpColorSpace_ = -1;
    std::map<std::pair<void*, unsigned>, ComPtr<ID3D11VideoProcessorInputView>> videoInViews_;
    std::map<TexId, ComPtr<ID3D11VideoProcessorOutputView>> videoOutViews_;
    // HDR tone mapping (toneMapHdr).
    ComPtr<ID3D11VertexShader> vsHdr_;
    ComPtr<ID3D11PixelShader> psHdr_;
    ComPtr<ID3D11Buffer> hdrParams_;
    ComPtr<ID3D11Texture2D> hdrTex_;
    ComPtr<ID3D11ShaderResourceView> hdrLuma_, hdrChroma_;
    UINT hdrSize_[2] = {0, 0};
    bool hdrUnsupported_ = false;

    // HDR: copy the decoder's P010 surface into a texture shaders can read, then tone-map it into
    // `out` (kHdrShader). False if this GPU can't (no P010 shader access): the video processor
    // converts instead.
    bool toneMapHdr(Texture& out, ID3D11Texture2D* src, unsigned arraySlice, int colorSpace) {
        if (hdrUnsupported_) return false;
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (sd.Format != DXGI_FORMAT_P010) return false;
        if (!psHdr_) {
            UINT support = 0;
            if (FAILED(device_->CheckFormatSupport(DXGI_FORMAT_P010, &support)) || !(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
                return hdrUnsupported_ = true, false;
            try {
                std::string hs = kHdrShader;
                auto vs = compile(hs, "vs_full", "vs_5_0");
                auto ps = compile(hs, "ps_hdr", "ps_5_0");
                check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vsHdr_), "CreateVertexShader");
                check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &psHdr_), "CreatePixelShader");
                D3D11_BUFFER_DESC cb{};
                cb.ByteWidth = 16;
                cb.Usage = D3D11_USAGE_DYNAMIC;
                cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                check(device_->CreateBuffer(&cb, nullptr, &hdrParams_), "CreateBuffer");
            } catch (const std::exception&) {
                vsHdr_.Reset();
                psHdr_.Reset();
                return hdrUnsupported_ = true, false;
            }
        }
        if (!hdrTex_ || hdrSize_[0] != sd.Width || hdrSize_[1] != sd.Height) {
            hdrTex_.Reset();
            hdrLuma_.Reset();
            hdrChroma_.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = sd.Width;
            d.Height = sd.Height;
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_P010;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &hdrTex_))) return hdrUnsupported_ = true, false;
            v.Format = DXGI_FORMAT_R16_UNORM;  // plane 0: Y
            if (FAILED(device_->CreateShaderResourceView(hdrTex_.Get(), &v, &hdrLuma_))) return hdrUnsupported_ = true, false;
            v.Format = DXGI_FORMAT_R16G16_UNORM;  // plane 1: CbCr at half size
            if (FAILED(device_->CreateShaderResourceView(hdrTex_.Get(), &v, &hdrChroma_))) return hdrUnsupported_ = true, false;
            hdrSize_[0] = sd.Width;
            hdrSize_[1] = sd.Height;
        }
        ctx_->CopySubresourceRegion(hdrTex_.Get(), 0, 0, 0, 0, src, D3D11CalcSubresource(0, arraySlice, sd.MipLevels), nullptr);

        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(hdrParams_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        const uint32_t params[4] = {static_cast<uint32_t>(colorSpace), 0, 0, 0};
        std::memcpy(m.pData, params, sizeof params);
        ctx_->Unmap(hdrParams_.Get(), 0);

        ID3D11RenderTargetView* rtv = out.rtv.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        D3D11_VIEWPORT vp{0.f, 0.f, float(out.w), float(out.h), 0.f, 1.f};
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetState(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vsHdr_.Get(), nullptr, 0);
        ctx_->PSSetShader(psHdr_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {hdrLuma_.Get(), hdrChroma_.Get()};
        ctx_->PSSetShaderResources(0, 2, srvs);
        ID3D11Buffer* cb = hdrParams_.Get();
        ctx_->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* smp = sampler_.Get();
        ctx_->PSSetSamplers(0, 1, &smp);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        ctx_->PSSetShaderResources(0, 2, none);
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        return true;
    }

    struct Readback {
        int w = 0, h = 0;
        std::vector<ComPtr<ID3D11Texture2D>> staging;
        std::vector<bool> mapped;
    };
    struct Copy {
        ReadbackId rb;
        int slot;
        std::shared_ptr<Readback> target;
    };
    struct Frame {
        uint64_t token = 0, fenceValue = 0;
        ComPtr<ID3D11Query> disjoint, tsBegin, tsEnd, done;
        std::vector<Copy> copies;
    };

    void draw(ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv, const Uniforms& u, bool blend = false) {
        LONG x0 = std::max(0L, static_cast<LONG>(u.dstRect[0]));
        LONG y0 = std::max(0L, static_cast<LONG>(u.dstRect[1]));
        LONG x1 = std::min(static_cast<LONG>(passW_), static_cast<LONG>(u.dstRect[0] + u.dstRect[2]));
        LONG y1 = std::min(static_cast<LONG>(passH_), static_cast<LONG>(u.dstRect[1] + u.dstRect[3]));
        if (x1 <= x0 || y1 <= y0) return;
        D3D11_VIEWPORT vp{u.dstRect[0], u.dstRect[1], u.dstRect[2], u.dstRect[3], 0.f, 1.f};
        ctx_->RSSetViewports(1, &vp);
        D3D11_RECT sc{x0, y0, x1, y1};
        ctx_->RSSetScissorRects(1, &sc);
        D3D11_MAPPED_SUBRESOURCE m{};
        check(ctx_->Map(cbuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map(cbuffer)");
        std::memcpy(m.pData, &u, sizeof(Uniforms));
        ctx_->Unmap(cbuffer_.Get(), 0);
        ctx_->OMSetBlendState(blend ? blendOver_.Get() : nullptr, nullptr, 0xffffffff);
        ctx_->PSSetShader(ps, nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, &srv);
        ctx_->Draw(3, 0);
    }

    // Maps a finished frame's staging textures and reports them. Caller holds no lock.
    void deliver(Frame& f) {
        for (auto& c : f.copies) {
            D3D11_MAPPED_SUBRESOURCE m{};
            {
                std::lock_guard l(m_);
                if (!readbacks_.count(c.rb)) continue;  // destroyed meanwhile
                if (FAILED(ctx_->Map(c.target->staging[c.slot].Get(), 0, D3D11_MAP_READ, 0, &m))) continue;
                c.target->mapped[c.slot] = true;
            }
            if (onReadback)
                onReadback(c.rb, c.slot, ReadbackView{static_cast<const uint8_t*>(m.pData), static_cast<int>(m.RowPitch), c.target->w, c.target->h}, f.token);
        }
        GpuFrameStats st;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        if (f.disjoint && ctx_->GetData(f.disjoint.Get(), &dj, sizeof dj, 0) == S_OK && !dj.Disjoint &&
            ctx_->GetData(f.tsBegin.Get(), &t0, sizeof t0, 0) == S_OK && ctx_->GetData(f.tsEnd.Get(), &t1, sizeof t1, 0) == S_OK &&
            dj.Frequency)
            st.gpuMs = double(t1 - t0) * 1000.0 / double(dj.Frequency);
        if (onFrameComplete) onFrameComplete(f.token, st);
    }

    void completionLoop() {
        for (;;) {
            Frame f;
            {
                std::unique_lock l(m_);
                cv_.wait(l, [this] { return quit_ || !pending_.empty(); });
                if (quit_) return;
                uint64_t value = pending_.front().fenceValue;
                if (fence_->GetCompletedValue() < value) {
                    fence_->SetEventOnCompletion(value, fenceEvent_);
                    l.unlock();
                    WaitForSingleObject(fenceEvent_, 100);
                    continue;
                }
            }
            {
                std::lock_guard l(m_);
                f = std::move(pending_.front());
            }
            deliver(f);
            {
                std::lock_guard l(m_);
                pending_.pop_front();
            }
            cv_.notify_all();
        }
    }

    // Fallback without fences: deliver frames whose EVENT query has completed.
    void pollCompleted(bool block) {
        while (!pending_.empty()) {
            Frame& f = pending_.front();
            BOOL done = FALSE;
            HRESULT hr = ctx_->GetData(f.done.Get(), &done, sizeof done, 0);
            if (hr != S_OK || !done) {
                if (!block) return;
                std::this_thread::yield();
                continue;
            }
            Frame fr = std::move(f);
            pending_.pop_front();
            deliver(fr);
            if (block) return;
        }
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11DeviceContext4> ctx4_;
    ComPtr<ID3D11Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    uint64_t fenceValue_ = 0;
    std::string deviceName_ = "unknown adapter";

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psPattern_, psTextured_, psUyvy_;
    ComPtr<ID3D11Buffer> cbuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11BlendState> blendOver_;

    std::unordered_map<int, Texture> textures_;               // render thread only
    struct Display {
        std::string monitor;
        HWND hwnd = nullptr;
        ComPtr<IDXGISwapChain2> swap;
        ComPtr<IDXGIOutput> output;
        HANDLE waitable = nullptr;
        TexId target = 0;
        int w = 0, h = 0;
        double hz = 60;
        bool ready = false;  // displayTarget() took a frame slot: present() must follow
        uint64_t presents = 0, busy = 0, repeats = 0;
        bool seen = false;
        UINT lastPresent = 0, lastRefresh = 0;
        std::string error;
    };
    std::map<DisplayId, Display> displays_;  // render thread only
    DisplayId nextDisplay_ = 1;
    std::chrono::steady_clock::time_point raisedAt_{};
    // The display windows belong to the render thread: handle their messages, and put them back
    // on top once a second (the taskbar or a notification can come up over them).
    void pumpDisplayMessages() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        auto now = std::chrono::steady_clock::now();
        if (now - raisedAt_ < std::chrono::seconds(1)) return;
        raisedAt_ = now;
        for (auto& [id, d] : displays_)
            SetWindowPos(d.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    std::unordered_map<int, std::shared_ptr<Readback>> readbacks_;  // guarded by m_
    std::atomic<int> nextId_{1};

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Frame> pending_;
    bool quit_ = false;
    std::thread completionThread_;

    Frame frame_;
    int passW_ = 0, passH_ = 0;
    GpuUtilization util_;
};

}  // namespace

std::vector<GpuInfo> listGpus() {
    std::vector<GpuInfo> out;
    auto all = hardwareAdapters();
    for (size_t i = 0; i < all.size(); ++i) {
        DXGI_ADAPTER_DESC1 d{};
        all[i]->GetDesc1(&d);
        GpuInfo g;
        g.index = static_cast<int>(i);
        g.name = narrow(d.Description);
        g.memoryMB = d.DedicatedVideoMemory / (1024 * 1024);
        ComPtr<IDXGIOutput> o;
        for (UINT k = 0; all[i]->EnumOutputs(k, &o) != DXGI_ERROR_NOT_FOUND; ++k, o.Reset()) {
            DXGI_OUTPUT_DESC od{};
            if (FAILED(o->GetDesc(&od)) || !od.AttachedToDesktop) continue;
            GpuOutputInfo m;
            m.name = narrow(od.DeviceName);
            m.x = od.DesktopCoordinates.left;
            m.y = od.DesktopCoordinates.top;
            m.w = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
            m.h = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
            m.primary = m.x == 0 && m.y == 0;
            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) m.refreshHz = dm.dmDisplayFrequency;
            g.outputs.push_back(m);
        }
        out.push_back(g);
    }
    return out;
}

std::unique_ptr<Renderer> createRenderer(const RendererOptions& options, std::string& error) {
    try {
        return std::make_unique<D3D11Renderer>(options.softwareGpu, options.gpu);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

}  // namespace mosaic
