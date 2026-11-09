// The receiver: its displays (stream + where on screen), one input per stream, and the
// render loop that shows the same engine frame on every display each refresh.
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "frames.hpp"
#include "presenter.hpp"

namespace mosaic::rx {

// One display: a stream shown in a window (a monitor), filling it unless a tile is given.
struct DisplayConfig {
    int index = 0;           // display number on this PC, as the endpoint numbers them
    std::string stream;      // full NDI name, or "synthetic" (see `synthetic`) for tests
    std::string label;       // for status only
    WindowSpec window;       // the monitor's desktop rectangle
    WindowSpec tile{0, 0, 0, 0};  // part of the window (w/h 0 = all of it); spanned monitors
    nlohmann::json synthetic;     // options for a synthetic stream
};

struct ReceiverConfig {
    std::vector<DisplayConfig> displays;
    int lagFrames = 6;       // a stream this far behind the others stops holding them back (100 ms at 60 fps)
    int lostAfterMs = 500;   // no frame for this long = signal lost (stops being waited for)
    bool flashTest = false;  // every display flashes white on engine frames divisible by 30
    // Windows shows some displays a refresh later than others (it composites them: HDR on, an
    // overlay, a different setting). Measured, and the other displays are held back to match.
    bool compensate = true;
    // Buffer: every display shows each frame this long after the engine made it (plus the
    // fastest network transit), so a stream that stalls for less than this never shows it.
    // 0 = show frames as soon as every stream has them (lowest delay).
    int bufferMs = 0;
};

void from_json(const nlohmann::json& j, ReceiverConfig& c);
nlohmann::json to_json(const ReceiverConfig& c);

class Receiver {
public:
    // The presenter is created on the render thread (windows belong to the thread that made them).
    using PresenterFactory = std::function<std::unique_ptr<Presenter>(std::string& error)>;
    explicit Receiver(PresenterFactory factory);
    ~Receiver();

    void setConfig(const ReceiverConfig& c);
    ReceiverConfig config() const;
    nlohmann::json status() const;

private:
    struct Stream {
        std::string name;
        std::unique_ptr<FrameInput> input;
        std::string error;
        bool everSynced = false;
        uint64_t shownId = 0;          // frame id on screen now
        const void* shownHold = nullptr;  // which frame is uploaded (to skip re-uploads)
        uint64_t late = 0;             // refreshes it couldn't show the in-step frame
        uint64_t shown = 0;            // new pictures put on screen
        int width = 0, height = 0;
        // The stream's recent pictures (newest last), for displays held back by compensation.
        struct Picture {
            Clock::time_point at;  // when it became the newest
            uint64_t gen = 0, frameId = 0;
        };
        std::deque<Picture> history;
        uint64_t gen = 0;  // pictures uploaded
    };

    void loop();
    void apply(const ReceiverConfig& c);
    void frame();
    void updateCompensation();

    PresenterFactory factory_;
    std::unique_ptr<Presenter> presenter_;
    std::string presenterError_;

    mutable std::mutex configMutex_;
    ReceiverConfig config_;
    bool configDirty_ = false;

    // Render thread only.
    ReceiverConfig active_;
    std::vector<Stream> streams_;
    std::vector<int> displayStream_;  // per display: index into streams_
    std::vector<TileDraw> tiles_;
    uint64_t target_ = 0;

    // Telemetry.
    mutable std::mutex statusMutex_;
    nlohmann::json status_ = nlohmann::json::object();
    uint64_t presents_ = 0, alignedPresents_ = 0, comparedPresents_ = 0;
    // Measured each second: refreshes actually presented, new pictures per stream, upload time.
    Clock::time_point windowStart_ = Clock::now();
    uint64_t windowPresents_ = 0;
    double windowUploadMs_ = 0;
    std::vector<uint64_t> windowShown_;
    double presentFps_ = 0, uploadMs_ = 0;
    std::vector<double> streamFps_;

    // On-screen timing per window (monitor), from the presenter's statistics: when each new
    // picture really appeared, so displays can be compared in milliseconds.
    struct Pending {
        uint32_t present = 0;  // present number
        uint64_t frameId = 0;  // the new picture it showed
        int64_t heldUs = 0;    // how long compensation held it back
    };
    struct WindowTrack {
        std::deque<Pending> pending;  // new pictures only
        uint64_t lastFrameId = 0, frameId = 0;  // frame id on screen (after any hold-back)
        uint32_t lastSyncPresent = 0, lastSyncRefresh = 0;
        bool seen = false, valid = false;
        uint64_t missed = 0;        // refreshes the monitor showed without a new present from us
        std::deque<double> offsets;  // ms behind the earliest display, per frame (recent), as seen
        std::deque<double> lag;      // the same without our hold-back: how late Windows shows this display
        int hold = 0;                // refreshes this display is held back (compensation)
        int want = 0, wantCount = 0;  // a different hold-back measured, and for how many seconds in a row
    };
    std::vector<WindowTrack> windows_;
    std::vector<int> windowStream_;  // per window: the stream of its first tile
    struct ShownAt {
        std::vector<int64_t> at, lag;  // per window, us: when it appeared; minus our hold-back (-1 = not yet)
    };
    std::map<uint64_t, ShownAt> shownAt_;  // frame id -> when it appeared on each window
    std::deque<double> spreads_;  // ms between the first and last display to show each frame (recent)
    std::deque<double> recentSpreads_;  // the same, last ~2 s (shows compensation taking effect)
    void trackTiming(const std::vector<PresentTiming>& t);
    std::vector<uint64_t> recentSpread_;  // last ~10 s
    Clock::time_point started_ = Clock::now();

    // Freezes: the screens kept the same picture for a while. Each is logged with what held it
    // up, so it can be matched against what else the PC was doing at that time.
    //   network  a stream sent nothing for a while (the loop kept presenting its last frame)
    //   wait     Windows / the GPU didn't take a new frame (waiting for the swap chains)
    //   upload   copying the pictures to the GPU took too long
    //   present  drawing / presenting took too long (GPU or driver busy)
    //   screen   presented on time, but the monitors skipped refreshes anyway
    //   loop     none of the steps was slow: the receiver's thread didn't get to run (CPU busy)
    struct Hiccup {
        std::string at;  // local time it started
        double ms = 0;   // how long
        std::string cause, detail;
    };
    std::deque<Hiccup> hiccups_;               // the most recent ones
    std::deque<Clock::time_point> hiccupTimes_;  // last minute, for the count
    uint64_t hiccupTotal_ = 0;
    Clock::time_point lastPresent_{}, lastLoopHiccup_{};
    double waitMs_ = 0, presentMs_ = 0;           // this refresh's steps (upload: lastUploadMs_)
    double lastUploadMs_ = 0;
    std::vector<Clock::time_point> streamGap_;  // per stream: last arrival before it went quiet (zero = receiving)
    uint32_t screenSkipped_ = 0;                // most refreshes a monitor skipped since the last present
    void hiccup(const std::string& cause, double ms, const std::string& detail);
    uint64_t gapsAbsorbed_ = 0;  // network gaps shorter than the buffer (not seen on screen)

    // The engine's clock as seen here: the smallest arrival-minus-timecode over the last ~10 s
    // (a frame that came straight through). Frames are shown at timecode + this + the buffer.
    std::deque<std::pair<Clock::time_point, int64_t>> transits_;
    int64_t engineOffsetUs_ = 0;
    bool engineClockKnown_ = false;
    void trackEngineClock(Clock::time_point now);
    double windowHeldMs_ = 0, heldMs_ = 0;  // time from arrival to on screen, per new picture
    uint64_t windowHeldCount_ = 0;

    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace mosaic::rx
