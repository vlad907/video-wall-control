#include <thread>

#include "presenter.hpp"

namespace mosaic::rx {

namespace {

class HeadlessPresenter final : public Presenter {
public:
    explicit HeadlessPresenter(double hz) : hz_(hz), next_(Clock::now()) {}
    std::string name() const override { return "headless"; }
    bool configure(const std::vector<WindowSpec>&, std::string&) override { return true; }
    void upload(int, const RxFrame&) override {}
    void present(const std::vector<TileDraw>&) override {}
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
};

}  // namespace

std::unique_ptr<Presenter> createHeadlessPresenter(double hz) { return std::make_unique<HeadlessPresenter>(hz); }

#ifndef _WIN32
std::unique_ptr<Presenter> createD3D11Presenter(bool, std::string& error) {
    error = "Direct3D 11 is only available on Windows";
    return nullptr;
}
#endif

}  // namespace mosaic::rx
