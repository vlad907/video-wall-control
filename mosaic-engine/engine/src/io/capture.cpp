#include "io/capture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "core/metrics.hpp"

namespace mosaic {

namespace {

// One open device: its newest frame, shared by every viewer.
struct Session {
    std::string name;
    std::mutex m;
    std::shared_ptr<std::vector<uint8_t>> frame;  // BGRA, width * 4 per row
    int width = 0, height = 0;
    uint64_t sequence = 0;
    Clock::time_point lastFrame{};
    RateMeter rate;
    std::unique_ptr<CaptureStream> stream;  // platform device
    std::thread testThread;                 // the test device
    std::atomic<bool> stop{false};
    int viewers = 0;

    void deliver(const uint8_t* bgra, int w, int h, long stride) {
        auto f = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            std::memcpy(f->data() + static_cast<size_t>(y) * w * 4, bgra + static_cast<std::ptrdiff_t>(y) * stride, static_cast<size_t>(w) * 4);
        std::lock_guard l(m);
        frame = std::move(f);
        width = w;
        height = h;
        ++sequence;
        lastFrame = Clock::now();
        rate.tick();
    }

    ~Session() {
        stop = true;
        if (testThread.joinable()) testThread.join();
        stream.reset();
    }
};

std::mutex g_mutex;
std::map<std::string, std::weak_ptr<Session>> g_sessions;

// The test device: 1280 x 720 at 60 fps, colour bars with a bar moving across and a frame counter
// stripe, so dropped or repeated frames show.
void runTestDevice(Session* s) {
    const int w = 1280, h = 720;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    const uint8_t bars[8][3] = {{255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0}, {255, 0, 255}, {0, 0, 255}, {255, 0, 0}, {0, 0, 0}};  // BGR
    auto next = Clock::now();
    for (uint64_t n = 0; !s->stop; ++n) {
        int moving = static_cast<int>((n * 8) % w);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
                const uint8_t* c = bars[x * 8 / w];
                bool bar = x >= moving && x < moving + 24;
                bool counter = y >= h - 40 && ((n >> ((x * 16 / w) & 15)) & 1);
                p[0] = bar || counter ? 255 : c[0];
                p[1] = bar || counter ? 255 : c[1];
                p[2] = bar || counter ? 255 : c[2];
                p[3] = 255;
            }
        s->deliver(px.data(), w, h, w * 4);
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
}

// A viewer: takes the session's newest frame when it's new to it.
class CaptureViewer final : public NdiReceiver {
public:
    explicit CaptureViewer(std::shared_ptr<Session> s) : s_(std::move(s)) {
        std::lock_guard l(s_->m);
        ++s_->viewers;
    }
    ~CaptureViewer() override {
        std::lock_guard l(s_->m);
        --s_->viewers;
    }
    bool acquireLatest(InputFrame& out) override {
        std::shared_ptr<std::vector<uint8_t>> f;
        int w, h;
        uint64_t seq;
        {
            std::lock_guard l(s_->m);
            if (!s_->frame || s_->sequence == seen_) return false;
            f = s_->frame;
            w = s_->width;
            h = s_->height;
            seq = seen_ = s_->sequence;
        }
        out.data = f->data();
        out.width = w;
        out.height = h;
        out.stride = w * 4;
        out.sequence = seq;
        out.opaque = new std::shared_ptr<std::vector<uint8_t>>(std::move(f));  // keeps the pixels until release()
        ++received_;
        return true;
    }
    void release(InputFrame& f) override {
        delete static_cast<std::shared_ptr<std::vector<uint8_t>>*>(f.opaque);
        f.opaque = nullptr;
    }
    bool connected() const override {
        std::lock_guard l(s_->m);
        return Clock::now() - s_->lastFrame < std::chrono::seconds(1);
    }
    double inputFps() const override {
        std::lock_guard l(s_->m);
        return s_->rate.rate();
    }
    uint64_t framesReceived() const override { return received_; }

private:
    std::shared_ptr<Session> s_;
    uint64_t seen_ = 0;
    uint64_t received_ = 0;
};

}  // namespace

std::vector<CaptureDevice> listCaptureDevices(std::string& error) {
    auto list = platformCaptureDevices(error);
    list.push_back({kTestCaptureName, "test"});
    return list;
}

std::unique_ptr<NdiReceiver> openCapture(const std::string& name, std::string& error) {
    std::lock_guard l(g_mutex);
    auto& weak = g_sessions[name];
    auto s = weak.lock();
    if (!s) {
        s = std::make_shared<Session>();
        s->name = name;
        if (name == kTestCaptureName) {
            Session* raw = s.get();
            s->testThread = std::thread([raw] { runTestDevice(raw); });
        } else {
            Session* raw = s.get();
            s->stream = openPlatformCapture(name, [raw](const uint8_t* p, int w, int h, long stride) { raw->deliver(p, w, h, stride); }, error);
            if (!s->stream) {
                g_sessions.erase(name);
                return nullptr;
            }
        }
        weak = s;
    }
    return std::make_unique<CaptureViewer>(s);
}

std::vector<CaptureStatus> captureStatus() {
    std::vector<CaptureStatus> out;
    std::lock_guard l(g_mutex);
    for (auto it = g_sessions.begin(); it != g_sessions.end();) {
        auto s = it->second.lock();
        if (!s) {
            it = g_sessions.erase(it);
            continue;
        }
        CaptureStatus st;
        st.name = s->name;
        if (s->stream) st.error = s->stream->error();
        {
            std::lock_guard sl(s->m);
            st.width = s->width;
            st.height = s->height;
            st.viewers = s->viewers;
            st.fps = s->rate.rate();
        }
        out.push_back(st);
        ++it;
    }
    return out;
}

}  // namespace mosaic
