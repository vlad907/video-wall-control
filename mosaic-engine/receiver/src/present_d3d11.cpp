// Direct3D 11 presenter: a borderless, topmost window per monitor, each with a flip-model
// swap chain (so DWM doesn't add a frame) and a frame-latency waitable object, used to pace
// the render loop to the displays. One device draws every window; all are presented together.
#include "presenter.hpp"

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstring>
#include <map>
#include <thread>

namespace mosaic::rx {

using Microsoft::WRL::ComPtr;

namespace {

// A full-window triangle; the viewport is the tile, so the picture is stretched to it.
const char* kShader = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(V i) : SV_Target { return float4(tex.Sample(smp, i.uv).rgb, 1); }

// UYVY 4:2:2 packed as one RGBA8 texel per pixel pair (r = U, g = Y0, b = V, a = Y1), BT.709 limited range.
cbuffer Picture : register(b0) { float2 size; float2 pad; };  // picture size in pixels
float4 ps_uyvy(V i) : SV_Target {
    float2 p = min(floor(i.uv * size), size - 1);
    float4 t = tex.Load(int3(int(p.x) / 2, int(p.y), 0));
    float y = ((int(p.x) & 1) ? t.a : t.g) * 255.0 - 16.0;
    float u = t.r * 255.0 - 128.0, v = t.b * 255.0 - 128.0;
    float yy = y / 219.0, cb = u / 224.0, cr = v / 224.0;
    return float4(saturate(float3(yy + 1.5748 * cr, yy - 0.1873 * cb - 0.4681 * cr, yy + 1.8556 * cb)), 1);
}
)";

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_SETCURSOR:
            SetCursor(nullptr);  // no pointer over the wall
            return TRUE;
        case WM_CLOSE:
            return 0;  // only the receiver closes its windows
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_DPICHANGED:
            return 0;  // stay exactly on the monitor rectangle we were given (physical pixels)
    }
    return DefWindowProcW(h, msg, w, l);
}

// Windows scales the windows of programs that don't say they handle display scaling, so on a
// 4K monitor at 200 % a 3840 x 2160 window would come out twice as big (showing a quarter of
// the picture). The dashboard places windows in physical pixels, so the receiver uses them too.
void usePhysicalPixels() {
    using SetContext = BOOL(WINAPI*)(HANDLE);
    auto fn = reinterpret_cast<SetContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (!fn || !fn(reinterpret_cast<HANDLE>(-4)))  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (Windows 10 1703+)
        SetProcessDPIAware();
}

class D3D11Presenter final : public Presenter {
public:
    bool init(bool software, std::string& error) {
        usePhysicalPixels();
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(nullptr, software ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1,
                                       D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
        if (FAILED(hr)) return error = "D3D11CreateDevice failed", false;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        device_.As(&dxgiDevice);
        dxgiDevice->GetAdapter(&adapter);
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory_)))) return error = "no DXGI factory", false;

        ComPtr<ID3DBlob> vsb, psb, err;
        if (FAILED(D3DCompile(kShader, std::strlen(kShader), "receiver", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
            FAILED(D3DCompile(kShader, std::strlen(kShader), "receiver", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err)))
            return error = "shader compile failed", false;
        device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
        device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
        ComPtr<ID3DBlob> uyb;
        if (FAILED(D3DCompile(kShader, std::strlen(kShader), "receiver", nullptr, nullptr, "ps_uyvy", "ps_5_0", 0, 0, &uyb, &err)))
            return error = "UYVY shader compile failed", false;
        device_->CreatePixelShader(uyb->GetBufferPointer(), uyb->GetBufferSize(), nullptr, &psUyvy_);
        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = 16;
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device_->CreateBuffer(&cb, nullptr, &picture_);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device_->CreateSamplerState(&sd, &sampler_);

        WNDCLASSW wc = {};
        wc.lpfnWndProc = wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MosaicReceiverWindow";
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassW(&wc);
        return true;
    }

    ~D3D11Presenter() override { destroyWindows(); }

    std::string name() const override { return "Direct3D 11"; }

    bool configure(const std::vector<WindowSpec>& specs, std::string& error) override {
        destroyWindows();
        for (auto& s : specs) {
            Win w;
            w.spec = s;
            w.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"MosaicReceiverWindow", L"Mosaic Receiver",
                                     WS_POPUP | WS_VISIBLE, s.x, s.y, s.w, s.h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!w.hwnd) return error = "cannot create a window", false;
            DXGI_SWAP_CHAIN_DESC1 d = {};
            d.Width = static_cast<UINT>(s.w);
            d.Height = static_cast<UINT>(s.h);
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            d.BufferCount = 2;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            ComPtr<IDXGISwapChain1> sc1;
            if (FAILED(factory_->CreateSwapChainForHwnd(device_.Get(), w.hwnd, &d, nullptr, nullptr, &sc1)) || FAILED(sc1.As(&w.swap))) {
                DestroyWindow(w.hwnd);
                return error = "cannot create a swap chain", false;
            }
            factory_->MakeWindowAssociation(w.hwnd, DXGI_MWA_NO_ALT_ENTER);
            w.swap->SetMaximumFrameLatency(1);
            w.waitable = w.swap->GetFrameLatencyWaitableObject();
            ComPtr<ID3D11Texture2D> back;
            w.swap->GetBuffer(0, IID_PPV_ARGS(&back));
            device_->CreateRenderTargetView(back.Get(), nullptr, &w.rtv);
            windows_.push_back(std::move(w));
        }
        hz_ = queryRefreshHz();
        return true;
    }

    void upload(int stream, const RxFrame& f) override {
        auto& t = textures_[stream];
        if (!t.tex || t.w != f.width || t.h != f.height || t.uyvy != f.uyvy) {
            D3D11_TEXTURE2D_DESC d = {};
            // UYVY: half as wide, one RGBA8 texel per pixel pair; converted in ps_uyvy.
            d.Width = static_cast<UINT>(f.uyvy ? (f.width + 1) / 2 : f.width);
            d.Height = static_cast<UINT>(f.height);
            d.MipLevels = d.ArraySize = 1;
            d.Format = f.uyvy ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            t = {};
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &t.tex))) return;
            device_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
            t.w = f.width;
            t.h = f.height;
            t.uyvy = f.uyvy;
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx_->Map(t.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        const size_t row = static_cast<size_t>(f.width) * (f.uyvy ? 2 : 4);
        for (int y = 0; y < f.height; ++y)
            std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, f.data + static_cast<size_t>(y) * f.stride, row);
        ctx_->Unmap(t.tex.Get(), 0);
    }

    void present(const std::vector<TileDraw>& tiles, const std::vector<bool>& flash) override {
        const float black[4] = {0, 0, 0, 1};
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        for (size_t i = 0; i < windows_.size(); ++i) {
            auto& w = windows_[i];
            ctx_->OMSetRenderTargets(1, w.rtv.GetAddressOf(), nullptr);
            if (i < flash.size() && flash[i]) {
                const float white[4] = {1, 1, 1, 1};
                ctx_->ClearRenderTargetView(w.rtv.Get(), white);
                continue;
            }
            ctx_->ClearRenderTargetView(w.rtv.Get(), black);
            for (auto& t : tiles) {
                auto tex = textures_.find(t.stream);
                if (t.window != static_cast<int>(i) || tex == textures_.end() || !tex->second.srv) continue;
                D3D11_VIEWPORT vp = {float(t.x), float(t.y), float(t.w), float(t.h), 0, 1};
                ctx_->RSSetViewports(1, &vp);
                if (tex->second.uyvy) {
                    D3D11_MAPPED_SUBRESOURCE m;
                    if (SUCCEEDED(ctx_->Map(picture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                        float size[4] = {float(tex->second.w), float(tex->second.h), 0, 0};
                        std::memcpy(m.pData, size, sizeof size);
                        ctx_->Unmap(picture_.Get(), 0);
                    }
                    ctx_->PSSetShader(psUyvy_.Get(), nullptr, 0);
                    ctx_->PSSetConstantBuffers(0, 1, picture_.GetAddressOf());
                } else {
                    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
                }
                ctx_->PSSetShaderResources(0, 1, tex->second.srv.GetAddressOf());
                ctx_->Draw(3, 0);
            }
        }
        // Every window in the same pass: each flips on its own monitor's next refresh.
        for (auto& w : windows_) w.swap->Present(1, 0);
        // Then ask each swap chain which present reached its monitor, and when.
        timings_.assign(windows_.size(), {});
        for (size_t i = 0; i < windows_.size(); ++i) {
            auto& t = timings_[i];
            UINT last = 0;
            windows_[i].swap->GetLastPresentCount(&last);
            t.presentCount = last;
            DXGI_FRAME_STATISTICS st = {};
            if (SUCCEEDED(windows_[i].swap->GetFrameStatistics(&st)) && st.SyncQPCTime.QuadPart) {
                t.valid = true;
                t.syncPresentCount = st.PresentCount;
                t.syncRefreshCount = st.SyncRefreshCount;
                t.syncMicros = static_cast<int64_t>(static_cast<double>(st.SyncQPCTime.QuadPart) * 1e6 / static_cast<double>(qpcFreq_));
            }
        }
    }

    std::vector<PresentTiming> timings() override { return timings_; }

    void waitForRefresh() override {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (windows_.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            return;
        }
        // Ready when every swap chain can take a frame, i.e. after the latest monitor's refresh.
        std::vector<HANDLE> handles;
        for (auto& w : windows_) handles.push_back(w.waitable);
        WaitForMultipleObjectsEx(static_cast<DWORD>(handles.size()), handles.data(), TRUE, 1000, TRUE);
    }

    double refreshHz() const override { return hz_; }

private:
    struct Win {
        WindowSpec spec;
        HWND hwnd = nullptr;
        ComPtr<IDXGISwapChain2> swap;
        ComPtr<ID3D11RenderTargetView> rtv;
        HANDLE waitable = nullptr;
    };
    struct Tex {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        int w = 0, h = 0;  // picture size in pixels
        bool uyvy = false;
    };

    double queryRefreshHz() const {
        if (windows_.empty()) return 60.0;
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(windows_[0].hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        DEVMODEW dm = {};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return dm.dmDisplayFrequency;
        return 60.0;
    }

    void destroyWindows() {
        if (ctx_) ctx_->ClearState();
        for (auto& w : windows_) {
            if (w.waitable) CloseHandle(w.waitable);
            w.rtv.Reset();
            w.swap.Reset();
            if (w.hwnd) DestroyWindow(w.hwnd);
        }
        windows_.clear();
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_, psUyvy_;
    ComPtr<ID3D11Buffer> picture_;
    ComPtr<ID3D11SamplerState> sampler_;
    std::vector<Win> windows_;
    std::vector<PresentTiming> timings_;
    int64_t qpcFreq_ = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<int64_t>(f.QuadPart);
    }();
    std::map<int, Tex> textures_;
    double hz_ = 60.0;
};

}  // namespace

std::unique_ptr<Presenter> createD3D11Presenter(bool softwareGpu, std::string& error) {
    auto p = std::make_unique<D3D11Presenter>();
    if (!p->init(softwareGpu, error)) return nullptr;
    return p;
}

}  // namespace mosaic::rx
