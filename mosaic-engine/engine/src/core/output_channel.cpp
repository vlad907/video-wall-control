#include "core/output_channel.hpp"

#include <cstring>

#include "util/realtime.hpp"

namespace mosaic {

OutputChannel::OutputChannel(std::string id_, int w, int h, std::string ndi, double fps, bool packUyvy,
                             Renderer& renderer, std::unique_ptr<VideoSink> sink)
    : id(std::move(id_)), width(w), height(h), ndiName(std::move(ndi)), uyvy(packUyvy), renderer_(renderer),
      sink_(std::move(sink)), fps_(fps), meta_(kSlots) {
    // UYVY: one BGRA8 texel carries two pixels, so the target and readback are half as wide.
    int rbWidth = uyvy ? width / 2 : width;
    if (uyvy) packed_ = renderer_.createTexture(rbWidth, height, TexUsage::RenderTarget);
    else target_ = renderer_.createTexture(width, height, TexUsage::RenderTarget);
    readback_ = renderer_.createReadback(rbWidth, height, kSlots);
    thread_ = std::thread([this] { run(); });
}

OutputChannel::~OutputChannel() {
    {
        std::lock_guard l(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

int OutputChannel::acquireSlot(const FrameMeta& meta) {
    int slot = ring_.acquire();
    if (slot >= 0) meta_[slot] = meta;
    return slot;
}

void OutputChannel::drainReleases() {
    ring_.drainReleases([this](int slot) { renderer_.releaseSlot(readback_, slot); });
}

void OutputChannel::onReadback(int slot, const ReadbackView& view) {
    ring_.set(slot, SlotRing::Ready);
    std::optional<ReadyFrame> evicted;
    {
        std::lock_guard l(m_);
        if (stop_) {
            ring_.requestRelease(slot);
            return;
        }
        ready_.push_back({slot, view, meta_[slot]});
        if (ready_.size() > kMaxQueue) {
            evicted = ready_.front();
            ready_.pop_front();
        }
    }
    if (evicted) {
        droppedQueue_++;
        ring_.requestRelease(evicted->slot);
        if (onDropped) onDropped(evicted->meta.frameId);
    }
    cv_.notify_one();
}

void OutputChannel::run() {
    raiseThreadPriority(ThreadRole::Sender);
    int held = -1;  // slot the sink may still be reading (async NDI)
    for (;;) {
        ReadyFrame f;
        {
            std::unique_lock l(m_);
            cv_.wait(l, [this] { return stop_ || !ready_.empty(); });
            if (stop_) break;
            f = ready_.front();
            ready_.pop_front();
        }
        ring_.set(f.slot, SlotRing::Held);

        VideoFrameView v;
        v.data = f.view.data;
        v.stride = f.view.stride;
        v.width = width;
        v.height = height;
        v.format = uyvy ? FrameFormat::UYVY : FrameFormat::BGRA;
        v.fps = fps_;
        v.frameId = f.meta.frameId;
        v.timecode = f.meta.timecode;

        auto t0 = Clock::now();
        sink_->send(v);
        auto t1 = Clock::now();

        {
            std::lock_guard l(snapMutex_);
            if (snapWanted_) {
                size_t row = static_cast<size_t>(v.width) * (uyvy ? 2 : 4);
                snapData_.resize(row * v.height);
                for (int y = 0; y < v.height; ++y)
                    std::memcpy(snapData_.data() + y * row, v.data + static_cast<size_t>(y) * v.stride, row);
                snapW_ = v.width;
                snapH_ = v.height;
                snapFrame_ = v.frameId;
                snapWanted_ = false;
                snapReady_ = true;
                snapCv_.notify_all();
            }
        }

        if (sink_->retainsUntilNext()) {
            // The previous buffer is free once the next async send has returned.
            if (held >= 0) ring_.requestRelease(held);
            held = f.slot;
        } else {
            ring_.requestRelease(f.slot);
        }

        sent_++;
        lastFrameId_ = v.frameId;
        rate_.tick();
        sendMs_.add(msBetween(t0, t1));
        latencyMs_.add(msBetween(f.meta.tick, t1));
        if (onDelivered) onDelivered(v.frameId);
    }
    sink_->flush();
    if (held >= 0) ring_.requestRelease(held);
    std::lock_guard l(m_);
    for (auto& f : ready_) ring_.requestRelease(f.slot);
    ready_.clear();
}

void OutputChannel::shutdown() {
    {
        std::lock_guard l(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    // Destroy the real sink now (the sender thread has flushed it): NDI won't
    // create a new sender under a name an old one still holds.
    std::lock_guard l(sinkMutex_);
    sink_ = createNullSink();
}

void OutputChannel::destroyGpuResources() {
    drainReleases();
    renderer_.destroyReadback(readback_);
    if (target_) renderer_.destroyTexture(target_);
    if (packed_) renderer_.destroyTexture(packed_);
    readback_ = target_ = packed_ = 0;
}

OutputMetrics OutputChannel::metrics() {
    OutputMetrics m;
    m.fps = rate_.rate();
    m.sent = sent_;
    m.droppedNoSlot = droppedNoSlot_;
    m.droppedQueue = droppedQueue_;
    {
        std::lock_guard l(m_);
        m.queueDepth = static_cast<int>(ready_.size());
    }
    m.slotsInUse = kSlots - ring_.count(SlotRing::Free);
    m.sendMs = sendMs_.value();
    m.sendMsMax = sendMs_.takeMax();
    m.latencyMs = latencyMs_.value();
    m.lastFrameId = lastFrameId_;
    {
        std::lock_guard l(sinkMutex_);
        m.connections = sink_->connections();
    }
    return m;
}

bool OutputChannel::snapshot(std::vector<uint8_t>& bgra, int& w, int& h, uint64_t& frameId, int timeoutMs) {
    std::unique_lock l(snapMutex_);
    snapWanted_ = true;
    snapReady_ = false;
    if (!snapCv_.wait_for(l, std::chrono::milliseconds(timeoutMs), [this] { return snapReady_; })) {
        snapWanted_ = false;
        return false;
    }
    bgra = snapData_;
    w = snapW_;
    h = snapH_;
    frameId = snapFrame_;
    return true;
}

}  // namespace mosaic
