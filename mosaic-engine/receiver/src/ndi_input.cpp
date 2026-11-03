// NDI receiving for the receiver (NDI SDK 5/6). Frames stay in NDI's memory until the
// render thread is done with them: each queued frame holds the NDI frame, and the receiver
// instance lives until the last of its frames is freed.
#include "ndi_input.hpp"

#include <Processing.NDI.Lib.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace mosaic::rx {

uint64_t parseFrameId(const char* metadata) {
    const char* p = metadata ? std::strstr(metadata, "frame_id=\"") : nullptr;
    return p ? std::strtoull(p + 10, nullptr, 10) : 0;
}

namespace {

struct Recv {
    NDIlib_recv_instance_t instance = nullptr;
    ~Recv() {
        if (instance) NDIlib_recv_destroy(instance);
    }
};

class NdiInput final : public FrameInput {
public:
    explicit NdiInput(const std::string& sourceName) : name_(sourceName), recv_(std::make_shared<Recv>()) {
        NDIlib_source_t src;
        src.p_ndi_name = name_.c_str();
        NDIlib_recv_create_v3_t desc;
        desc.source_to_connect_to = src;
        // UYVY: what the engine sends, so NDI doesn't convert on the CPU; half the bytes of BGRA to upload.
        desc.color_format = NDIlib_recv_color_format_UYVY_BGRA;
        desc.bandwidth = NDIlib_recv_bandwidth_highest;
        desc.allow_video_fields = false;
        desc.p_ndi_recv_name = "Mosaic Receiver";
        recv_->instance = NDIlib_recv_create_v3(&desc);
        if (recv_->instance) thread_ = std::thread([this] { run(); });
    }

    ~NdiInput() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        queue_.clear();  // queued frames free themselves; the receiver goes with the last one
    }

    bool ok() const { return recv_->instance != nullptr; }
    StreamQueue& queue() override { return queue_; }
    std::string status() const override {
        auto last = queue_.lastArrival();
        if (last == Clock::time_point{}) return "waiting for source";
        return Clock::now() - last < std::chrono::seconds(1) ? "ok" : "no signal";
    }

private:
    void run() {
        while (!stop_) {
            NDIlib_video_frame_v2_t v;
            if (NDIlib_recv_capture_v3(recv_->instance, &v, nullptr, nullptr, 100) != NDIlib_frame_type_video) continue;
            bool uyvy = v.FourCC == NDIlib_FourCC_video_type_UYVY;
            if (!uyvy && v.FourCC != NDIlib_FourCC_video_type_BGRA && v.FourCC != NDIlib_FourCC_video_type_BGRX) {
                NDIlib_recv_free_video_v2(recv_->instance, &v);  // not expected with UYVY_BGRA requested
                continue;
            }
            RxFrame f;
            f.frameId = parseFrameId(v.p_metadata);
            f.timecode = v.timecode;
            f.width = v.xres;
            f.height = v.yres;
            f.stride = v.line_stride_in_bytes;
            f.data = v.p_data;
            f.uyvy = uyvy;
            f.arrived = Clock::now();
            auto recv = recv_;
            f.hold = std::shared_ptr<void>(new NDIlib_video_frame_v2_t(v), [recv](void* p) {
                auto* frame = static_cast<NDIlib_video_frame_v2_t*>(p);
                NDIlib_recv_free_video_v2(recv->instance, frame);
                delete frame;
            });
            queue_.push(std::move(f));
        }
    }

    std::string name_;
    std::shared_ptr<Recv> recv_;
    StreamQueue queue_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace

std::unique_ptr<FrameInput> createNdiInput(const std::string& sourceName, std::string& whyNot) {
    static const bool initialised = NDIlib_initialize();
    if (!initialised) {
        whyNot = "NDI runtime missing or CPU not supported";
        return nullptr;
    }
    auto in = std::make_unique<NdiInput>(sourceName);
    if (!in->ok()) {
        whyNot = "cannot create an NDI receiver";
        return nullptr;
    }
    return in;
}

}  // namespace mosaic::rx
