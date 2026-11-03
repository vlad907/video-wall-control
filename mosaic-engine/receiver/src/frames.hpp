// Received frames and the small per-stream queue the receiver aligns from.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mosaic::rx {

using Clock = std::chrono::steady_clock;

// One video frame from a stream. `hold` keeps the pixels alive (an NDI frame, or our own buffer).
struct RxFrame {
    uint64_t frameId = 0;  // the engine's frame id (from NDI metadata); 0 = not a mosaic stream
    int64_t timecode = 0;  // NDI timecode, 100 ns units
    int width = 0, height = 0, stride = 0;
    const uint8_t* data = nullptr;  // BGRA / BGRX, or UYVY 4:2:2 (2 bytes per pixel) when `uyvy`
    bool uyvy = false;
    std::shared_ptr<void> hold;
    Clock::time_point arrived;
};

// The last few frames of one stream, oldest first. Filled by the stream's receive thread,
// read by the render thread. Small and bounded: when full, the oldest frame goes.
class StreamQueue {
public:
    explicit StreamQueue(size_t capacity = 8) : capacity_(capacity) {}

    void push(RxFrame f) {
        std::lock_guard lock(mutex_);
        frames_.push_back(std::move(f));
        while (frames_.size() > capacity_) frames_.pop_front();
        lastArrival_ = Clock::now();
        ++received_;
    }

    // Newest frame id held (0 if empty or not a mosaic stream).
    uint64_t newestId() const {
        std::lock_guard lock(mutex_);
        return frames_.empty() ? 0 : frames_.back().frameId;
    }

    // The newest frame whose id is <= `id` (with id 0: the newest frame). Frames older than the
    // one returned are dropped: they will never be shown.
    std::optional<RxFrame> takeUpTo(uint64_t id) {
        std::lock_guard lock(mutex_);
        std::optional<RxFrame> pick;
        size_t keepFrom = 0;
        for (size_t i = 0; i < frames_.size(); ++i)
            if (id == 0 || frames_[i].frameId <= id) {
                pick = frames_[i];
                keepFrom = i;
            }
        if (pick) frames_.erase(frames_.begin(), frames_.begin() + static_cast<std::ptrdiff_t>(keepFrom));
        return pick;
    }

    bool empty() const {
        std::lock_guard lock(mutex_);
        return frames_.empty();
    }
    Clock::time_point lastArrival() const {
        std::lock_guard lock(mutex_);
        return lastArrival_;
    }
    uint64_t received() const {
        std::lock_guard lock(mutex_);
        return received_;
    }
    void clear() {
        std::lock_guard lock(mutex_);
        frames_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::deque<RxFrame> frames_;
    size_t capacity_;
    Clock::time_point lastArrival_{};
    uint64_t received_ = 0;
};

// A source of frames for one stream: NDI, or a synthetic generator for tests.
class FrameInput {
public:
    virtual ~FrameInput() = default;
    virtual StreamQueue& queue() = 0;
    virtual std::string status() const = 0;  // "ok", "waiting for source", "NDI unavailable", …
};

}  // namespace mosaic::rx
