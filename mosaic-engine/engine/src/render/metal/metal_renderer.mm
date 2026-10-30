// Metal backend (macOS). Used for development and as a reference for the
// Direct3D 11 backend; both share render/shaders.hpp.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "render/renderer.hpp"
#include "render/shaders.hpp"

// The render loop is a plain std::thread with no autorelease pool, so each
// frame opens one (per-frame Metal objects are autoreleased).
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);

namespace mosaic {

namespace {

constexpr int kMaxFramesInFlight = 2;

class MetalRenderer final : public Renderer {
public:
    MetalRenderer() {
        device_ = MTLCreateSystemDefaultDevice();
        if (!device_) throw std::runtime_error("no Metal device");
        queue_ = [device_ newCommandQueue];
        inflight_ = dispatch_semaphore_create(kMaxFramesInFlight);

        std::string src = std::string(shaders::kMetalPrelude) + shaders::kShaderCommon + shaders::kMetalEntryPoints;
        NSError* err = nil;
        MTLCompileOptions* opts = [MTLCompileOptions new];
        opts.mathMode = MTLMathModeSafe;
        id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:opts error:&err];
        if (!lib) throw std::runtime_error(std::string("Metal shader compile failed: ") + err.localizedDescription.UTF8String);

        patternPso_ = makePipeline(lib, @"ps_pattern");
        texturedPso_ = makePipeline(lib, @"ps_textured");
        texturedBlendPso_ = makePipeline(lib, @"ps_textured", true);
        uyvyPso_ = makePipeline(lib, @"ps_textured_uyvy");

        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler_ = [device_ newSamplerStateWithDescriptor:sd];
    }

    ~MetalRenderer() override { waitIdle(); }

    std::string backendName() const override { return "Metal"; }
    std::string deviceName() const override { return device_.name.UTF8String; }

    double gpuUtilization() override {
        // Apple GPUs publish "Device Utilization %" in the IOAccelerator
        // PerformanceStatistics; readable without privileges. Cached for 1 s.
        auto now = std::chrono::steady_clock::now();
        if (now - utilAt_ < std::chrono::seconds(1)) return util_;
        utilAt_ = now;
        util_ = -1;
        io_iterator_t it = 0;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return util_;
        io_registry_entry_t entry;
        while ((entry = IOIteratorNext(it)) != 0) {
            CFMutableDictionaryRef props = nullptr;
            if (IORegistryEntryCreateCFProperties(entry, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
                auto perf = static_cast<CFDictionaryRef>(CFDictionaryGetValue(props, CFSTR("PerformanceStatistics")));
                if (perf && CFGetTypeID(perf) == CFDictionaryGetTypeID()) {
                    auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(perf, CFSTR("Device Utilization %")));
                    long long v = 0;
                    if (n && CFNumberGetValue(n, kCFNumberSInt64Type, &v)) util_ = static_cast<double>(v);
                }
                CFRelease(props);
            }
            IOObjectRelease(entry);
            if (util_ >= 0) break;
        }
        IOObjectRelease(it);
        return util_;
    }

    TexId createTexture(int w, int h, TexUsage usage) override {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:w height:h mipmapped:NO];
        if (usage == TexUsage::RenderTarget) {
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModePrivate;
        } else {
            d.usage = MTLTextureUsageShaderRead;
            d.storageMode = device_.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
        }
        id<MTLTexture> t = [device_ newTextureWithDescriptor:d];
        if (!t) throw std::runtime_error("texture allocation failed");
        TexId id = nextId_++;
        textures_[id] = t;
        return id;
    }

    void destroyTexture(TexId id) override { textures_.erase(id); }

    ReadbackId createReadback(int w, int h, int slots) override {
        Readback rb;
        rb.w = w;
        rb.h = h;
        for (int i = 0; i < slots; ++i) {
            // Shared storage: on Apple silicon this is the same memory the GPU
            // wrote, so the sink reads it with no extra copy.
            id<MTLBuffer> b = [device_ newBufferWithLength:static_cast<NSUInteger>(w) * h * 4 options:MTLResourceStorageModeShared];
            if (!b) throw std::runtime_error("readback allocation failed");
            rb.buffers.push_back(b);
        }
        ReadbackId id = nextId_++;
        readbacks_[id] = rb;
        return id;
    }

    void destroyReadback(ReadbackId id) override { readbacks_.erase(id); }

    void waitIdle() override {
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
        // Let every outstanding completion handler finish.
        for (int i = 0; i < kMaxFramesInFlight; ++i) dispatch_semaphore_wait(inflight_, DISPATCH_TIME_FOREVER);
        for (int i = 0; i < kMaxFramesInFlight; ++i) dispatch_semaphore_signal(inflight_);
    }

    void beginFrame(uint64_t token) override {
        dispatch_semaphore_wait(inflight_, DISPATCH_TIME_FOREVER);
        pool_ = objc_autoreleasePoolPush();
        token_ = token;
        cb_ = [queue_ commandBuffer];
        copies_.clear();
    }

    void uploadBGRA(TexId dst, const uint8_t* data, int stride) override {
        id<MTLTexture> t = textures_.at(dst);
        [t replaceRegion:MTLRegionMake2D(0, 0, t.width, t.height) mipmapLevel:0 withBytes:data bytesPerRow:stride];
    }

    void beginPass(TexId dst, const float* clear) override {
        endBlit();
        id<MTLTexture> t = textures_.at(dst);
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (clear) {
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
        } else {
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        }
        enc_ = [cb_ renderCommandEncoderWithDescriptor:rp];
        passW_ = static_cast<int>(t.width);
        passH_ = static_cast<int>(t.height);
    }

    void drawTestPattern(const Uniforms& u) override { draw(patternPso_, nil, u); }

    void drawTextured(TexId src, const Uniforms& u) override {
        // Blending reads the target back; skip it for opaque draws (bandwidth matters on integrated GPUs).
        draw(u.fparams[3] < 0.999f ? texturedBlendPso_ : texturedPso_, textures_.at(src), u);
    }
    void drawTexturedUYVY(TexId src, const Uniforms& u) override { draw(uyvyPso_, textures_.at(src), u); }

    void endPass() override {
        [enc_ endEncoding];
        enc_ = nil;
    }

    void copyToReadback(TexId src, ReadbackId dst, int slot) override {
        if (!blit_) blit_ = [cb_ blitCommandEncoder];
        id<MTLTexture> t = textures_.at(src);
        Readback& rb = readbacks_.at(dst);
        id<MTLBuffer> buf = rb.buffers.at(slot);
        NSUInteger rowBytes = static_cast<NSUInteger>(rb.w) * 4;
        [blit_ copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(rb.w, rb.h, 1) toBuffer:buf destinationOffset:0
        destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * rb.h];
        copies_.push_back({dst, slot, ReadbackView{static_cast<const uint8_t*>(buf.contents), static_cast<int>(rowBytes), rb.w, rb.h}});
    }

    void endFrame() override {
        endBlit();
        auto copies = copies_;
        uint64_t token = token_;
        dispatch_semaphore_t sem = inflight_;
        [cb_ addCompletedHandler:^(id<MTLCommandBuffer> done) {
            if (onReadback)
                for (auto& c : copies) onReadback(c.rb, c.slot, c.view, token);
            GpuFrameStats st;
            st.gpuMs = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
            if (onFrameComplete) onFrameComplete(token, st);
            dispatch_semaphore_signal(sem);
        }];
        [cb_ commit];
        cb_ = nil;
        objc_autoreleasePoolPop(pool_);
        pool_ = nullptr;
    }

    void releaseSlot(ReadbackId, int) override {}  // shared buffers need no unmapping

private:
    struct Readback {
        int w = 0, h = 0;
        std::vector<id<MTLBuffer>> buffers;
    };
    struct Copy {
        ReadbackId rb;
        int slot;
        ReadbackView view;
    };

    id<MTLRenderPipelineState> makePipeline(id<MTLLibrary> lib, NSString* fragment, bool blend = false) {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vs_main"];
        d.fragmentFunction = [lib newFunctionWithName:fragment];
        auto* ca = d.colorAttachments[0];
        ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (blend) {
            // Straight alpha "over": opaque draws (alpha 1) simply replace the target.
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
            ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        NSError* err = nil;
        id<MTLRenderPipelineState> pso = [device_ newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pso) throw std::runtime_error(std::string("pipeline creation failed: ") + err.localizedDescription.UTF8String);
        return pso;
    }

    void draw(id<MTLRenderPipelineState> pso, id<MTLTexture> src, const Uniforms& u) {
        // Clip the viewport rectangle to the target for the scissor.
        int x0 = std::max(0, static_cast<int>(u.dstRect[0]));
        int y0 = std::max(0, static_cast<int>(u.dstRect[1]));
        int x1 = std::min(passW_, static_cast<int>(u.dstRect[0] + u.dstRect[2]));
        int y1 = std::min(passH_, static_cast<int>(u.dstRect[1] + u.dstRect[3]));
        if (x1 <= x0 || y1 <= y0) return;
        [enc_ setRenderPipelineState:pso];
        [enc_ setViewport:(MTLViewport){u.dstRect[0], u.dstRect[1], u.dstRect[2], u.dstRect[3], 0.0, 1.0}];
        [enc_ setScissorRect:(MTLScissorRect){static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0),
                                              static_cast<NSUInteger>(x1 - x0), static_cast<NSUInteger>(y1 - y0)}];
        [enc_ setFragmentBytes:&u length:sizeof(Uniforms) atIndex:0];
        if (src) {
            [enc_ setFragmentTexture:src atIndex:0];
            [enc_ setFragmentSamplerState:sampler_ atIndex:0];
        }
        [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }

    void endBlit() {
        if (blit_) {
            [blit_ endEncoding];
            blit_ = nil;
        }
    }

    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLRenderPipelineState> patternPso_, texturedPso_, texturedBlendPso_, uyvyPso_;
    id<MTLSamplerState> sampler_;
    dispatch_semaphore_t inflight_;

    std::unordered_map<int, id<MTLTexture>> textures_;
    std::unordered_map<int, Readback> readbacks_;
    int nextId_ = 1;

    id<MTLCommandBuffer> cb_;
    id<MTLRenderCommandEncoder> enc_;
    id<MTLBlitCommandEncoder> blit_;
    int passW_ = 0, passH_ = 0;
    uint64_t token_ = 0;
    std::vector<Copy> copies_;
    void* pool_ = nullptr;

    std::chrono::steady_clock::time_point utilAt_{};
    double util_ = -1;
};

}  // namespace

std::unique_ptr<Renderer> createRenderer(const RendererOptions&, std::string& error) {
    try {
        @autoreleasepool {
            return std::make_unique<MetalRenderer>();
        }
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

}  // namespace mosaic
