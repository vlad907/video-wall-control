#include "io/video_file.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace mosaic {

namespace {

// Few frames ahead: hardware decoders lend out a small pool of surfaces, and an 8K frame is big.
constexpr size_t kQueueFrames = 3;

class Decoder final : public VideoFileDecoder {
public:
    Decoder(std::unique_ptr<VideoBackend> backend, bool loop)
        : backend_(std::move(backend)), info_(backend_->info()), loop_(loop), thread_([this] { run(); }) {}

    ~Decoder() override {
        stop_ = true;
        cv_.notify_all();
        thread_.join();
        std::lock_guard lock(m_);
        queue_.clear();  // decoder surfaces go back before the backend does
    }

    std::optional<DecodedFrame> frameDue(double t) override {
        std::lock_guard lock(m_);
        clock_ = t;
        std::optional<DecodedFrame> due;
        while (!queue_.empty() && queue_.front().pts <= t) {
            due = std::move(queue_.front());
            queue_.pop_front();
        }
        // Room for the decoder again; or everything it had ready was due already (the canvas renders
        // slower than the video plays): it skips ahead to the present rather than fall behind.
        if (due || behind()) cv_.notify_all();
        return due;
    }

    void restart() override {
        std::lock_guard lock(m_);
        queue_.clear();
        restart_ = true;
        finished_ = false;
        cv_.notify_all();
    }

    void setLoop(bool loop) override {
        loop_ = loop;
        cv_.notify_all();
    }
    VideoInfo info() const override { return info_; }
    bool finished() const override { return finished_; }
    std::string status() const override {
        std::lock_guard lock(m_);
        return error_.empty() ? "ok" : error_;
    }
    double decodeFps() const override { return decodeFps_; }

private:
    void run() {
        double offset = 0, lastPts = 0;
        const double frameTime = info_.fps > 0 ? 1.0 / info_.fps : 1.0 / 30.0;
        auto windowStart = std::chrono::steady_clock::now();
        int windowFrames = 0;
        while (!stop_) {
            {
                std::unique_lock lock(m_);
                cv_.wait_for(lock, std::chrono::milliseconds(20), [&] {
                    return stop_ || restart_ || ((queue_.size() < kQueueFrames || behind()) && !finished_) || (finished_ && loop_);
                });
                if (stop_) break;
                if (finished_ && !loop_) continue;
                if (queue_.size() >= kQueueFrames && !restart_ && !behind()) continue;
            }
            std::string err;
            if (restart_.exchange(false)) {
                if (!backend_->rewind(err)) setError(err);
                offset = lastPts = 0;
            }
            DecodedFrame f;
            bool eos = false;
            if (!backend_->read(f, eos, err)) {
                setError(err);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            if (eos) {
                if (loop_) {
                    // The next pass continues the timeline right after the last frame.
                    offset += lastPts + frameTime;
                    lastPts = 0;
                    if (!backend_->rewind(err)) setError(err);
                } else {
                    finished_ = true;
                }
                continue;
            }
            if (!f.hold) continue;
            lastPts = f.pts;
            f.pts += offset;
            ++windowFrames;
            auto now = std::chrono::steady_clock::now();
            double span = std::chrono::duration<double>(now - windowStart).count();
            if (span >= 1.0) {
                decodeFps_ = windowFrames / span;
                windowFrames = 0;
                windowStart = now;
            }
            std::lock_guard lock(m_);
            if (restart_) continue;  // a restart came in while decoding: this frame is stale
            while (queue_.size() >= kQueueFrames) queue_.pop_front();  // behind: the oldest is too late to show
            queue_.push_back(std::move(f));
            error_.clear();
            finished_ = false;  // looping was turned back on after the end
        }
    }

    // With m_ held: the queue is full and even its newest frame is due, so playback has moved past
    // everything decoded.
    bool behind() const { return queue_.size() >= kQueueFrames && queue_.back().pts <= clock_; }

    void setError(const std::string& e) {
        std::lock_guard lock(m_);
        error_ = e;
    }

    std::unique_ptr<VideoBackend> backend_;
    VideoInfo info_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<DecodedFrame> queue_;
    double clock_ = 0;  // the playback time last asked for (guarded by m_)
    std::string error_;
    std::atomic<bool> loop_, stop_{false}, restart_{false}, finished_{false};
    std::atomic<double> decodeFps_{0};
    std::thread thread_;
};

}  // namespace

std::unique_ptr<VideoFileDecoder> openVideoFile(const std::string& path, void* nativeDevice, bool loop, std::string& error) {
    auto backend = openVideoBackend(path, nativeDevice, error);
    if (!backend) return nullptr;
    return std::make_unique<Decoder>(std::move(backend), loop);
}

}  // namespace mosaic
