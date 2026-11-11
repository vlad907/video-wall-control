#include <algorithm>
#include <cmath>
#include <thread>

#include "presenter.hpp"

namespace mosaic::rx {

namespace {

class HeadlessPresenter final : public Presenter {
public:
    HeadlessPresenter(double hz, std::vector<int> extraLatency, double msPerWindow)
        : hz_(hz), start_(Clock::now()), extra_(std::move(extraLatency)), cost_(msPerWindow) {}
    std::string name() const override { return "headless"; }
    bool configure(const std::vector<WindowSpec>& windows, std::string&) override {
        timings_.assign(windows.size(), {});
        counts_.assign(windows.size(), 0);
        return true;
    }
    void upload(const std::vector<Upload>&) override {}
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
            if (cost_ > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(cost_));
            int64_t now = micros(Clock::now());
            int64_t tick = refreshAfter(now) + (w < extra_.size() ? extra_[w] : 0);
            t = {true, ++counts_[w], counts_[w], static_cast<uint32_t>(tick), micros(start_) + static_cast<int64_t>(tick * 1e6 / hz_), lastRefresh_, now, true};
        }
    }
    std::vector<PresentTiming> timings() override { return timings_; }
    void waitForRefresh() override {
        // Until the next refresh; a late caller waits for the one after it, like a real monitor.
        int64_t tick = refreshAfter(micros(Clock::now()));
        std::this_thread::sleep_until(start_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(tick / hz_)));
        lastRefresh_ = micros(start_) + static_cast<int64_t>(tick * 1e6 / hz_);
    }
    double refreshHz() const override { return hz_; }

private:
    static int64_t micros(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count(); }
    int64_t refreshAfter(int64_t us) const { return static_cast<int64_t>(std::floor((us - micros(start_)) * hz_ / 1e6)) + 1; }

    double hz_;
    Clock::time_point start_;
    std::vector<int> extra_;
    double cost_;
    std::vector<uint32_t> counts_;
    int64_t lastRefresh_ = 0;
    std::vector<PresentTiming> timings_;
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
