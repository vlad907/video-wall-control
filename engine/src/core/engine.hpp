// The render engine: paces frames, keeps GPU resources in step with the
// active layout, renders sources -> canvas -> outputs, and feeds the output
// channels. Everything GPU-related runs on the single render thread.
#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
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

    // Low-res BGRA preview of "canvas" or an output id, encoded as JPEG.
    bool previewJpeg(const std::string& target, std::string& jpeg);
    // Full-resolution JPEG of the next frame an output sends.
    bool snapshotJpeg(const std::string& outputId, std::string& jpeg, uint64_t& frameId);

private:
    struct SourceState {
        SourceConfig cfg;
        TexId tex[3] = {0, 0, 0};
        int texCount = 0, current = 0;
        int w = 0, h = 0;
        bool hasFrame = false;
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
    void updateSources(const Layout& layout, uint64_t frameId, double seconds);
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
    std::map<std::string, SourceState> sources_;  // render thread only

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

    std::mutex statusMutex_;
    nlohmann::json sourceStatus_ = nlohmann::json::array();

    // Metrics
    RateMeter renderRate_;
    Ewma encodeMs_, waitMs_, gpuMs_, frameMs_;
    std::atomic<uint64_t> frameId_{0}, lateFrames_{0};
    std::atomic<double> targetFps_{60};
};

}  // namespace mosaic
