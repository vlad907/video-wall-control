// One output stream: its render target, a bounded ring of readback buffers,
// and a sender thread that hands finished frames to a VideoSink.
//
// Slot lifecycle (bounded, no allocation per frame):
//   Free --render thread--> Gpu --GPU done--> Ready --sender--> Held --release--> Free
// If the sender falls behind, the oldest Ready frame is dropped so latency
// never grows past `maxQueue` frames.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <optional>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/metrics.hpp"
#include "io/video_io.hpp"
#include "render/renderer.hpp"

namespace mosaic {

class SlotRing {
public:
    enum State : int { Free = 0, Gpu, Ready, Held };

    explicit SlotRing(int n) : states_(n) {
        for (auto& s : states_) s.store(Free);
    }
    int size() const { return static_cast<int>(states_.size()); }
    // Render thread.
    int acquire() {
        for (int i = 0; i < size(); ++i)
            if (states_[i].load() == Free) {
                states_[i].store(Gpu);
                return i;
            }
        return -1;
    }
    void set(int slot, State s) { states_[slot].store(s); }
    int count(State s) const {
        int n = 0;
        for (auto& st : states_) n += st.load() == s;
        return n;
    }
    // Any thread.
    void requestRelease(int slot) {
        std::lock_guard l(m_);
        pending_.push_back(slot);
    }
    // Render thread: frees slots whose release was requested.
    template <class F>
    void drainReleases(F&& onRelease) {
        std::vector<int> slots;
        {
            std::lock_guard l(m_);
            slots.swap(pending_);
        }
        for (int s : slots) {
            onRelease(s);
            states_[s].store(Free);
        }
    }

private:
    std::vector<std::atomic<int>> states_;
    std::mutex m_;
    std::vector<int> pending_;
};

struct FrameMeta {
    uint64_t frameId = 0;
    Clock::time_point tick{};
    int64_t timecode = 0;
};

struct OutputMetrics {
    double fps = 0;
    uint64_t sent = 0;
    uint64_t droppedNoSlot = 0;   // render thread found no free buffer
    uint64_t droppedQueue = 0;    // sender too slow, oldest ready frame discarded
    int queueDepth = 0;
    int slotsInUse = 0;
    double sendMs = 0, sendMsMax = 0;
    double latencyMs = 0;         // frame tick -> sink accepted it
    uint64_t lastFrameId = 0;
    int connections = -1;
};

class OutputChannel {
public:
    static constexpr int kSlots = 4;
    static constexpr size_t kMaxQueue = 2;

    OutputChannel(std::string id, int width, int height, std::string ndiName, double fps,
                  Renderer& renderer, std::unique_ptr<VideoSink> sink);
    ~OutputChannel();

    const std::string id;
    const int width, height;
    const std::string ndiName;
    TexId target() const { return target_; }
    ReadbackId readback() const { return readback_; }
    std::string sinkKind() const { return sink_->kind(); }

    // Render thread.
    int acquireSlot(const FrameMeta& meta);
    void drainReleases();
    void countNoSlotDrop() { droppedNoSlot_++; }

    // GPU completion (any thread).
    void onReadback(int slot, const ReadbackView& view);

    // Stops the sender, then waits for every slot to be released. Render thread.
    void shutdown();
    // Frees GPU resources. Call after shutdown() and renderer.waitIdle().
    void destroyGpuResources();

    OutputMetrics metrics();
    // Full-resolution BGRA copy of the next frame sent (for testing). Blocks up to timeoutMs.
    bool snapshot(std::vector<uint8_t>& bgra, int& w, int& h, uint64_t& frameId, int timeoutMs);

    std::function<void(uint64_t frameId)> onDelivered;  // set before use
    std::function<void(uint64_t frameId)> onDropped;

private:
    struct ReadyFrame {
        int slot;
        ReadbackView view;
        FrameMeta meta;
    };
    void run();

    Renderer& renderer_;
    std::unique_ptr<VideoSink> sink_;
    double fps_;
    TexId target_ = 0;
    ReadbackId readback_ = 0;
    SlotRing ring_{kSlots};
    std::vector<FrameMeta> meta_;

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<ReadyFrame> ready_;
    bool stop_ = false;
    std::thread thread_;

    std::atomic<uint64_t> sent_{0}, droppedNoSlot_{0}, droppedQueue_{0}, lastFrameId_{0};
    RateMeter rate_;
    Ewma sendMs_, latencyMs_;

    std::mutex snapMutex_;
    std::condition_variable snapCv_;
    bool snapWanted_ = false, snapReady_ = false;
    std::vector<uint8_t> snapData_;
    int snapW_ = 0, snapH_ = 0;
    uint64_t snapFrame_ = 0;
};

}  // namespace mosaic
