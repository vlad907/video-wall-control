// Backend-neutral GPU interface. One instance is driven by the render thread;
// only releaseSlot() and the stats getters may be called from other threads.
//
// All textures are BGRA8 (matching NDI's BGRA/BGRX FourCC), so a readback
// buffer can be handed to the NDI SDK without any CPU-side conversion.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mosaic {

using TexId = int;
using ReadbackId = int;

enum class TexUsage {
    RenderTarget,  // GPU-only, rendered to and sampled
    Upload,        // CPU-writable (NDI input frames), sampled
};

// Must match the `Uniforms` struct in shaders.hpp (std140-like: all 16-byte vectors).
struct alignas(16) Uniforms {
    float dstRect[4];  // x, y, w, h of the viewport in target pixels
    float crop[4];     // x, y, w, h of the sampled region in source pixels
    float sizes[4];    // source width, source height, dst width, dst height
    float bg[4];       // background / letterbox colour (RGBA)
    float fparams[4];  // time (s), tile width, tile height, opacity (drawTextured; blended over the target)
    uint32_t iparams[4];  // frame id, rotation (deg), scale mode, flags
};
static_assert(sizeof(Uniforms) == 96, "Uniforms layout must match shaders");

// A finished GPU->CPU copy. `data` stays valid until releaseSlot() for it.
struct ReadbackView {
    const uint8_t* data = nullptr;
    int stride = 0;  // bytes per row (may be padded on D3D11)
    int width = 0, height = 0;
};

struct GpuFrameStats {
    double gpuMs = 0;  // GPU execution time of the frame (0 if unavailable)
};

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual std::string backendName() const = 0;
    virtual std::string deviceName() const = 0;
    // 0..100, or -1 if the platform doesn't expose it.
    virtual double gpuUtilization() { return -1; }

    // ---- resources (render thread) ----
    virtual TexId createTexture(int w, int h, TexUsage usage) = 0;
    virtual void destroyTexture(TexId) = 0;
    // A ring of `slots` CPU-readable buffers of w x h BGRA.
    virtual ReadbackId createReadback(int w, int h, int slots) = 0;
    virtual void destroyReadback(ReadbackId) = 0;
    // Blocks until all submitted GPU work has completed.
    virtual void waitIdle() = 0;

    // ---- callbacks (set once before the first frame) ----
    // Invoked when a readback slot's copy has completed. May run on a backend
    // thread (Metal) or the render thread (D3D11).
    std::function<void(ReadbackId, int slot, const ReadbackView&, uint64_t frameToken)> onReadback;
    std::function<void(uint64_t frameToken, const GpuFrameStats&)> onFrameComplete;

    // ---- per frame (render thread) ----
    // Waits until fewer than maxFramesInFlight frames are on the GPU.
    virtual void beginFrame(uint64_t frameToken) = 0;
    virtual void uploadBGRA(TexId dst, const uint8_t* data, int stride) = 0;
    // A pass renders into one texture; clearRGBA == nullptr keeps its contents.
    virtual void beginPass(TexId dst, const float* clearRGBA) = 0;
    virtual void drawTestPattern(const Uniforms&) = 0;
    virtual void drawTextured(TexId src, const Uniforms&) = 0;
    // Like drawTextured for an output of sizes[2] x sizes[3] pixels, but writes it
    // packed as UYVY 4:2:2 into the pass target, which is half as wide (one pass, no BGRA copy).
    virtual void drawTexturedUYVY(TexId src, const Uniforms&) = 0;
    virtual void endPass() = 0;
    virtual void copyToReadback(TexId src, ReadbackId dst, int slot) = 0;
    virtual void endFrame() = 0;

    // Marks a slot reusable. Thread-safe; the backend may defer the work
    // (D3D11 unmaps on the render thread at the next beginFrame).
    virtual void releaseSlot(ReadbackId, int slot) = 0;
};

struct RendererOptions {
    // Windows: use WARP (the CPU rasterizer that ships with Windows) instead of
    // a hardware GPU. For CI machines without a GPU; correctness only, not speed.
    bool softwareGpu = false;
};

std::unique_ptr<Renderer> createRenderer(const RendererOptions& options, std::string& error);

}  // namespace mosaic
