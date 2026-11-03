#include "receiver.hpp"

#include <algorithm>
#include <iostream>

#include "aligner.hpp"
#include "ndi_input.hpp"
#include "synthetic_input.hpp"

using nlohmann::json;

namespace mosaic::rx {

static WindowSpec rectFrom(const json& j, WindowSpec def) {
    if (!j.is_object()) return def;
    return {j.value("x", def.x), j.value("y", def.y), j.value("w", def.w), j.value("h", def.h)};
}

void from_json(const json& j, ReceiverConfig& c) {
    c.lagFrames = std::clamp(j.value("lagFrames", c.lagFrames), 0, 30);
    c.lostAfterMs = std::clamp(j.value("lostAfterMs", c.lostAfterMs), 50, 10000);
    c.displays.clear();
    int n = 0;
    for (auto& d : j.value("displays", json::array())) {
        DisplayConfig dc;
        dc.index = d.value("index", ++n);
        dc.stream = d.value("stream", "");
        dc.label = d.value("label", "");
        dc.window = rectFrom(d.value("window", json::object()), dc.window);
        dc.tile = rectFrom(d.value("tile", json::object()), dc.tile);
        dc.synthetic = d.value("synthetic", json::object());
        c.displays.push_back(dc);
    }
}

json to_json(const ReceiverConfig& c) {
    json displays = json::array();
    for (auto& d : c.displays)
        displays.push_back({{"index", d.index},
                            {"stream", d.stream},
                            {"label", d.label},
                            {"window", {{"x", d.window.x}, {"y", d.window.y}, {"w", d.window.w}, {"h", d.window.h}}},
                            {"tile", {{"x", d.tile.x}, {"y", d.tile.y}, {"w", d.tile.w}, {"h", d.tile.h}}},
                            {"synthetic", d.synthetic}});
    return {{"displays", displays}, {"lagFrames", c.lagFrames}, {"lostAfterMs", c.lostAfterMs}};
}

static std::unique_ptr<FrameInput> makeInput(const DisplayConfig& d, std::string& error) {
    if (d.stream == "synthetic") {
        const json& o = d.synthetic;
        SyntheticOptions s;
        s.width = o.value("width", s.width);
        s.height = o.value("height", s.height);
        s.fps = o.value("fps", s.fps);
        s.delayMs = o.value("delayMs", s.delayMs);
        s.jitterMs = o.value("jitterMs", s.jitterMs);
        s.dropEvery = o.value("dropEvery", s.dropEvery);
        s.stallAtS = o.value("stallAtS", s.stallAtS);
        s.stallForS = o.value("stallForS", s.stallForS);
        s.colour = o.value("colour", s.colour);
        return std::make_unique<SyntheticInput>(s);
    }
    return createNdiInput(d.stream, error);
}

Receiver::Receiver(PresenterFactory factory) : factory_(std::move(factory)), thread_([this] { loop(); }) {}

Receiver::~Receiver() {
    stop_ = true;
    thread_.join();
}

void Receiver::setConfig(const ReceiverConfig& c) {
    std::lock_guard lock(configMutex_);
    config_ = c;
    configDirty_ = true;
}

ReceiverConfig Receiver::config() const {
    std::lock_guard lock(configMutex_);
    return config_;
}

json Receiver::status() const {
    std::lock_guard lock(statusMutex_);
    return status_;
}

void Receiver::loop() {
    presenter_ = factory_(presenterError_);
    if (!presenter_) {
        std::cerr << "[receiver] no presenter: " << presenterError_ << "\n";
        std::lock_guard lock(statusMutex_);
        status_ = {{"ok", false}, {"error", presenterError_}};
        return;
    }
    while (!stop_) {
        {
            std::unique_lock lock(configMutex_);
            if (configDirty_) {
                ReceiverConfig c = config_;
                configDirty_ = false;
                lock.unlock();
                apply(c);
            }
        }
        presenter_->waitForRefresh();
        frame();
    }
    streams_.clear();
    presenter_.reset();
}

// New displays: inputs for streams that are still used carry on (no reconnect); windows are rebuilt.
void Receiver::apply(const ReceiverConfig& c) {
    std::vector<Stream> next;
    displayStream_.clear();
    for (auto& d : c.displays) {
        std::string key = d.stream == "synthetic" ? "synthetic:" + d.synthetic.dump() : d.stream;
        auto have = std::find_if(next.begin(), next.end(), [&](auto& s) { return s.name == key; });
        if (have == next.end()) {
            Stream s;
            s.name = key;
            auto old = std::find_if(streams_.begin(), streams_.end(), [&](auto& o) { return o.name == key && o.input; });
            if (old != streams_.end()) s = std::move(*old);
            else if (!d.stream.empty()) s.input = makeInput(d, s.error);
            else s.error = "no stream assigned";
            next.push_back(std::move(s));
            have = next.end() - 1;
        }
        displayStream_.push_back(static_cast<int>(have - next.begin()));
    }
    streams_ = std::move(next);
    for (auto& s : streams_) s.shownHold = nullptr;  // textures are per stream index: upload again

    // One window per distinct monitor rectangle; displays sharing one become tiles of it.
    std::vector<WindowSpec> windows;
    tiles_.clear();
    for (size_t i = 0; i < c.displays.size(); ++i) {
        const auto& d = c.displays[i];
        auto w = std::find_if(windows.begin(), windows.end(), [&](auto& x) { return x.x == d.window.x && x.y == d.window.y && x.w == d.window.w && x.h == d.window.h; });
        if (w == windows.end()) windows.push_back(d.window), w = windows.end() - 1;
        TileDraw t;
        t.window = static_cast<int>(w - windows.begin());
        bool whole = d.tile.w <= 0 || d.tile.h <= 0;
        t.x = whole ? 0 : d.tile.x;
        t.y = whole ? 0 : d.tile.y;
        t.w = whole ? d.window.w : d.tile.w;
        t.h = whole ? d.window.h : d.tile.h;
        t.stream = displayStream_[i];
        tiles_.push_back(t);
    }
    std::string error;
    if (!presenter_->configure(windows, error)) std::cerr << "[receiver] " << error << "\n";
    active_ = c;
    target_ = 0;
    std::cerr << "[receiver] " << c.displays.size() << " display(s), " << streams_.size() << " stream(s), " << windows.size() << " window(s)\n";
}

void Receiver::frame() {
    auto now = Clock::now();
    const auto lost = std::chrono::milliseconds(active_.lostAfterMs);
    std::vector<StreamView> views(streams_.size());
    for (size_t i = 0; i < streams_.size(); ++i) {
        auto& s = streams_[i];
        if (!s.input) continue;
        auto& q = s.input->queue();
        views[i].newest = q.newestId();
        if (views[i].newest) s.everSynced = true;
        views[i].synced = s.everSynced;
        views[i].stale = q.lastArrival() == Clock::time_point{} || now - q.lastArrival() > lost;
    }
    Choice c = chooseFrames(views, target_, static_cast<uint64_t>(active_.lagFrames));
    target_ = c.target;

    uint64_t lo = UINT64_MAX, hi = 0;
    auto uploadStart = Clock::now();
    for (size_t i = 0; i < streams_.size(); ++i) {
        auto& s = streams_[i];
        if (!s.input) continue;
        auto f = s.input->queue().takeUpTo(c.show[i]);
        if (!f) continue;
        if (f->hold.get() != s.shownHold) {
            presenter_->upload(static_cast<int>(i), *f);
            s.shownHold = f->hold.get();
            s.shownId = f->frameId;
            s.width = f->width;
            s.height = f->height;
            ++s.shown;
        }
        // A stream catching up after a stall may only hold its old frame for a moment: it's late, not in step.
        bool inStep = views[i].synced && !views[i].stale && !c.lagging[i] && s.shownId + static_cast<uint64_t>(active_.lagFrames) >= c.target;
        if (views[i].synced && !views[i].stale && (c.lagging[i] || (c.target && s.shownId != c.target))) ++s.late;
        if (inStep) lo = std::min(lo, s.shownId), hi = std::max(hi, s.shownId);
    }
    windowUploadMs_ += std::chrono::duration<double, std::milli>(Clock::now() - uploadStart).count();
    presenter_->present(tiles_);

    // Real rates, once a second: how often we presented, how many new pictures each stream showed.
    ++windowPresents_;
    double span = std::chrono::duration<double>(now - windowStart_).count();
    if (windowShown_.size() != streams_.size()) windowShown_.assign(streams_.size(), 0), streamFps_.assign(streams_.size(), 0);
    if (span >= 1.0) {
        presentFps_ = windowPresents_ / span;
        uploadMs_ = windowUploadMs_ / static_cast<double>(windowPresents_);
        for (size_t i = 0; i < streams_.size(); ++i) {
            streamFps_[i] = (streams_[i].shown - windowShown_[i]) / span;
            windowShown_[i] = streams_[i].shown;
        }
        windowStart_ = now;
        windowPresents_ = 0;
        windowUploadMs_ = 0;
    }

    // Spread: how many frames apart the in-step displays are this refresh (0 = all the same frame).
    ++presents_;
    recentSpread_.push_back(lo != UINT64_MAX ? hi - lo : 0);
    if (lo != UINT64_MAX && streams_.size() > 1) {
        ++comparedPresents_;
        if (hi == lo) ++alignedPresents_;
    }
    size_t keep = static_cast<size_t>(presenter_->refreshHz() * 10);
    if (recentSpread_.size() > keep) recentSpread_.erase(recentSpread_.begin(), recentSpread_.end() - static_cast<std::ptrdiff_t>(keep));

    if (presents_ % 6 != 0) return;  // status ~10 times a second is plenty
    json displays = json::array();
    for (size_t i = 0; i < active_.displays.size(); ++i) {
        const auto& d = active_.displays[i];
        const auto& s = streams_[displayStream_[i]];
        displays.push_back({{"index", d.index},
                            {"label", d.label},
                            {"stream", d.stream},
                            {"status", s.input ? s.input->status() : s.error},
                            {"shownFrameId", s.shownId},
                            {"late", s.late},
                            {"framesShown", s.shown},
                            {"fps", displayStream_[i] < static_cast<int>(streamFps_.size()) ? streamFps_[displayStream_[i]] : 0.0},
                            {"received", s.input ? s.input->queue().received() : 0},
                            {"width", s.width},
                            {"height", s.height}});
    }
    uint64_t spreadMax = recentSpread_.empty() ? 0 : *std::max_element(recentSpread_.begin(), recentSpread_.end());
    std::lock_guard lock(statusMutex_);
    status_ = {{"ok", true},
               {"presenter", presenter_->name()},
               {"refreshHz", presenter_->refreshHz()},
               {"presentFps", presentFps_},
               {"uploadMs", uploadMs_},
               {"uptimeS", std::chrono::duration<double>(now - started_).count()},
               {"presents", presents_},
               {"targetFrameId", target_},
               {"spread", recentSpread_.empty() ? 0 : recentSpread_.back()},
               {"spreadMax10s", spreadMax},
               {"alignedRatio", comparedPresents_ ? double(alignedPresents_) / double(comparedPresents_) : 1.0},
               {"displays", displays}};
}

}  // namespace mosaic::rx
