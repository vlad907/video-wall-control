// A made-up stream for tests and for trying the receiver without NDI: frames numbered like
// the engine's (all synthetic streams share one clock), arriving with a chosen delay,
// jitter, dropped frames and stalls. Each frame is a colour with a white bar whose position
// follows the frame id, so displays out of step are visible at a glance.
#pragma once

#include <atomic>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "frames.hpp"

namespace mosaic::rx {

struct SyntheticOptions {
    int width = 640, height = 360;
    double fps = 60.0;
    double delayMs = 0;      // every frame arrives this late
    double jitterMs = 0;     // plus 0..jitter, random
    int dropEvery = 0;       // skip every Nth frame id (0 = never)
    double stallAtS = -1;    // stop sending at this time (s since start) ...
    double stallForS = 0;    // ... for this long
    bool stallLate = false;  // the stall's frames come late, all at once at its end (like NDI over a congested network), instead of never
    uint32_t colour = 0xFF3060C0;  // BGRA
};

class SyntheticInput : public FrameInput {
public:
    explicit SyntheticInput(SyntheticOptions o) : o_(o), thread_([this] { run(); }) {}
    ~SyntheticInput() override {
        stop_ = true;
        thread_.join();
    }
    StreamQueue& queue() override { return queue_; }
    std::string status() const override { return "ok (synthetic)"; }

    // All synthetic streams number their frames from the same moment, like engine outputs.
    static Clock::time_point epoch() {
        static const Clock::time_point e = Clock::now();
        return e;
    }

private:
    void run() {
        using namespace std::chrono;
        std::mt19937 rng(static_cast<uint32_t>(o_.colour));
        std::uniform_real_distribution<double> jitter(0.0, o_.jitterMs);
        const auto period = duration<double>(1.0 / o_.fps);
        const auto start = epoch();
        uint64_t n = static_cast<uint64_t>(duration<double>(Clock::now() - start) / period) + 1;
        std::vector<RxFrame> held;  // a late stall's frames
        while (!stop_) {
            auto due = start + duration_cast<Clock::duration>(period * static_cast<double>(n)) +
                       duration_cast<Clock::duration>(duration<double, std::milli>(o_.delayMs + jitter(rng)));
            while (!stop_ && Clock::now() < due) std::this_thread::sleep_for(microseconds(500));
            double t = duration<double>(Clock::now() - start).count();
            bool stalled = o_.stallAtS >= 0 && t >= o_.stallAtS && t < o_.stallAtS + o_.stallForS;
            bool dropped = o_.dropEvery > 0 && n % static_cast<uint64_t>(o_.dropEvery) == 0;
            if (stalled && o_.stallLate && !dropped) held.push_back(make(n));
            if (!stop_ && !stalled) {
                for (auto& f : held) f.arrived = Clock::now(), queue_.push(std::move(f));
                held.clear();
                if (!dropped) queue_.push(make(n));
            }
            ++n;
        }
    }

    RxFrame make(uint64_t id) {
        auto pixels = std::make_shared<std::vector<uint32_t>>(static_cast<size_t>(o_.width) * o_.height, o_.colour);
        int bar = static_cast<int>((id % 60) * o_.width / 60);
        for (int y = 0; y < o_.height; ++y)
            for (int x = bar; x < std::min(o_.width, bar + o_.width / 60 + 1); ++x) (*pixels)[static_cast<size_t>(y) * o_.width + x] = 0xFFFFFFFF;
        RxFrame f;
        f.frameId = id;
        f.timecode = static_cast<int64_t>(id) * static_cast<int64_t>(1e7 / o_.fps);
        f.width = o_.width;
        f.height = o_.height;
        f.stride = o_.width * 4;
        f.data = reinterpret_cast<const uint8_t*>(pixels->data());
        f.hold = pixels;
        f.arrived = Clock::now();
        return f;
    }

    SyntheticOptions o_;
    StreamQueue queue_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace mosaic::rx
