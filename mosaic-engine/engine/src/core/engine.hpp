// The render engine: paces frames, keeps GPU resources in step with the
// active layout, renders sources -> canvas -> outputs, and feeds the output
// channels. Everything GPU-related runs on the single render thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "audio/audio_outputs.hpp"
#include "core/layout_store.hpp"
#include "core/metrics.hpp"
#include "core/output_channel.hpp"
#include "io/video_file.hpp"
#include "io/video_io.hpp"
#include "render/renderer.hpp"

namespace mosaic {

class Engine {
public:
    // Renders one canvas (canvasId: see LayoutStore), following its layout.
    Engine(Renderer& renderer, LayoutStore& store, NdiSystem* ndi, std::string canvasId = "canvas-1");
    const std::string& canvasId() const { return canvasId_; }
    // The render thread stopped on an error (GPU device lost, a GPU call failed): why. The canvas
    // is then started again (Canvases::sync).
    bool failed() const { return failed_; }
    // Tests: the next frame fails as if the GPU device was lost.
    void simulateFailure() { simulateFailure_ = true; }
    std::string failure() const;
    ~Engine();

    void start();
    void stop();

    // Folder that image sources' files are read from (set before start()).
    void setMediaDir(std::filesystem::path dir) { mediaDir_ = std::move(dir); }
    const std::filesystem::path& mediaDir() const { return mediaDir_; }

    nlohmann::json metrics();
    nlohmann::json sourceStatus();
    std::vector<std::string> discoveredNdiSources();
    bool ndiAvailable() const { return ndi_ != nullptr; }
    // The sound outputs: what each plays, its level and delay, and any problem.
    nlohmann::json audioStatus() const { return audio_->status(); }

    // Freezes the current canvas so the next layout change crossfades from it over `ms`,
    // within `region` (canvas pixels; w/h 0 = the whole canvas, e.g. one wall group's area).
    // Call before changing the layout; returns false if the render thread didn't respond.
    bool captureForTransition(int ms, Rect region = {});

    // Starts a video source from its first frame again (any thread; done on the next frame).
    void restartVideo(const std::string& sourceId);

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
        std::vector<uint8_t> pending;  // image: decoded pixels waiting for upload
        bool hasAlpha = false;         // image: has transparent pixels (drawn blended)
        std::string error;             // image: why it couldn't be loaded
        Clock::time_point createdAt = Clock::now(), lastFrameAt;  // NDI: for spotting a lost signal
        bool signalLost = false;
        std::unique_ptr<NdiReceiver> rx;
        // Video files: the decoder, and the playback clock (paused = position held).
        std::unique_ptr<VideoFileDecoder> video;
        Clock::time_point playStart = Clock::now();
        double pausedAt = -1;   // seconds into the video while paused; -1 = playing
        bool gpuFrames = false; // its texture takes hardware-decoded frames (render target) rather than uploads
    };
    std::mutex videoRestartMutex_;
    std::vector<std::string> videoRestarts_;
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
    void run();
    void fail(const std::string& why);
    std::atomic<bool> failed_{false}, simulateFailure_{false};
    std::string failure_;  // statusMutex_
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
    std::string canvasId_;
    std::unique_ptr<AudioOutputs> audio_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    Clock::time_point start_;
    std::shared_ptr<const Layout> applied_;  // the layout reconciled last

    TexId canvas_ = 0;
    int canvasW_ = 0, canvasH_ = 0;
    // What the outputs sample this frame: the canvas, or (direct path) the one
    // source that exactly covers it, w x h pixels stretched over the canvas.
    struct View {
        TexId tex = 0;
        float w = 0, h = 0;
    } view_;
    std::map<std::string, SourceState> sources_;  // render thread only
    std::filesystem::path mediaDir_;
    // Fallback images for NDI sources without signal, by media file name (render thread only).
    struct ImageTex {
        TexId tex = 0;
        int w = 0, h = 0;
        bool hasAlpha = false, uploaded = false;
        std::vector<uint8_t> pending;
        std::string error;
    };
    std::map<std::string, ImageTex> fallbacks_;
    const ImageTex* fallbackFor(const Layout& layout, const SourceConfig& sc) const;
    struct OutputDraw {
        TexId src;
        Uniforms u;
    };
    std::map<std::string, OutputDraw> outputDraws_;  // this frame's output mappings, for previews (render thread)

    // Display outputs (an output with a monitor set): its window on the monitor, by output id.
    struct DisplayOut {
        DisplayId id = 0;  // 0 = couldn't open (error says why; tried again every few seconds)
        std::string monitor, error;
        int modeW = 0, modeH = 0, modeHz = 0;  // the mode asked for when it opened (0 = as it was)
        std::string modeError;                 // why the monitor couldn't be put in that mode
        // Delay: the last frames drawn, oldest shown. ring.size() = frames held + 1.
        std::vector<TexId> ring;
        int ringW = 0, ringH = 0, head = 0, filled = 0;
    };
    std::map<std::string, DisplayOut> displays_;  // render thread only
    void syncDisplays(const Layout& layout, bool retryFailed);
    void closeDisplay(DisplayOut& d);
    void freeRing(DisplayOut& d);
    std::mutex displayStatusMutex_;
    nlohmann::json displayStatus_ = nlohmann::json::object();  // by output id, for metrics()
    Clock::time_point displayStatsAt_{};
    std::map<std::string, uint64_t> displayPresents_;  // render thread: presents at the last status update

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
    Rect fadeRect_;  // the part of the canvas that crossfades
    std::mutex transitionMutex_;
    std::condition_variable transitionCv_;
    int transitionRequestMs_ = 0;
    Rect transitionRequestRect_;
    bool transitionCaptured_ = false;

    mutable std::mutex statusMutex_;
    nlohmann::json sourceStatus_ = nlohmann::json::array();

    // Metrics
    RateMeter renderRate_;
    Ewma encodeMs_, waitMs_, gpuMs_, frameMs_, jitterMs_;
    std::atomic<uint64_t> frameId_{0}, lateFrames_{0}, directFrames_{0};
    std::atomic<double> targetFps_{60};
};

}  // namespace mosaic
