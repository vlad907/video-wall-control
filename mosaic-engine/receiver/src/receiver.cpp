#include "receiver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>

#include "aligner.hpp"
#include "ndi_input.hpp"
#include "synthetic_input.hpp"

using nlohmann::json;

namespace mosaic::rx {

static double median(std::deque<double> v) {
    if (v.empty()) return 0;
    auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

std::string localTime(std::chrono::system_clock::time_point t) {
    std::time_t secs = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char day[32], out[48];
    std::strftime(day, sizeof day, "%Y-%m-%d %H:%M:%S", &tm);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000;
    std::snprintf(out, sizeof out, "%s.%03d", day, static_cast<int>(ms));
    return out;
}

static WindowSpec rectFrom(const json& j, WindowSpec def) {
    if (!j.is_object()) return def;
    return {j.value("x", def.x), j.value("y", def.y), j.value("w", def.w), j.value("h", def.h)};
}

void from_json(const json& j, ReceiverConfig& c) {
    c.lagFrames = std::clamp(j.value("lagFrames", c.lagFrames), 0, 30);
    c.lostAfterMs = std::clamp(j.value("lostAfterMs", c.lostAfterMs), 50, 10000);
    c.flashTest = j.value("flashTest", c.flashTest);
    c.probe = j.value("probe", c.probe);
    c.compensate = j.value("compensate", c.compensate);
    c.bufferMs = std::clamp(j.value("bufferMs", c.bufferMs), 0, 1000);
    c.displays.clear();
    int n = 0;
    for (auto& d : j.value("displays", json::array())) {
        DisplayConfig dc;
        dc.index = d.value("index", ++n);
        dc.stream = d.value("stream", "");
        dc.label = d.value("label", "");
        dc.window = rectFrom(d.value("window", json::object()), dc.window);
        dc.tile = rectFrom(d.value("tile", json::object()), dc.tile);
        if (d.contains("crop") && d.at("crop").is_object()) {
            const auto& cr = d.at("crop");
            dc.cropX = std::clamp(cr.value("x", 0.0), 0.0, 0.99);
            dc.cropY = std::clamp(cr.value("y", 0.0), 0.0, 0.99);
            dc.cropW = std::clamp(cr.value("w", 1.0), 0.01, 1.0 - dc.cropX);
            dc.cropH = std::clamp(cr.value("h", 1.0), 0.01, 1.0 - dc.cropY);
        }
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
                            {"crop", {{"x", d.cropX}, {"y", d.cropY}, {"w", d.cropW}, {"h", d.cropH}}},
                            {"synthetic", d.synthetic}});
    return {{"displays", displays}, {"lagFrames", c.lagFrames}, {"lostAfterMs", c.lostAfterMs}, {"flashTest", c.flashTest}, {"compensate", c.compensate}, {"bufferMs", c.bufferMs}};
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
        s.stallLate = o.value("stallLate", s.stallLate);
        s.colour = o.value("colour", s.colour);
        s.quadrants = o.value("quadrants", s.quadrants);
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
        auto waitStart = Clock::now();
        presenter_->waitForRefresh();
        waitMs_ = std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count();
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
    // Room for the buffer's worth of frames at up to 60 fps, plus the usual few.
    const size_t capacity = 8 + static_cast<size_t>(std::ceil(c.bufferMs * 60.0 / 1000.0));
    for (auto& s : streams_) {
        s.shownHold = nullptr;  // textures are per stream index: upload again
        if (s.input) s.input->queue().setCapacity(capacity);
    }

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
        t.cx = d.cropX;
        t.cy = d.cropY;
        t.cw = d.cropW;
        t.ch = d.cropH;
        tiles_.push_back(t);
    }
    std::string error;
    if (!presenter_->configure(windows, error)) std::cerr << "[receiver] " << error << "\n";
    presenter_->setProbe(c.probe);
    windows_.assign(windows.size(), {});
    windowStream_.assign(windows.size(), -1);
    drawn_.assign(windows.size(), UINT64_MAX);  // every window is drawn once
    tileGen_.clear();
    busySince_.assign(windows.size(), {});
    for (auto& t : tiles_)
        if (windowStream_[t.window] < 0) windowStream_[t.window] = t.stream;
    shownAt_.clear();
    streamGap_.assign(streams_.size(), {});
    lastPresent_ = {};  // rebuilding the windows isn't a freeze
    spreads_.clear();
    recentSpreads_.clear();
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
        // Network freeze: nothing from the stream for over 100 ms (3 frames at 30 fps), logged once it sends again.
        auto last = q.lastArrival();
        if (!s.everSynced || last == Clock::time_point{} || i >= streamGap_.size()) continue;
        if (streamGap_[i] == Clock::time_point{}) {
            if (now - last > std::chrono::milliseconds(100)) streamGap_[i] = last;
        } else if (last > streamGap_[i]) {
            double ms = std::chrono::duration<double, std::milli>(last - streamGap_[i]).count();
            if (ms < active_.bufferMs) ++gapsAbsorbed_;  // the buffer had enough frames in hand: not seen on screen
            else hiccup("network", ms, "no frames from " + s.name + " for " + std::to_string(std::lround(ms)) + " ms");
            streamGap_[i] = {};
        }
    }
    trackEngineClock(now);
    Choice c;
    if (active_.bufferMs > 0 && engineClockKnown_) {
        // Buffered: the frame due now is the newest the engine made at least `bufferMs` (plus the
        // fastest transit) ago. Every stream that has it shows it; one that doesn't (stalled for
        // longer than the buffer) shows its newest and counts as late.
        int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        int64_t dueTimecode = (nowUs - engineOffsetUs_ - active_.bufferMs * 1000LL) * 10;
        uint64_t due = 0;
        for (size_t i = 0; i < streams_.size(); ++i)
            if (streams_[i].input && views[i].synced && !views[i].stale) due = std::max(due, streams_[i].input->queue().newestIdAtOrBefore(dueTimecode));
        c.show.assign(streams_.size(), 0);
        c.lagging.assign(streams_.size(), false);
        c.target = std::max(due, target_);
        const uint64_t pick = c.target ? c.target : 1;  // nothing due yet: nothing new (engine frame ids are larger)
        for (size_t i = 0; i < streams_.size(); ++i)
            if (views[i].synced && !views[i].stale) c.show[i] = pick;
    } else {
        c = chooseFrames(views, target_, static_cast<uint64_t>(active_.lagFrames));
    }
    target_ = c.target;

    uint64_t lo = UINT64_MAX, hi = 0;
    auto uploadStart = Clock::now();
    std::vector<RxFrame> fresh;
    std::vector<Upload> uploads;
    fresh.reserve(streams_.size());
    for (size_t i = 0; i < streams_.size(); ++i) {
        auto& s = streams_[i];
        if (!s.input) continue;
        auto f = s.input->queue().takeUpTo(c.show[i]);
        if (!f) continue;
        if (f->hold.get() != s.shownHold) {
            s.shownHold = f->hold.get();
            s.shownId = f->frameId;
            s.width = f->width;
            s.height = f->height;
            ++s.shown;
            s.history.push_back({now, ++s.gen, f->frameId});
            if (s.history.size() > static_cast<size_t>(kPictureHistory)) s.history.pop_front();
            windowHeldMs_ += std::chrono::duration<double, std::milli>(now - f->arrived).count();
            ++windowHeldCount_;
            fresh.push_back(std::move(*f));
            uploads.push_back({static_cast<int>(i), &fresh.back()});
        }
        // A stream catching up after a stall may only hold its old frame for a moment: it's late, not in step.
        bool inStep = views[i].synced && !views[i].stale && !c.lagging[i] && s.shownId + static_cast<uint64_t>(active_.lagFrames) >= c.target;
        if (views[i].synced && !views[i].stale && (c.lagging[i] || (c.target && s.shownId != c.target))) ++s.late;
        if (inStep) lo = std::min(lo, s.shownId), hi = std::max(hi, s.shownId);
    }
    presenter_->upload(uploads);
    lastUploadMs_ = std::chrono::duration<double, std::milli>(Clock::now() - uploadStart).count();
    windowUploadMs_ += lastUploadMs_;

    // Compensation: a display held back `hold` refreshes shows the picture that was newest that
    // long ago (half a refresh of slack absorbs loop jitter).
    const auto period = std::chrono::duration<double>(1.0 / presenter_->refreshHz());
    std::vector<TileDraw> draw = tiles_;
    // What each window would show (its tiles' pictures): only windows where that changed are
    // drawn and presented, so a refresh costs only the displays that change in it.
    std::vector<uint64_t> look(windows_.size(), 0);
    auto mix = [](uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ULL; };
    if (tileGen_.size() != draw.size()) tileGen_.assign(draw.size(), 0), tileSkipped_.assign(draw.size(), false);
    for (size_t k = 0; k < draw.size(); ++k) {
        auto& t = draw[k];
        if (t.window >= static_cast<int>(windows_.size())) continue;
        auto& win = windows_[t.window];
        uint64_t gen = 0;
        if (t.stream >= 0 && !streams_[t.stream].history.empty()) {
            const auto& s = streams_[t.stream];
            const auto holdBack = std::chrono::duration_cast<Clock::duration>(period * (win.hold - 0.5));
            size_t pick = 0;
            for (size_t j = 0; j < s.history.size(); ++j)
                if (win.hold == 0 || s.history[j].at + holdBack <= now) pick = j;
            // A held display more than a refresh behind (the loop fell behind) with a newer
            // picture due by the next refresh keeps what it shows and takes the newer one then,
            // instead of presenting a stale picture alongside the others' new one: presenting
            // every display in one refresh is what made the loop fall behind. Once only: a display
            // skipped last refresh takes what's due now.
            bool skip = false;
            if (win.hold > 0 && !tileSkipped_[k] && pick + 1 < s.history.size() && now - (s.history[pick].at + holdBack) > period &&
                s.history[pick + 1].at + holdBack <= now + period) {
                auto shown = std::find_if(s.history.begin(), s.history.end(), [&](auto& p) { return p.gen == tileGen_[k]; });
                if (shown != s.history.end()) pick = static_cast<size_t>(shown - s.history.begin()), skip = true;
            }
            tileSkipped_[k] = skip;
            t.back = static_cast<int>(s.gen - s.history[pick].gen);
            gen = s.history[pick].gen;
            if (windowStream_[t.window] == t.stream) win.frameId = s.history[pick].frameId;
        }
        look[t.window] = mix(mix(look[t.window], static_cast<uint64_t>(t.stream + 1)), gen);
        tileWant_.resize(draw.size());
        tileWant_[k] = gen;
    }
    // Flash test: a window goes white while it shows an engine frame divisible by 30.
    std::vector<bool> flash(windows_.size(), false);
    for (size_t w = 0; w < windows_.size() && active_.flashTest; ++w) flash[w] = windows_[w].frameId && windows_[w].frameId % 30 == 0;
    std::vector<bool> which(windows_.size(), false);
    for (size_t w = 0; w < windows_.size(); ++w) {
        look[w] = mix(look[w], flash[w] ? 1 : 0);
        which[w] = look[w] != drawn_[w];
    }
    // Budget: if presenting every window wanted would run past this refresh (so the next one is
    // missed and every display stutters), the held-back displays wait one refresh instead.
    auto presentStart = Clock::now();
    {
        double usedMs = std::chrono::duration<double, std::milli>(presentStart - now).count();
        int wanted = static_cast<int>(std::count(which.begin(), which.end(), true));
        if (wanted > 1 && usedMs + wanted * msPerWindow_ > 0.8 * 1000.0 / presenter_->refreshHz()) {
            for (size_t w = 0; w < windows_.size(); ++w)
                if (which[w] && windows_[w].hold > 0 && std::count(which.begin(), which.end(), true) > 1) which[w] = false, ++deferred_;
        }
    }
    presenter_->present(draw, flash, which);
    auto presentedAt = Clock::now();
    presentMs_ = std::chrono::duration<double, std::milli>(presentedAt - presentStart).count();
    screenSkipped_ = 0;
    auto timings = presenter_->timings();
    {
        int n = 0;
        for (size_t w = 0; w < timings.size(); ++w) n += timings[w].presented ? 1 : 0;
        if (n) msPerWindow_ = msPerWindow_ ? 0.9 * msPerWindow_ + 0.1 * (presentMs_ / n) : presentMs_ / n;  // what one window's present costs, smoothed
    }
    const double periodMs = 1000.0 / presenter_->refreshHz();
    for (size_t w = 0; w < windows_.size(); ++w) {
        if (!which[w]) continue;
        bool done = w < timings.size() && timings[w].presented;
        if (!done) {
            if (busySince_[w] == Clock::time_point{}) busySince_[w] = presentStart;
            continue;
        }
        drawn_[w] = look[w];
        for (size_t k = 0; k < draw.size(); ++k)
            if (draw[k].window == static_cast<int>(w)) tileGen_[k] = tileWant_[k];
        if (busySince_[w] != Clock::time_point{}) {
            // Its swap chain didn't take the new picture for a while (Windows / the GPU behind on that monitor).
            double ms = std::chrono::duration<double, std::milli>(presentedAt - busySince_[w]).count() + periodMs;
            if (ms >= 2.5 * periodMs) hiccup("wait", ms, "window " + std::to_string(w) + "'s screen didn't take a new picture for " + std::to_string(std::lround(ms)) + " ms");
            busySince_[w] = {};
        }
        ++windowPresentCount_;
    }
    trackTiming(timings);

    // Loop freeze: this pass (one per refresh, presenting whatever changed) came 2.5 refreshes or
    // more after the last one. The slowest step (if it took over a refresh) is what held it up.
    if (lastPresent_ != Clock::time_point{}) {
        double gap = std::chrono::duration<double, std::milli>(presentedAt - lastPresent_).count();
        if (gap >= 2.5 * periodMs) {
            std::pair<double, const char*> steps[] = {{waitMs_, "wait"}, {lastUploadMs_, "upload"}, {presentMs_, "present"}};
            auto slowest = *std::max_element(std::begin(steps), std::end(steps));
            char detail[160];
            std::snprintf(detail, sizeof detail, "no pass for %.0f ms (refresh wait %.0f ms, upload %.0f ms, present %.0f ms)", gap, waitMs_,
                          lastUploadMs_, presentMs_);
            hiccup(slowest.first > periodMs ? slowest.second : "loop", gap, detail);
            lastLoopHiccup_ = presentedAt;
        } else if (screenSkipped_ >= 2 && presentedAt - lastLoopHiccup_ > std::chrono::milliseconds(500)) {
            // The monitors' statistics trail by a few presents, so skips right after a loop freeze are that freeze.
            hiccup("screen", screenSkipped_ * periodMs,
                   "a monitor showed a picture " + std::to_string(screenSkipped_) + " refreshes later than usual though the receiver presented on time");
        }
    }
    lastPresent_ = presentedAt;

    // Real rates, once a second: how often we presented, how many new pictures each stream showed.
    ++windowPresents_;
    double span = std::chrono::duration<double>(now - windowStart_).count();
    if (windowShown_.size() != streams_.size()) windowShown_.assign(streams_.size(), 0), streamFps_.assign(streams_.size(), 0);
    if (span >= 1.0) {
        presentFps_ = windowPresents_ / span;
        windowPresentsPerS_ = windowPresentCount_ / span;
        windowPresentCount_ = 0;
        uploadMs_ = windowUploadMs_ / static_cast<double>(windowPresents_);
        for (size_t i = 0; i < streams_.size(); ++i) {
            streamFps_[i] = (streams_[i].shown - windowShown_[i]) / span;
            windowShown_[i] = streams_[i].shown;
        }
        heldMs_ = windowHeldCount_ ? windowHeldMs_ / static_cast<double>(windowHeldCount_) : 0;
        windowHeldMs_ = 0;
        windowHeldCount_ = 0;
        windowStart_ = now;
        windowPresents_ = 0;
        windowUploadMs_ = 0;
        updateCompensation();
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
    auto maxOf = [](const std::deque<double>& v) { return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()); };
    auto avgOf = [](const std::deque<double>& v) {
        double sum = 0;
        for (double x : v) sum += x;
        return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
    };
    bool timingOk = !windows_.empty() && std::all_of(windows_.begin(), windows_.end(), [](auto& w) { return w.valid; });
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
                            {"offsetMs", tiles_[i].window < static_cast<int>(windows_.size()) ? avgOf(windows_[tiles_[i].window].offsets) : 0.0},
                            {"offsetMaxMs", tiles_[i].window < static_cast<int>(windows_.size()) ? maxOf(windows_[tiles_[i].window].offsets) : 0.0},
                            {"missedRefreshes", tiles_[i].window < static_cast<int>(windows_.size()) ? windows_[tiles_[i].window].missed : 0},
                            {"windowsLateMs", tiles_[i].window < static_cast<int>(windows_.size()) ? median(windows_[tiles_[i].window].lag) : 0.0},
                            {"heldRefreshes", tiles_[i].window < static_cast<int>(windows_.size()) ? windows_[tiles_[i].window].hold : 0},
                            {"fps", displayStream_[i] < static_cast<int>(streamFps_.size()) ? streamFps_[displayStream_[i]] : 0.0},
                            {"received", s.input ? s.input->queue().received() : 0},
                            {"width", s.width},
                            {"height", s.height}});
    }
    while (!hiccupTimes_.empty() && now - hiccupTimes_.front() > std::chrono::minutes(1)) hiccupTimes_.pop_front();
    json hiccups = json::array();
    for (auto it = hiccups_.rbegin(); it != hiccups_.rend(); ++it)
        hiccups.push_back({{"at", it->at}, {"ms", std::round(it->ms)}, {"cause", it->cause}, {"detail", it->detail}});
    uint64_t spreadMax = recentSpread_.empty() ? 0 : *std::max_element(recentSpread_.begin(), recentSpread_.end());
    std::lock_guard lock(statusMutex_);
    status_ = {{"ok", true},
               {"presenter", presenter_->name()},
               {"refreshHz", presenter_->refreshHz()},
               {"presentFps", presentFps_},
               {"windowPresentsPerS", windowPresentsPerS_},
               {"compensationChanges", holdChanges_},
               {"presentMsPerWindow", msPerWindow_},
               {"heldDeferred", deferred_},
               {"timing", timingOk ? "measured" : "not available"},
               {"screenSpreadMs", avgOf(spreads_)},
               {"screenSpreadMaxMs", maxOf(spreads_)},
               {"screenSpreadNowMs", avgOf(recentSpreads_)},
               {"compensate", active_.compensate},
               {"flashTest", active_.flashTest},
               {"uploadMs", uploadMs_},
               {"uptimeS", std::chrono::duration<double>(now - started_).count()},
               {"presents", presents_},
               {"targetFrameId", target_},
               {"spread", recentSpread_.empty() ? 0 : recentSpread_.back()},
               {"spreadMax10s", spreadMax},
               {"alignedRatio", comparedPresents_ ? double(alignedPresents_) / double(comparedPresents_) : 1.0},
               {"bufferMs", active_.bufferMs},
               {"bufferReady", active_.bufferMs > 0 && engineClockKnown_},
               {"heldMs", heldMs_},
               {"gapsAbsorbed", gapsAbsorbed_},
               {"shownAtEngine", shownAtEngineJson()},
               {"freezesLastMinute", hiccupTimes_.size()},
               {"freezesTotal", hiccupTotal_},
               {"freezes", hiccups},
               {"displays", displays}};
    if (active_.probe) status_["probe"] = presenter_->probe();
}

// Matches each window's "this present reached the screen at T" with the present it was. How
// long after we presented it Windows showed it is that display's lateness (our hold-back isn't
// in it); a new picture seen on several windows gives their offsets: how much later than the
// first display each one showed it, in milliseconds. A present shown later than that display
// usually shows them counts as skipped refreshes (the monitor or Windows fell behind).
void Receiver::trackTiming(const std::vector<PresentTiming>& timings) {
    const size_t keep = static_cast<size_t>(presenter_->refreshHz() * 10);  // ~10 s
    uint64_t newest = 0;
    for (size_t w = 0; w < windows_.size() && w < timings.size(); ++w) {
        auto& tr = windows_[w];
        const auto& t = timings[w];
        uint64_t id = tr.frameId;
        newest = std::max(newest, id);
        if (t.presented) {
            // Every present is remembered with when it was made; a new picture also gets timed across windows.
            // The first refresh after the present: the pass's refresh plus however many whole
            // refreshes had gone by when it was made (one, unless the pass overran).
            const double periodUs = 1e6 / presenter_->refreshHz();
            double behind = std::max(1.0, std::ceil((t.submitMicros - t.refreshMicros) / periodUs));
            tr.pending.push_back({t.presentCount, id && id != tr.lastFrameId ? id : 0, t.refreshMicros + static_cast<int64_t>(behind * periodUs)});
            if (tr.pending.size() > 32) tr.pending.pop_front();
            if (id) tr.lastFrameId = id;
        }
        tr.valid = t.valid;
        if (!t.valid) continue;
        const double periodUs = 1e6 / presenter_->refreshHz();
        while (!tr.pending.empty() && static_cast<int32_t>(tr.pending.front().present - t.syncPresentCount) <= 0) {
            const auto& p = tr.pending.front();
            if (p.present == t.syncPresentCount) {
                // How late Windows showed it: on screen minus the first refresh after we presented it
                // (0 normally; a refresh or more for a display Windows composites). Our hold-back
                // isn't in it (a held display is presented later, not shown later), and neither is
                // where in the refresh the loop got round to presenting it.
                int64_t lateUs = t.syncMicros - p.dueUs;
                if (tr.lateUs.size() >= 20) {
                    double usual = median(tr.lateUs);
                    double extra = (lateUs - usual) / periodUs;
                    if (extra >= 1.5) {
                        auto skipped = static_cast<uint32_t>(std::lround(extra));
                        tr.missed += skipped;
                        screenSkipped_ = std::max(screenSkipped_, skipped);
                    }
                }
                tr.lateUs.push_back(static_cast<double>(lateUs));
                if (tr.lateUs.size() > 180) tr.lateUs.pop_front();
                if (p.frameId) {
                    auto& sa = shownAt_[p.frameId];
                    if (sa.at.empty()) sa.at.assign(windows_.size(), -1), sa.lag.assign(windows_.size(), -1);
                    sa.at[w] = t.syncMicros;
                    sa.lag[w] = lateUs;
                }
            }
            tr.pending.pop_front();
        }
    }
    // A frame is judged once every window has shown it, or once it's well in the past.
    for (auto it = shownAt_.begin(); it != shownAt_.end();) {
        auto& at = it->second.at;
        const auto& lag = it->second.lag;
        bool complete = std::all_of(at.begin(), at.end(), [](int64_t v) { return v >= 0; });
        if (!complete && it->first + 30 > newest) {
            ++it;
            continue;
        }
        int64_t first = INT64_MAX, last = INT64_MIN;
        int n = 0;
        for (int64_t v : at)
            if (v >= 0) first = std::min(first, v), last = std::max(last, v), ++n;
        // When the frame appeared, on the engine's clock (this PC's estimate of it: see
        // trackEngineClock), so the engine can compare the PCs of one canvas with each other.
        if (n >= 1 && engineClockKnown_) {
            shownAtEngine_.push_back({it->first, first - engineOffsetUs_});
            if (shownAtEngine_.size() > 120) shownAtEngine_.pop_front();
        }
        if (n >= 2) {
            int64_t lagFirst = INT64_MAX;
            for (int64_t v : lag)
                if (v >= 0) lagFirst = std::min(lagFirst, v);
            for (size_t w = 0; w < at.size(); ++w)
                if (at[w] >= 0) {
                    auto& tr = windows_[w];
                    tr.offsets.push_back((at[w] - first) / 1000.0);
                    if (tr.offsets.size() > keep) tr.offsets.pop_front();
                    tr.lag.push_back((lag[w] - lagFirst) / 1000.0);
                    if (tr.lag.size() > keep / 3) tr.lag.pop_front();  // ~3 s: compensation follows changes quickly
                }
            spreads_.push_back((last - first) / 1000.0);
            if (spreads_.size() > keep) spreads_.pop_front();
            recentSpreads_.push_back((last - first) / 1000.0);
            if (recentSpreads_.size() > keep / 5) recentSpreads_.pop_front();
        }
        it = shownAt_.erase(it);
    }
    while (shownAt_.size() > 240) shownAt_.erase(shownAt_.begin());
}

// Collects each stream's fastest frame (arrival minus engine timecode) and keeps the smallest of
// the last ~10 s as the engine's clock. A jump of over a second (the engine restarted, so its
// timecodes started again) starts the estimate over.
void Receiver::trackEngineClock(Clock::time_point now) {
    int64_t fastest = INT64_MAX;
    for (auto& s : streams_)
        if (s.input) fastest = std::min(fastest, s.input->queue().takeMinTransitUs());
    if (fastest != INT64_MAX) {
        if (engineClockKnown_ && std::llabs(fastest - engineOffsetUs_) > 1000000) transits_.clear();
        transits_.push_back({now, fastest});
    }
    while (!transits_.empty() && now - transits_.front().first > std::chrono::seconds(10)) transits_.pop_front();
    engineClockKnown_ = !transits_.empty();
    if (engineClockKnown_)
        engineOffsetUs_ = std::min_element(transits_.begin(), transits_.end(), [](auto& a, auto& b) { return a.second < b.second; })->second;
}

// Records a freeze (newest last) and logs it with the local time.
void Receiver::hiccup(const std::string& cause, double ms, const std::string& detail) {
    auto start = std::chrono::system_clock::now() - std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double, std::milli>(ms));
    Hiccup h{localTime(start), ms, cause, detail};
    std::cerr << "[receiver] " << h.at << " freeze " << std::lround(ms) << " ms (" << cause << "): " << detail << "\n";
    hiccups_.push_back(std::move(h));
    if (hiccups_.size() > 20) hiccups_.pop_front();
    hiccupTimes_.push_back(Clock::now());
    ++hiccupTotal_;
}

// Once a second: how much later Windows shows each display than the soonest one (median of the
// last ~3 s, without our hold-back), in whole refreshes. Displays shown sooner are held back by
// the difference, so every display shows each frame on the same refresh. A new value must hold
// for 3 s before it's applied, and one halfway between two refresh counts is ignored (displays
// whose refreshes aren't lined up can't be matched by holding back anyway).
void Receiver::updateCompensation() {
    const double period = 1000.0 / presenter_->refreshHz();
    std::vector<double> late(windows_.size(), 0);
    bool measured = active_.compensate && windows_.size() > 1;
    for (size_t w = 0; w < windows_.size() && measured; ++w) {
        if (windows_[w].lag.size() < 20) measured = false;
        else late[w] = median(windows_[w].lag);
    }
    double latest = measured ? *std::max_element(late.begin(), late.end()) : 0;
    for (size_t w = 0; w < windows_.size(); ++w) {
        auto& tr = windows_[w];
        if (!active_.compensate || windows_.size() < 2) {
            tr.hold = tr.want = tr.wantCount = 0;
            continue;
        }
        if (!measured) continue;
        double x = (latest - late[w]) / period;
        int want = std::clamp(static_cast<int>(std::lround(x)), 0, kPictureHistory - 2);
        if (std::fabs(x - std::lround(x)) > 0.3 || want == tr.hold) {
            tr.wantCount = 0;
            continue;
        }
        tr.wantCount = want == tr.want ? tr.wantCount + 1 : 1;
        tr.want = want;
        if (tr.wantCount >= 3) {
            std::cerr << "[receiver] window " << w << ": shown " << latest - late[w] << " ms sooner than the latest; holding it back " << want << " refresh(es)\n";
            tr.hold = want;
            tr.wantCount = 0;
            ++holdChanges_;
        }
    }
}

}  // namespace mosaic::rx
