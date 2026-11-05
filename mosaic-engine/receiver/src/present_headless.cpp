#include <algorithm>
#include <thread>

#include "presenter.hpp"

namespace mosaic::rx {

namespace {

class HeadlessPresenter final : public Presenter {
public:
    HeadlessPresenter(double hz, std::vector<int> extraLatency) : hz_(hz), next_(Clock::now()), extra_(std::move(extraLatency)) {}
    std::string name() const override { return "headless"; }
    bool configure(const std::vector<WindowSpec>&, std::string&) override { return true; }
    void upload(const std::vector<Upload>&) override {}
    // Every window "reaches the screen" at the same moment (plus its extra latency), one present per refresh: the bookkeeping
    // above it gets exercised (offsets 0, no missed refreshes).
    void present(const std::vector<TileDraw>& tiles, const std::vector<bool>&) override {
        int windows = 0;
        for (auto& t : tiles) windows = std::max(windows, t.window + 1);
        ++count_;
        int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        timings_.assign(static_cast<size_t>(windows), {true, count_, count_, count_, now});
        for (size_t w = 0; w < timings_.size() && w < extra_.size(); ++w) timings_[w].syncMicros += static_cast<int64_t>(extra_[w] * 1e6 / hz_);
    }
    std::vector<PresentTiming> timings() override { return timings_; }
    void waitForRefresh() override {
        next_ += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz_));
        auto now = Clock::now();
        if (next_ < now) next_ = now;  // fell behind: don't try to catch up
        std::this_thread::sleep_until(next_);
    }
    double refreshHz() const override { return hz_; }

private:
    double hz_;
    Clock::time_point next_;
    std::vector<int> extra_;
    uint32_t count_ = 0;
    std::vector<PresentTiming> timings_;
};

}  // namespace

std::unique_ptr<Presenter> createHeadlessPresenter(double hz, std::vector<int> extraLatency) {
    return std::make_unique<HeadlessPresenter>(hz, std::move(extraLatency));
}

#ifndef _WIN32
std::unique_ptr<Presenter> createD3D11Presenter(bool, std::string& error) {
    error = "Direct3D 11 is only available on Windows";
    return nullptr;
}
#endif

}  // namespace mosaic::rx
