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
// Receiving: a thread per source (shared by every canvas showing it) captures frames as
// BGRX/BGRA and keeps only the newest one; each canvas uploads it once, the last one frees it.
#include <Processing.NDI.Lib.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "audio/audio_ring.hpp"
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
        v.FourCC = f.format == FrameFormat::UYVY ? NDIlib_FourCC_video_type_UYVY : NDIlib_FourCC_video_type_BGRA;
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

// One NDI connection per source, shared by every canvas showing it: the source is received
// (and, for NDI|HX, decoded) once, however many canvases use it.
class NdiShared {
public:
    // Owns the receiver instance; frames hold it too, so it outlives the last frame in use.
    struct Handle {
        NDIlib_recv_instance_t recv = nullptr;
        ~Handle() {
            if (recv) NDIlib_recv_destroy(recv);
        }
    };
    struct Frame {
        std::shared_ptr<Handle> h;
        NDIlib_video_frame_v2_t v{};
        ~Frame() { NDIlib_recv_free_video_v2(h->recv, &v); }
    };

    explicit NdiShared(const std::string& sourceName) : name_(sourceName), h_(std::make_shared<Handle>()) {
        NDIlib_source_t src;
        src.p_ndi_name = name_.c_str();
        NDIlib_recv_create_v3_t desc;
        desc.source_to_connect_to = src;
        desc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
        desc.bandwidth = NDIlib_recv_bandwidth_highest;
        desc.allow_video_fields = false;
        desc.p_ndi_recv_name = "Mosaic Engine";
        h_->recv = NDIlib_recv_create_v3(&desc);
        if (h_->recv) thread_ = std::thread([this] { run(); });
    }
    ~NdiShared() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        std::lock_guard l(m_);
        latest_.reset();
    }

    bool ok() const { return h_->recv != nullptr; }
    // The newest frame and its number (0 = none yet).
    std::shared_ptr<Frame> latest(uint64_t& seq) {
        std::lock_guard l(m_);
        seq = seq_;
        return latest_;
    }
    bool connected() const { return Clock::now() - lastFrame_.load() < std::chrono::seconds(1); }
    double inputFps() const { return rate_.rate(); }
    uint64_t framesReceived() const { return received_; }
    double sentFps() const { return sentFps_; }
    // Frames the NDI library dropped because they weren't taken in time (we're too slow), and all frames.
    uint64_t dropped() const { return dropped_; }

private:
    void run() {
        auto lastPerf = Clock::now();
        while (!stop_) {
            NDIlib_video_frame_v2_t v;
            if (NDIlib_recv_capture_v3(h_->recv, &v, nullptr, nullptr, 100) == NDIlib_frame_type_video) {
                if (v.FourCC != NDIlib_FourCC_video_type_BGRA && v.FourCC != NDIlib_FourCC_video_type_BGRX) {  // not with BGRX_BGRA requested
                    NDIlib_recv_free_video_v2(h_->recv, &v);
                    continue;
                }
                received_++;
                rate_.tick();
                lastFrame_ = Clock::now();
                if (v.frame_rate_D > 0) sentFps_ = double(v.frame_rate_N) / v.frame_rate_D;
                auto f = std::make_shared<Frame>();
                f->h = h_;
                f->v = v;
                std::lock_guard l(m_);
                latest_ = std::move(f);  // latest wins: an older frame nobody took is freed
                ++seq_;
            }
            if (Clock::now() - lastPerf > std::chrono::seconds(1)) {
                lastPerf = Clock::now();
                NDIlib_recv_performance_t total{}, dropped{};
                NDIlib_recv_get_performance(h_->recv, &total, &dropped);
                dropped_ = static_cast<uint64_t>(dropped.video_frames);
            }
        }
    }

    std::string name_;
    std::shared_ptr<Handle> h_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex m_;
    std::shared_ptr<Frame> latest_;
    uint64_t seq_ = 0;
    std::atomic<uint64_t> received_{0}, dropped_{0};
    std::atomic<double> sentFps_{0};
    std::atomic<Clock::time_point> lastFrame_{Clock::time_point{}};
    RateMeter rate_;
};

// A canvas's view of a shared source: takes each new frame once.
class NdiInput final : public NdiReceiver {
public:
    explicit NdiInput(std::shared_ptr<NdiShared> shared) : s_(std::move(shared)) {}

    bool acquireLatest(InputFrame& out) override {
        uint64_t seq = 0;
        auto f = s_->latest(seq);
        if (!f || seq == taken_) return false;
        taken_ = seq;
        auto* held = new std::shared_ptr<NdiShared::Frame>(std::move(f));
        const auto& v = (*held)->v;
        out.data = v.p_data;
        out.width = v.xres;
        out.height = v.yres;
        out.stride = v.line_stride_in_bytes;
        out.sequence = seq;
        out.opaque = held;
        return true;
    }

    void release(InputFrame& f) override {
        delete static_cast<std::shared_ptr<NdiShared::Frame>*>(f.opaque);
        f.opaque = nullptr;
    }

    bool connected() const override { return s_->connected(); }
    double inputFps() const override { return s_->inputFps(); }
    uint64_t framesReceived() const override { return s_->framesReceived(); }
    double sentFps() const override { return s_->sentFps(); }
    uint64_t framesDropped() const override { return s_->dropped(); }

private:
    std::shared_ptr<NdiShared> s_;
    uint64_t taken_ = 0;
};

// Audio only: the source's audio, as float, into the ring with its arrival time.
class NdiAudio final : public NdiAudioInput {
public:
    NdiAudio(const std::string& sourceName, std::shared_ptr<AudioRing> ring) : name_(sourceName), ring_(std::move(ring)) {
        NDIlib_source_t src;
        src.p_ndi_name = name_.c_str();
        NDIlib_recv_create_v3_t desc;
        desc.source_to_connect_to = src;
        desc.bandwidth = NDIlib_recv_bandwidth_audio_only;
        desc.p_ndi_recv_name = "Mosaic Engine audio";
        recv_ = NDIlib_recv_create_v3(&desc);
        if (recv_) thread_ = std::thread([this] { run(); });
    }
    ~NdiAudio() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (recv_) NDIlib_recv_destroy(recv_);
    }
    bool ok() const { return recv_ != nullptr; }
    bool connected() const override { return Clock::now() - last_.load() < std::chrono::seconds(1); }
    int channels() const override { return channels_; }

private:
    void run() {
        while (!stop_) {
            NDIlib_audio_frame_v3_t a;
            if (NDIlib_recv_capture_v3(recv_, nullptr, &a, nullptr, 100) != NDIlib_frame_type_audio) continue;
            double arrived = steadySeconds();
            last_ = Clock::now();
            channels_ = a.no_channels;
            if (a.FourCC == NDIlib_FourCC_audio_type_FLTP && a.no_channels > 0 && a.no_samples > 0) {
                const auto* left = reinterpret_cast<const float*>(a.p_data);
                const auto* right = a.no_channels > 1 ? reinterpret_cast<const float*>(a.p_data + a.channel_stride_in_bytes) : left;
                ring_->push(left, right, a.no_samples, a.sample_rate, arrived);
            }
            NDIlib_recv_free_audio_v3(recv_, &a);
        }
    }

    std::string name_;
    std::shared_ptr<AudioRing> ring_;
    NDIlib_recv_instance_t recv_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<Clock::time_point> last_{Clock::time_point{}};
    std::atomic<int> channels_{0};
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
        std::lock_guard l(sharedMutex_);
        auto shared = shared_[sourceName].lock();
        if (!shared) {
            shared = std::make_shared<NdiShared>(sourceName);
            if (!shared->ok()) return nullptr;
            shared_[sourceName] = shared;
        }
        return std::make_unique<NdiInput>(shared);
    }

    std::unique_ptr<NdiAudioInput> createAudioReceiver(const std::string& sourceName, std::shared_ptr<AudioRing> ring) override {
        auto rx = std::make_unique<NdiAudio>(sourceName, std::move(ring));
        if (!rx->ok()) return nullptr;
        return rx;
    }

private:
    NDIlib_find_instance_t find_ = nullptr;
    std::mutex sharedMutex_;
    std::map<std::string, std::weak_ptr<NdiShared>> shared_;  // by source name
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
