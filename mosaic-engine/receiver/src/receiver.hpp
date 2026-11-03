// The receiver: its displays (stream + where on screen), one input per stream, and the
// render loop that shows the same engine frame on every display each refresh.
#pragma once

#include <atomic>
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
    int lagFrames = 2;       // a stream this far behind the others stops holding them back
    int lostAfterMs = 500;   // no frame for this long = signal lost (stops being waited for)
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
    };

    void loop();
    void apply(const ReceiverConfig& c);
    void frame();

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
    std::vector<uint64_t> recentSpread_;  // last ~10 s
    Clock::time_point started_ = Clock::now();

    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace mosaic::rx
