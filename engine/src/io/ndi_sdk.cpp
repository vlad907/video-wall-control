// NDI implementation of the video I/O interfaces (NDI SDK 5/6).
//
// STATUS: written against the documented NDI SDK C API, but NOT compiled or
// tested here because the SDK was not installed on the development machine.
// It is only built when CMake finds the SDK. See docs/NDI.md.
//
// Sending: NDIlib_send_send_video_async_v2 with BGRA frames that point straight
// at the GPU readback buffer. NDI keeps using that buffer until the *next*
// async call returns, which is why VideoSink::retainsUntilNext() is true and
// the output channel holds one slot back. NDI copies/encodes on its own
// threads; there is no API for submitting GPU textures directly.
//
// Receiving: a thread per source captures frames as BGRX/BGRA and keeps only
// the newest one; the render thread uploads it once and frees it.
#include <Processing.NDI.Lib.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>

#include "core/metrics.hpp"
#include "io/video_io.hpp"

namespace mosaic {

namespace {

class NdiSender final : public VideoSink {
public:
    explicit NdiSender(NDIlib_send_instance_t inst) : inst_(inst) {}
    ~NdiSender() override {
        flush();
        NDIlib_send_destroy(inst_);
    }

    std::string kind() const override { return "ndi"; }
    bool retainsUntilNext() const override { return true; }

    void send(const VideoFrameView& f) override {
        // Metadata carries the engine frame id so receivers/tools can verify
        // that every output came from the same rendered frame. It must stay
        // valid as long as the frame, hence two alternating strings.
        std::string& meta = meta_[next_ ^= 1];
        meta = "<mosaic frame_id=\"" + std::to_string(f.frameId) + "\"/>";

        NDIlib_video_frame_v2_t v;
        v.xres = f.width;
        v.yres = f.height;
        v.FourCC = NDIlib_FourCC_video_type_BGRA;
        v.frame_rate_N = static_cast<int>(std::lround(f.fps * 1000.0));
        v.frame_rate_D = 1000;
        v.picture_aspect_ratio = static_cast<float>(f.width) / static_cast<float>(f.height);
        v.frame_format_type = NDIlib_frame_format_type_progressive;
        v.timecode = f.timecode;  // same value on every output of a frame
        v.p_data = const_cast<uint8_t*>(f.data);
        v.line_stride_in_bytes = f.stride;
        v.p_metadata = meta.c_str();
        NDIlib_send_send_video_async_v2(inst_, &v);
    }

    void flush() override {
        // A null frame waits until NDI has released the last buffer.
        NDIlib_send_send_video_async_v2(inst_, nullptr);
    }

    int connections() const override { return NDIlib_send_get_no_connections(inst_, 0); }

private:
    NDIlib_send_instance_t inst_;
    std::string meta_[2];
    int next_ = 0;
};

class NdiInput final : public NdiReceiver {
public:
    explicit NdiInput(const std::string& sourceName) : name_(sourceName) {
        NDIlib_source_t src;
        src.p_ndi_name = name_.c_str();
        NDIlib_recv_create_v3_t desc;
        desc.source_to_connect_to = src;
        desc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
        desc.bandwidth = NDIlib_recv_bandwidth_highest;
        desc.allow_video_fields = false;
        desc.p_ndi_recv_name = "Mosaic Engine";
        recv_ = NDIlib_recv_create_v3(&desc);
        if (recv_) thread_ = std::thread([this] { run(); });
    }

    ~NdiInput() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        std::lock_guard l(m_);
        if (hasLatest_) NDIlib_recv_free_video_v2(recv_, &latest_);
        if (recv_) NDIlib_recv_destroy(recv_);
    }

    bool ok() const { return recv_ != nullptr; }

    bool acquireLatest(InputFrame& out) override {
        std::lock_guard l(m_);
        if (!hasLatest_) return false;
        auto* held = new NDIlib_video_frame_v2_t(latest_);
        hasLatest_ = false;
        out.data = held->p_data;
        out.width = held->xres;
        out.height = held->yres;
        out.stride = held->line_stride_in_bytes;
        out.sequence = ++sequence_;
        out.opaque = held;
        return true;
    }

    void release(InputFrame& f) override {
        auto* held = static_cast<NDIlib_video_frame_v2_t*>(f.opaque);
        if (held) NDIlib_recv_free_video_v2(recv_, held);
        delete held;
        f.opaque = nullptr;
    }

    bool connected() const override {
        return Clock::now() - lastFrame_.load() < std::chrono::seconds(1);
    }
    double inputFps() const override { return rate_.rate(); }
    uint64_t framesReceived() const override { return received_; }

private:
    void run() {
        while (!stop_) {
            NDIlib_video_frame_v2_t v;
            switch (NDIlib_recv_capture_v3(recv_, &v, nullptr, nullptr, 100)) {
                case NDIlib_frame_type_video: {
                    bool bgra = v.FourCC == NDIlib_FourCC_video_type_BGRA || v.FourCC == NDIlib_FourCC_video_type_BGRX;
                    if (!bgra) {  // should not happen with BGRX_BGRA requested
                        NDIlib_recv_free_video_v2(recv_, &v);
                        break;
                    }
                    received_++;
                    rate_.tick();
                    lastFrame_ = Clock::now();
                    std::lock_guard l(m_);
                    // Latest wins: an unconsumed older frame is dropped.
                    if (hasLatest_) NDIlib_recv_free_video_v2(recv_, &latest_);
                    latest_ = v;
                    hasLatest_ = true;
                    break;
                }
                default:
                    break;
            }
        }
    }

    std::string name_;
    NDIlib_recv_instance_t recv_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex m_;
    NDIlib_video_frame_v2_t latest_;
    bool hasLatest_ = false;
    uint64_t sequence_ = 0;
    std::atomic<uint64_t> received_{0};
    std::atomic<Clock::time_point> lastFrame_{Clock::time_point{}};
    RateMeter rate_;
};

class NdiSystemImpl final : public NdiSystem {
public:
    NdiSystemImpl() {
        NDIlib_find_create_t desc;
        desc.show_local_sources = true;
        find_ = NDIlib_find_create_v2(&desc);
    }
    ~NdiSystemImpl() override {
        if (find_) NDIlib_find_destroy(find_);
        NDIlib_destroy();
    }

    std::string version() const override { return NDIlib_version(); }

    std::vector<std::string> discoveredSources() override {
        std::vector<std::string> names;
        if (!find_) return names;
        std::lock_guard l(m_);
        uint32_t n = 0;
        const NDIlib_source_t* list = NDIlib_find_get_current_sources(find_, &n);
        for (uint32_t i = 0; i < n; ++i)
            if (list[i].p_ndi_name) names.emplace_back(list[i].p_ndi_name);
        return names;
    }

    std::unique_ptr<VideoSink> createSender(const std::string& streamName) override {
        NDIlib_send_create_t desc;
        desc.p_ndi_name = streamName.c_str();
        desc.clock_video = false;  // the engine paces frames itself
        desc.clock_audio = false;
        NDIlib_send_instance_t inst = NDIlib_send_create(&desc);
        if (!inst) return nullptr;
        return std::make_unique<NdiSender>(inst);
    }

    std::unique_ptr<NdiReceiver> createReceiver(const std::string& sourceName) override {
        auto rx = std::make_unique<NdiInput>(sourceName);
        if (!rx->ok()) return nullptr;
        return rx;
    }

private:
    NDIlib_find_instance_t find_ = nullptr;
    std::mutex m_;
};

}  // namespace

std::unique_ptr<NdiSystem> createNdiSystem(std::string& whyNot) {
    if (!NDIlib_initialize()) {
        whyNot = "NDIlib_initialize failed (unsupported CPU or NDI runtime missing)";
        return nullptr;
    }
    return std::make_unique<NdiSystemImpl>();
}

}  // namespace mosaic
