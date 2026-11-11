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
using DisplayId = int;

// A display output: what reached the monitor.
struct DisplayStats {
    std::string monitor;
    int width = 0, height = 0;  // the monitor's mode (the size the output is drawn at)
    double refreshHz = 0;
    uint64_t presents = 0;      // frames handed to it
    uint64_t busy = 0;          // frames skipped because it hadn't shown the previous one yet
    uint64_t repeats = 0;       // refreshes that showed the previous frame again (normal below the refresh rate)
    std::string error;
};

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
    float xform[4];    // cos, sin of the crop's free angle (clockwise about its centre), unused, unused
    float grade0[4];   // colour correction when flags bit 4 is set: red, green, blue gain, brightness
    float grade1[4];   // contrast, saturation, gamma, unused
};
static_assert(sizeof(Uniforms) == 144, "Uniforms layout must match shaders");

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

    // ---- video files (render thread) ----
    // The native device (ID3D11Device* on Windows), so a hardware video decoder can use the same GPU.
    virtual void* nativeDevice() { return nullptr; }

    // ---- display outputs: full screen on a monitor connected to this GPU (render thread) ----
    // Opens a borderless full-screen window on the monitor ("\\.\DISPLAY3", see listGpus()). 0 and
    // `error` set if it can't (no such monitor, it's on another GPU, not supported here).
    virtual DisplayId createDisplay(const std::string& monitor, std::string& error) {
        error = "display outputs need Windows (Direct3D 11)";
        (void)monitor;
        return 0;
    }
    virtual void destroyDisplay(DisplayId) {}
    // The display's back buffer as a render target (draw the output into it with beginPass), 0 if
    // it has none. Its size is DisplayStats::width x height.
    virtual TexId displayTarget(DisplayId) { return 0; }
    // Shows what was drawn at the monitor's next refresh. Never waits: if the monitor hasn't shown
    // the previous frame yet, this one is skipped (counted as busy).
    virtual void presentDisplay(DisplayId) {}
    virtual DisplayStats displayStats(DisplayId) { return {}; }
    // Blocks until the display's monitor's next vertical blank (paces a canvas that drives
    // monitors). False if it can't (then the caller keeps its own timing).
    virtual bool waitDisplayVBlank(DisplayId) { return false; }
    // Converts a decoded NV12 / P010 frame (the decoder's texture and array slice, on nativeDevice())
    // into `dst`, a RenderTarget texture, on the GPU's video processor. colorSpace: 0 = BT.709,
    // 1 = BT.2020 PQ (HDR10), 2 = BT.2020 HLG; HDR is mapped to SDR. False if the backend can't.
    virtual bool blitVideoFrame(TexId dst, void* nativeYuvTexture, unsigned arraySlice, int colorSpace = 0) { return false; }

    // Marks a slot reusable. Thread-safe; the backend may defer the work
    // (D3D11 unmaps on the render thread at the next beginFrame).
    virtual void releaseSlot(ReadbackId, int slot) = 0;
};

struct RendererOptions {
    // Windows: use WARP (the CPU rasterizer that ships with Windows) instead of
    // a hardware GPU. For CI machines without a GPU; correctness only, not speed.
    bool softwareGpu = false;
    // Which GPU: an index into listGpus(); -1 = the system's default one.
    int gpu = -1;
};

std::unique_ptr<Renderer> createRenderer(const RendererOptions& options, std::string& error);

// A monitor connected to a GPU.
struct GpuOutputInfo {
    std::string name;            // Windows display device ("\\.\DISPLAY3")
    int x = 0, y = 0, w = 0, h = 0;  // desktop rectangle, physical pixels
    double refreshHz = 0;
    bool primary = false;
};

// A GPU this PC has (hardware only), in the order createRenderer() numbers them.
struct GpuInfo {
    int index = 0;
    std::string name;
    uint64_t memoryMB = 0;
    std::vector<GpuOutputInfo> outputs;
};
std::vector<GpuInfo> listGpus();

}  // namespace mosaic
