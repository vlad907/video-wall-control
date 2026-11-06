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

// Local wall-clock time with milliseconds, to match freezes against other logs on the PC.
static std::string localTime(std::chrono::system_clock::time_point t) {
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
    c.compensate = j.value("compensate", c.compensate);
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
    return {{"displays", displays}, {"lagFrames", c.lagFrames}, {"lostAfterMs", c.lostAfterMs}, {"flashTest", c.flashTest}, {"compensate", c.compensate}};
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
    windows_.assign(windows.size(), {});
    windowStream_.assign(windows.size(), -1);
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
            hiccup("network", ms, "no frames from " + s.name + " for " + std::to_string(std::lround(ms)) + " ms");
            streamGap_[i] = {};
        }
    }
    Choice c = chooseFrames(views, target_, static_cast<uint64_t>(active_.lagFrames));
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
    for (auto& t : draw) {
        if (t.stream < 0 || t.window >= static_cast<int>(windows_.size())) continue;
        const auto& s = streams_[t.stream];
        auto& win = windows_[t.window];
        if (s.history.empty()) continue;
        const Stream::Picture* pick = &s.history.front();
        auto due = now - std::chrono::duration_cast<Clock::duration>(period * (win.hold - 0.5));
        for (auto& p : s.history)
            if (win.hold == 0 || p.at <= due) pick = &p;
        t.back = static_cast<int>(s.gen - pick->gen);
        if (windowStream_[t.window] == t.stream) win.frameId = pick->frameId;
    }
    // Flash test: a window goes white while it shows an engine frame divisible by 30.
    std::vector<bool> flash(windows_.size(), false);
    for (size_t w = 0; w < windows_.size() && active_.flashTest; ++w) flash[w] = windows_[w].frameId && windows_[w].frameId % 30 == 0;
    auto presentStart = Clock::now();
    presenter_->present(draw, flash);
    auto presentedAt = Clock::now();
    presentMs_ = std::chrono::duration<double, std::milli>(presentedAt - presentStart).count();
    screenSkipped_ = 0;
    trackTiming(presenter_->timings());

    // Loop freeze: this present came 2.5 refreshes or more after the last one. The slowest step
    // (if it took over a refresh) is what held it up.
    const double periodMs = 1000.0 / presenter_->refreshHz();
    if (lastPresent_ != Clock::time_point{}) {
        double gap = std::chrono::duration<double, std::milli>(presentedAt - lastPresent_).count();
        if (gap >= 2.5 * periodMs) {
            std::pair<double, const char*> steps[] = {{waitMs_, "wait"}, {lastUploadMs_, "upload"}, {presentMs_, "present"}};
            auto slowest = *std::max_element(std::begin(steps), std::end(steps));
            char detail[160];
            std::snprintf(detail, sizeof detail, "no present for %.0f ms (wait %.0f ms, upload %.0f ms, present %.0f ms)", gap, waitMs_, lastUploadMs_,
                          presentMs_);
            hiccup(slowest.first > periodMs ? slowest.second : "loop", gap, detail);
            lastLoopHiccup_ = presentedAt;
        } else if (screenSkipped_ >= 3 && presentedAt - lastLoopHiccup_ > std::chrono::milliseconds(500)) {
            // The monitors' statistics trail by a few presents, so skips right after a loop freeze are that freeze.
            hiccup("screen", screenSkipped_ * periodMs,
                   "monitors skipped " + std::to_string(screenSkipped_) + " refreshes though the receiver presented on time");
        }
    }
    lastPresent_ = presentedAt;

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
               {"freezesLastMinute", hiccupTimes_.size()},
               {"freezesTotal", hiccupTotal_},
               {"freezes", hiccups},
               {"displays", displays}};
}

// Matches each window's "this present reached the screen at T" with the frame that present
// showed. A frame seen on several windows gives their offsets: how much later than the first
// display each one showed it, in milliseconds. Also counts refreshes a monitor showed without a
// new present from us (the loop fell behind: a repeated frame, i.e. stutter).
void Receiver::trackTiming(const std::vector<PresentTiming>& timings) {
    const size_t keep = static_cast<size_t>(presenter_->refreshHz() * 10);  // ~10 s
    uint64_t newest = 0;
    for (size_t w = 0; w < windows_.size() && w < timings.size(); ++w) {
        auto& tr = windows_[w];
        const auto& t = timings[w];
        uint64_t id = tr.frameId;
        newest = std::max(newest, id);
        if (id && id != tr.lastFrameId) {
            tr.pending.push_back({t.presentCount, id, static_cast<int64_t>(tr.hold * 1e6 / presenter_->refreshHz())});
            if (tr.pending.size() > 32) tr.pending.pop_front();
            tr.lastFrameId = id;
        }
        tr.valid = t.valid;
        if (!t.valid) continue;
        if (tr.seen) {
            uint32_t presents = t.syncPresentCount - tr.lastSyncPresent;
            uint32_t refreshes = t.syncRefreshCount - tr.lastSyncRefresh;
            if (presents > 0 && refreshes > presents && refreshes - presents < 600) {
                tr.missed += refreshes - presents;
                screenSkipped_ = std::max(screenSkipped_, refreshes - presents);
            }
        }
        tr.seen = true;
        tr.lastSyncPresent = t.syncPresentCount;
        tr.lastSyncRefresh = t.syncRefreshCount;
        while (!tr.pending.empty() && static_cast<int32_t>(tr.pending.front().present - t.syncPresentCount) <= 0) {
            const auto& p = tr.pending.front();
            if (p.present == t.syncPresentCount) {
                auto& sa = shownAt_[p.frameId];
                if (sa.at.empty()) sa.at.assign(windows_.size(), -1), sa.lag.assign(windows_.size(), -1);
                sa.at[w] = t.syncMicros;
                sa.lag[w] = t.syncMicros - p.heldUs;
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
        }
    }
}

}  // namespace mosaic::rx
