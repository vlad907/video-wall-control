// The render engine: paces frames, keeps GPU resources in step with the
// active layout, renders sources -> canvas -> outputs, and feeds the output
// channels. Everything GPU-related runs on the single render thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/layout_store.hpp"
#include "core/metrics.hpp"
#include "core/output_channel.hpp"
#include "io/video_io.hpp"
#include "render/renderer.hpp"

namespace mosaic {

class Engine {
public:
    Engine(Renderer& renderer, LayoutStore& store, NdiSystem* ndi);
    ~Engine();

    void start();
    void stop();

    nlohmann::json metrics();
    nlohmann::json sourceStatus();
    std::vector<std::string> discoveredNdiSources();
    bool ndiAvailable() const { return ndi_ != nullptr; }

    // Freezes the current canvas so the next layout change crossfades from it over `ms`.
    // Call before changing the layout; returns false if the render thread didn't respond.
    bool captureForTransition(int ms);

    // Low-res BGRA preview of "canvas" or an output id, encoded as JPEG.
    bool previewJpeg(const std::string& target, std::string& jpeg);
    // Full-resolution JPEG of the next frame an output sends.
    bool snapshotJpeg(const std::string& outputId, std::string& jpeg, uint64_t& frameId);
    // Same frame as raw BGRA (UYVY outputs are decoded), for automated checks.
    bool snapshotBgra(const std::string& outputId, std::vector<uint8_t>& bgra, int& w, int& h, uint64_t& frameId);

private:
    struct SourceState {
        SourceConfig cfg;
        TexId tex[3] = {0, 0, 0};
        int texCount = 0, current = 0;
        int w = 0, h = 0;
        bool hasFrame = false;
        bool patternDrawn = false;  // test pattern: texture holds a drawn frame
        std::unique_ptr<NdiReceiver> rx;
    };
    struct PreviewState {
        std::string target;
        int w = 0, h = 0;
        TexId tex = 0;
        ReadbackId rb = 0;
        SlotRing ring{2};
        std::mutex m;
        std::vector<uint8_t> bgra;
        uint64_t frameId = 0;
        bool has = false;
    };
    struct SyncEntry {
        uint64_t frameId;
        int expected, delivered, dropped;
    };

    void loop();
    void reconcile(const Layout& layout);
    void renderFrame(const Layout& layout, uint64_t frameId, Clock::time_point tick, double seconds);
    void updateSources(const Layout& layout, const std::set<std::string>& needed, uint64_t frameId, double seconds);
    void renderCanvas(const Layout& layout, const std::set<std::string>& needed, Clock::time_point tick);
    void renderPreviews(const Layout& layout);
    void destroySource(SourceState& s);
    std::unique_ptr<VideoSink> makeSink(const std::string& ndiName, std::string& kind);
    void syncDelivered(uint64_t frameId, bool delivered);

    Renderer& renderer_;
    LayoutStore& store_;
    NdiSystem* ndi_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    Clock::time_point start_;
    uint64_t appliedVersion_ = 0;

    TexId canvas_ = 0;
    int canvasW_ = 0, canvasH_ = 0;
    // What the outputs sample this frame: the canvas, or (direct path) the one
    // source that exactly covers it, w x h pixels stretched over the canvas.
    struct View {
        TexId tex = 0;
        float w = 0, h = 0;
    } view_;
    std::map<std::string, SourceState> sources_;  // render thread only
    struct OutputDraw {
        TexId src;
        Uniforms u;
    };
    std::map<std::string, OutputDraw> outputDraws_;  // this frame's output mappings, for previews (render thread)

    std::mutex channelsMutex_;
    std::map<std::string, std::shared_ptr<OutputChannel>> channels_;
    std::unordered_map<ReadbackId, std::shared_ptr<OutputChannel>> channelByReadback_;
    std::map<std::string, std::shared_ptr<PreviewState>> previews_;
    std::unordered_map<ReadbackId, std::shared_ptr<PreviewState>> previewByReadback_;

    std::mutex previewReqMutex_;
    std::map<std::string, Clock::time_point> previewRequests_;

    std::mutex syncMutex_;
    std::deque<SyncEntry> sync_;
    uint64_t framesComplete_ = 0, framesPartial_ = 0;

    // Crossfade (render thread, except the request handshake)
    TexId fadeTex_ = 0;
    Clock::time_point fadeStart_;
    double fadeMs_ = 0;
    std::mutex transitionMutex_;
    std::condition_variable transitionCv_;
    int transitionRequestMs_ = 0;
    bool transitionCaptured_ = false;

    std::mutex statusMutex_;
    nlohmann::json sourceStatus_ = nlohmann::json::array();

    // Metrics
    RateMeter renderRate_;
    Ewma encodeMs_, waitMs_, gpuMs_, frameMs_, jitterMs_;
    std::atomic<uint64_t> frameId_{0}, lateFrames_{0}, directFrames_{0};
    std::atomic<double> targetFps_{60};
};

}  // namespace mosaic
