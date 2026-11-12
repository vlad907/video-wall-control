#include <algorithm>
#include <cmath>
#include <thread>

#include "presenter.hpp"

namespace mosaic::rx {

namespace {

// Sleeps until `t` to within a fraction of a millisecond: sleeps most of the way, then spins
// (a plain sleep is only as fine as the system's timer).
void preciseSleepUntil(Clock::time_point t) {
    auto coarse = t - std::chrono::milliseconds(2);
    if (Clock::now() < coarse) std::this_thread::sleep_until(coarse);
    while (Clock::now() < t) std::this_thread::yield();
}

class HeadlessPresenter final : public Presenter {
public:
    HeadlessPresenter(double hz, std::vector<int> extraLatency, double msPerWindow)
        : hz_(hz), start_(Clock::now()), extra_(std::move(extraLatency)), cost_(msPerWindow) {}
    std::string name() const override { return "headless"; }
    bool configure(const std::vector<WindowSpec>& windows, std::string&) override {
        specs_ = windows;
        probes_.assign(windows.size(), 0);
        timings_.assign(windows.size(), {});
        counts_.assign(windows.size(), 0);
        return true;
    }
    void upload(const std::vector<Upload>& frames) override {
        if (!probe_) return;
        for (auto& u : frames) {
            if (!u.frame || u.frame->uyvy) continue;
            if (static_cast<size_t>(u.stream) >= pictures_.size()) pictures_.resize(static_cast<size_t>(u.stream) + 1);
            pictures_[static_cast<size_t>(u.stream)] = *u.frame;  // keeps its pixels (hold)
        }
    }
    void setProbe(bool on) override { probe_ = on; }
    std::vector<uint32_t> probe() override { return probes_; }
    // The monitors refresh together every 1/hz; a present reaches the screen at the first refresh
    // after it (plus the window's extra latency). Each window's present takes `cost_` ms.
    void present(const std::vector<TileDraw>& tiles, const std::vector<bool>&, const std::vector<bool>& which) override {
        size_t windows = which.size();
        for (auto& t : tiles) windows = std::max(windows, static_cast<size_t>(t.window + 1));
        if (timings_.size() < windows) timings_.resize(windows), counts_.resize(windows, 0);
        for (size_t w = 0; w < windows; ++w) {
            auto& t = timings_[w];
            t.presented = false;
            if (w >= which.size() || !which[w]) continue;
            if (probe_ && w < specs_.size()) probes_[w] = centreColour(tiles, static_cast<int>(w));
            if (cost_ > 0) preciseSleepUntil(Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(cost_)));
            int64_t now = micros(Clock::now());
            int64_t tick = refreshAfter(now) + (w < extra_.size() ? extra_[w] : 0);
            t = {true, ++counts_[w], counts_[w], static_cast<uint32_t>(tick), micros(start_) + static_cast<int64_t>(tick * 1e6 / hz_), lastRefresh_, now, true};
        }
    }
    std::vector<PresentTiming> timings() override { return timings_; }
    void waitForRefresh() override {
        // Until the next refresh; a late caller waits for the one after it, like a real monitor.
        int64_t tick = refreshAfter(micros(Clock::now()));
        preciseSleepUntil(start_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(tick / hz_)));
        lastRefresh_ = micros(start_) + static_cast<int64_t>(tick * 1e6 / hz_);
    }
    double refreshHz() const override { return hz_; }

private:
    // What a real presenter would draw at the centre of window w: the tile over it, and the pixel
    // of its stream's picture that lands there (the tile shows the part cx, cy, cw, ch of it).
    uint32_t centreColour(const std::vector<TileDraw>& tiles, int w) const {
        double px = specs_[static_cast<size_t>(w)].w / 2.0, py = specs_[static_cast<size_t>(w)].h / 2.0;
        for (auto& t : tiles) {
            if (t.window != w || t.stream < 0 || static_cast<size_t>(t.stream) >= pictures_.size()) continue;
            if (px < t.x || px >= t.x + t.w || py < t.y || py >= t.y + t.h) continue;
            const RxFrame& f = pictures_[static_cast<size_t>(t.stream)];
            if (!f.data) continue;
            int sx = std::clamp(static_cast<int>((t.cx + (px - t.x) / t.w * t.cw) * f.width), 0, f.width - 1);
            int sy = std::clamp(static_cast<int>((t.cy + (py - t.y) / t.h * t.ch) * f.height), 0, f.height - 1);
            const uint8_t* p = f.data + static_cast<size_t>(sy) * f.stride + static_cast<size_t>(sx) * 4;  // B, G, R, A
            return (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
        }
        return 0;
    }

    static int64_t micros(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count(); }
    int64_t refreshAfter(int64_t us) const { return static_cast<int64_t>(std::floor((us - micros(start_)) * hz_ / 1e6)) + 1; }

    double hz_;
    Clock::time_point start_;
    std::vector<int> extra_;
    double cost_;
    std::vector<uint32_t> counts_;
    int64_t lastRefresh_ = 0;
    std::vector<PresentTiming> timings_;
    bool probe_ = false;
    std::vector<WindowSpec> specs_;
    std::vector<uint32_t> probes_;
    std::vector<RxFrame> pictures_;  // by stream: its newest picture (probe only)
};

}  // namespace

std::unique_ptr<Presenter> createHeadlessPresenter(double hz, std::vector<int> extraLatency, double msPerWindow) {
    return std::make_unique<HeadlessPresenter>(hz, std::move(extraLatency), msPerWindow);
}

#ifndef _WIN32
std::unique_ptr<Presenter> createD3D11Presenter(bool, std::string& error) {
    error = "Direct3D 11 is only available on Windows";
    return nullptr;
}
#endif

}  // namespace mosaic::rx
