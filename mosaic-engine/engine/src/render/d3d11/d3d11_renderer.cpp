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
#include <dxgi1_2.h>
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

class D3D11Renderer final : public Renderer {
public:
    explicit D3D11Renderer(bool warp) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                &device_, nullptr, &ctx_),
              "D3D11CreateDevice");

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
        auto psUyvy = compile(src, "ps_uyvy", "ps_5_0");
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
    }

    ~D3D11Renderer() override {
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

    void destroyTexture(TexId id) override { textures_.erase(id); }

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
    void drawTextured(TexId src, const Uniforms& u) override { draw(psTextured_.Get(), textures_.at(src).srv.Get(), u); }
    void drawUYVY(TexId src, const Uniforms& u) override { draw(psUyvy_.Get(), textures_.at(src).srv.Get(), u); }

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

    void draw(ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv, const Uniforms& u) {
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

    std::unordered_map<int, Texture> textures_;               // render thread only
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

std::unique_ptr<Renderer> createRenderer(const RendererOptions& options, std::string& error) {
    try {
        return std::make_unique<D3D11Renderer>(options.softwareGpu);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

}  // namespace mosaic
