#include "core/engine.hpp"
#include "io/capture.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <set>

#include "util/image.hpp"
#include "util/paths.hpp"
#include "util/realtime.hpp"

using nlohmann::json;

namespace mosaic {

namespace {
constexpr float kTileW = 1920.f, kTileH = 1080.f;  // test pattern tile size
constexpr int kPreviewEvery = 6;                   // previews at ~10 fps when running 60
constexpr auto kPreviewKeepAlive = std::chrono::seconds(3);
constexpr size_t kSyncWindow = 240;
const float kNoSignal[4] = {0.08f, 0.08f, 0.1f, 1.f};

uint32_t scaleModeIndex(ScaleMode m) { return m == ScaleMode::Fit ? 1u : m == ScaleMode::Fill ? 2u : 0u; }

bool covers(const Rect& a, const Rect& b) {
    return a.x <= b.x && a.y <= b.y && a.x + a.w >= b.x + b.w && a.y + a.h >= b.y + b.h;
}

// Sources that contribute to the picture: enabled, not transparent, on the
// canvas and not completely hidden under an opaque source drawn after them,
// plus any source an output samples directly. Everything else is skipped.
std::set<std::string> neededSources(const Layout& l) {
    std::set<std::string> needed;
    const Rect canvas{0, 0, l.canvas.width, l.canvas.height};
    for (size_t i = 0; i < l.sources.size(); ++i) {
        const SourceConfig& s = l.sources[i];
        if (!s.enabled || s.opacity <= 0.f) continue;
        Rect r = s.rect;
        r.x = std::max(r.x, 0);
        r.y = std::max(r.y, 0);
        r.w = std::min(s.rect.x + s.rect.w, canvas.w) - r.x;
        r.h = std::min(s.rect.y + s.rect.h, canvas.h) - r.y;
        if (r.w <= 0 || r.h <= 0) continue;
        bool hidden = false;
        for (size_t j = i + 1; j < l.sources.size() && !hidden; ++j) {
            const SourceConfig& o = l.sources[j];
            // Images may have transparent pixels, so they never hide what's under them.
            bool opaque = o.enabled && o.opacity >= 1.f && o.type != SourceType::Image && (o.type != SourceType::Solid || o.color[3] >= 1.f);
            hidden = opaque && covers(o.rect, r);
        }
        if (!hidden) needed.insert(s.id);
    }
    for (auto& o : l.outputs)
        if (o.enabled && o.source != "canvas") needed.insert(o.source);
    return needed;
}

static nlohmann::json displayStatusOf(const std::string& monitor, const std::string& error, const std::string& modeError, bool open) {
    nlohmann::json j = {{"monitor", monitor}, {"error", error}, {"open", open}};
    if (!modeError.empty()) j["modeError"] = modeError;
    return j;
}

Uniforms makeUniforms(float dx, float dy, float dw, float dh) {
    Uniforms u{};
    u.dstRect[0] = dx;
    u.dstRect[1] = dy;
    u.dstRect[2] = dw;
    u.dstRect[3] = dh;
    u.sizes[2] = dw;
    u.sizes[3] = dh;
    u.bg[3] = 1.f;
    u.fparams[3] = 1.f;  // opaque
    u.xform[0] = 1.f;    // no tilt
    return u;
}
}  // namespace

Engine::Engine(Renderer& renderer, LayoutStore& store, NdiSystem* ndi, std::string canvasId)
    : renderer_(renderer), store_(store), ndi_(ndi), canvasId_(std::move(canvasId)), audio_(std::make_unique<AudioOutputs>(ndi)) {
    renderer_.onReadback = [this](ReadbackId rb, int slot, const ReadbackView& view, uint64_t) {
        std::shared_ptr<OutputChannel> ch;
        std::shared_ptr<PreviewState> pv;
        {
            std::lock_guard l(channelsMutex_);
            if (auto it = channelByReadback_.find(rb); it != channelByReadback_.end()) ch = it->second;
            else if (auto it2 = previewByReadback_.find(rb); it2 != previewByReadback_.end()) pv = it2->second;
        }
        if (ch) {
            ch->onReadback(slot, view);
        } else if (pv) {
            {
                std::lock_guard l(pv->m);
                pv->bgra.resize(static_cast<size_t>(view.width) * view.height * 4);
                for (int y = 0; y < view.height; ++y)
                    std::memcpy(pv->bgra.data() + static_cast<size_t>(y) * view.width * 4,
                                view.data + static_cast<size_t>(y) * view.stride, static_cast<size_t>(view.width) * 4);
                pv->has = true;
            }
            pv->ring.requestRelease(slot);
        }
    };
    renderer_.onFrameComplete = [this](uint64_t, const GpuFrameStats& st) {
        if (st.gpuMs > 0) gpuMs_.add(st.gpuMs);
    };
}

Engine::~Engine() { stop(); }

void Engine::start() {
    if (running_.exchange(true)) return;
    start_ = Clock::now();
    thread_ = std::thread([this] { loop(); });
}

void Engine::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (failed_) {
        // The device is gone: GPU calls would fail. Senders stop; GPU objects go with the renderer.
        std::lock_guard l(channelsMutex_);
        for (auto& [id, ch] : channels_) ch->shutdown();
        return;
    }
    // Tear down: stop senders, let the GPU finish, then free resources (display outputs were
    // closed by the render thread).
    std::map<std::string, std::shared_ptr<OutputChannel>> channels;
    {
        std::lock_guard l(channelsMutex_);
        channels.swap(channels_);
    }
    for (auto& [id, ch] : channels) ch->shutdown();
    renderer_.waitIdle();
    for (auto& [id, ch] : channels) ch->destroyGpuResources();
    {
        std::lock_guard l(channelsMutex_);
        channelByReadback_.clear();
        for (auto& [id, pv] : previews_) {
            renderer_.destroyReadback(pv->rb);
            renderer_.destroyTexture(pv->tex);
        }
        previews_.clear();
        previewByReadback_.clear();
    }
    for (auto& [id, s] : sources_) destroySource(s);
    for (auto& [name, fb] : fallbacks_)
        if (fb.tex) renderer_.destroyTexture(fb.tex);
    fallbacks_.clear();
    sources_.clear();
    if (canvas_) renderer_.destroyTexture(canvas_);
    canvas_ = 0;
    if (fadeTex_) renderer_.destroyTexture(fadeTex_);
    fadeTex_ = 0;
}

bool Engine::captureForTransition(int ms, Rect region) {
    std::unique_lock l(transitionMutex_);
    transitionRequestMs_ = std::clamp(ms, 1, 10000);
    transitionRequestRect_ = region;
    transitionCaptured_ = false;
    return transitionCv_.wait_for(l, std::chrono::milliseconds(250), [this] { return transitionCaptured_; });
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

void Engine::loop() {
    try {
        run();
    } catch (const std::exception& e) {
        // The GPU device was lost (driver reset, card removed) or a GPU call failed: this canvas
        // stops here and Canvases starts it again; the other canvases carry on.
        fail(e.what());
    }
    // Display windows belong to this thread: closed here (Windows won't let another thread).
    try {
        for (auto& [oid, d] : displays_) closeDisplay(d);
    } catch (...) {
    }
    displays_.clear();
}

void Engine::fail(const std::string& why) {
    {
        std::lock_guard l(statusMutex_);
        failure_ = why;
    }
    failed_ = true;
    std::cerr << "[engine] canvas " << canvasId_ << " stopped: " << why << "\n";
}

std::string Engine::failure() const {
    std::lock_guard l(statusMutex_);
    return failure_;
}

void Engine::run() {
    raiseThreadPriority(ThreadRole::Render);
    PreciseSleeper sleeper;
    auto next = Clock::now();
    while (running_) {
        if (simulateFailure_) throw std::runtime_error("simulated GPU device loss (test)");
        if (frameId_ % 60 == 0) {
            std::string lost = renderer_.deviceLost();
            if (!lost.empty()) return fail(lost);
        }
        auto layout = store_.snapshot(canvasId_);
        if (!layout) {  // the canvas is being stopped
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (layout != applied_) {
            applied_ = layout;
            reconcile(*layout);
        }
        auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / layout->frameRate));
        if (frameId_ % 120 == 0) syncDisplays(*layout, true);  // monitors that weren't there: try again

        // A canvas with display outputs runs on its first monitor's refresh (a frame every
        // refresh, or every other one for 30 fps on 60 Hz), so they never drop or repeat frames.
        DisplayId pace = 0;
        double paceHz = 0;
        for (auto& [oid, d] : displays_)
            if (d.id) {
                pace = d.id;
                paceHz = renderer_.displayStats(d.id).refreshHz;
                break;
            }
        bool paced = false;
        if (pace && paceHz > 0) {
            int perFrame = std::max(1, static_cast<int>(std::lround(paceHz / layout->frameRate)));
            paced = true;
            for (int i = 0; i < perFrame && paced; ++i) paced = renderer_.waitDisplayVBlank(pace);
            if (paced) targetFps_ = paceHz / perFrame;
        }
        if (!paced) {
            targetFps_ = layout->frameRate;
            sleeper.sleepUntil(next);
        }
        auto tick = Clock::now();
        if (paced) {
            next = tick + interval;
        } else {
            jitterMs_.add(msBetween(next, tick));  // how late the tick fired
            if (tick - next > interval) {
                // Fell behind by more than a frame: skip ahead instead of bursting.
                lateFrames_++;
                next = tick;
            }
            next += interval;
        }

        uint64_t frameId = ++frameId_;
        double seconds = std::chrono::duration<double>(tick - start_).count();
        renderFrame(*layout, frameId, tick, seconds);
        frameMs_.add(msBetween(tick, Clock::now()));
        renderRate_.tick();
    }
}

void Engine::renderFrame(const Layout& layout, uint64_t frameId, Clock::time_point tick, double seconds) {
    // Free readback slots the senders are done with (D3D11 unmaps here).
    {
        std::lock_guard l(channelsMutex_);
        for (auto& [id, ch] : channels_) ch->drainReleases();
        for (auto& [id, pv] : previews_) pv->ring.drainReleases([&](int s) { renderer_.releaseSlot(pv->rb, s); });
    }

    auto w0 = Clock::now();
    renderer_.beginFrame(frameId);
    auto t0 = Clock::now();
    waitMs_.add(msBetween(w0, t0));

    const std::set<std::string> needed = neededSources(layout);
    updateSources(layout, needed, frameId, seconds);

    // Transition: freeze what the canvas shows now; it fades out over the new content.
    int transitionMs = 0;
    Rect transitionRect;
    {
        std::lock_guard l(transitionMutex_);
        std::swap(transitionMs, transitionRequestMs_);
        transitionRect = transitionRequestRect_;
    }
    if (transitionMs > 0 && view_.tex) {
        if (!fadeTex_) fadeTex_ = renderer_.createTexture(canvasW_, canvasH_, TexUsage::RenderTarget);
        Uniforms u = makeUniforms(0, 0, float(canvasW_), float(canvasH_));
        u.crop[2] = u.sizes[0] = view_.w;
        u.crop[3] = u.sizes[1] = view_.h;
        renderer_.beginPass(fadeTex_, nullptr);
        renderer_.drawTextured(view_.tex, u);
        renderer_.endPass();
        fadeStart_ = tick;
        fadeMs_ = transitionMs;
        // Only that part of the canvas fades; elsewhere the live picture carries on untouched.
        int x0 = std::clamp(transitionRect.x, 0, canvasW_), y0 = std::clamp(transitionRect.y, 0, canvasH_);
        int x1 = std::clamp(transitionRect.x + transitionRect.w, 0, canvasW_), y1 = std::clamp(transitionRect.y + transitionRect.h, 0, canvasH_);
        fadeRect_ = transitionRect.w > 0 && transitionRect.h > 0 && x1 > x0 && y1 > y0 ? Rect{x0, y0, x1 - x0, y1 - y0}
                                                                                         : Rect{0, 0, canvasW_, canvasH_};
        std::lock_guard l(transitionMutex_);
        transitionCaptured_ = true;
        transitionCv_.notify_all();
    }

    // Direct path: one opaque, uncropped source covering exactly the canvas is
    // sampled by the outputs as is, saving a full-canvas pass per frame.
    const SourceConfig* only = nullptr;
    int drawn = 0;
    for (auto& sc : layout.sources)
        if (needed.count(sc.id) && sc.enabled && sc.opacity > 0.f) {
            only = &sc;
            ++drawn;
        }
    const SourceState* direct = nullptr;
    if (drawn == 1 && fadeMs_ <= 0 && only->opacity >= 1.f && only->type != SourceType::Solid && only->type != SourceType::Image && only->crop.w == 0 &&
        only->rect.x == 0 && only->rect.y == 0 && only->rect.w == canvasW_ && only->rect.h == canvasH_) {
        auto it = sources_.find(only->id);
        if (it != sources_.end() && it->second.hasFrame && !(it->second.signalLost && fallbackFor(layout, *only))) direct = &it->second;
    }
    if (direct) {
        view_ = {direct->tex[direct->current], float(direct->w), float(direct->h)};
        directFrames_++;
    } else {
        view_ = {canvas_, float(canvasW_), float(canvasH_)};
        renderCanvas(layout, needed, tick);
    }

    // Outputs: each samples the canvas (or one source) into its own target.
    int64_t timecode = std::chrono::duration_cast<std::chrono::nanoseconds>(tick - start_).count() / 100;
    int expected = 0;
    std::vector<std::shared_ptr<OutputChannel>> active;
    {
        std::lock_guard l(channelsMutex_);
        for (auto& o : layout.outputs) {
            if (!o.enabled) continue;
            auto it = channels_.find(o.id);
            if (it == channels_.end()) continue;
            active.push_back(it->second);
        }
    }
    outputDraws_.clear();
    // How an output samples the view (or its one source) into a w x h target.
    auto mapping = [&](const OutputConfig& oc, int w, int h, TexId& src) {
        const OutputConfig* o = &oc;
        // Crop is in canvas pixels; the view's texture covers exactly the canvas
        // (whatever its own resolution), so normalise by the canvas size.
        src = view_.tex;
        float sw = float(canvasW_), sh = float(canvasH_);
        bool solid = false;
        if (o->source != "canvas") {
            auto it = sources_.find(o->source);
            if (it != sources_.end() && it->second.hasFrame) {
                src = it->second.tex[it->second.current];
                sw = float(it->second.w);
                sh = float(it->second.h);
            } else {
                solid = true;
            }
        }
        Uniforms u = makeUniforms(0, 0, float(w), float(h));
        u.crop[0] = float(o->crop.x);
        u.crop[1] = float(o->crop.y);
        u.crop[2] = float(o->crop.w);
        u.crop[3] = float(o->crop.h);
        const float rad = o->angle * 3.14159265f / 180.f;
        u.xform[0] = std::cos(rad);
        u.xform[1] = std::sin(rad);
        u.sizes[0] = sw;
        u.sizes[1] = sh;
        std::copy(layout.canvas.background.begin(), layout.canvas.background.end(), u.bg);
        u.iparams[0] = static_cast<uint32_t>(frameId);
        u.iparams[1] = static_cast<uint32_t>(o->rotation);
        u.iparams[2] = scaleModeIndex(o->scaleMode);
        u.iparams[3] = solid ? 1u : 0u;
        if (!o->color.neutral()) {
            const ColorAdjust& c = o->color;
            u.iparams[3] |= 4u;
            u.grade0[0] = c.red;
            u.grade0[1] = c.green;
            u.grade0[2] = c.blue;
            u.grade0[3] = c.brightness;
            u.grade1[0] = c.contrast;
            u.grade1[1] = c.saturation;
            u.grade1[2] = c.gamma;
        }
        return u;
    };
    // Display outputs: drawn straight into their monitor's back buffer, at its resolution.
    for (auto& [oid, d] : displays_) {
        const OutputConfig* o = layout.findOutput(oid);
        if (!o || !o->enabled || !d.id) continue;
        DisplayStats st = renderer_.displayStats(d.id);
        TexId src = 0;
        Uniforms u = mapping(*o, st.width, st.height, src);
        outputDraws_[oid] = {src, u};
        // Delay: frames held back = delay x frame rate, at most 120 and ~2 GB of GPU memory.
        int64_t frameBytes = std::max<int64_t>(1, int64_t(st.width) * st.height * 4);
        int held = std::clamp(static_cast<int>(std::lround(o->delayMs * targetFps_ / 1000.0)), 0,
                              static_cast<int>(std::min<int64_t>(120, (int64_t(2) << 30) / frameBytes)));
        if (held == 0) {
            if (!d.ring.empty()) freeRing(d);
            TexId target = renderer_.displayTarget(d.id);
            if (!target) continue;  // the monitor hasn't shown the last frame yet: skipped (counted)
            renderer_.beginPass(target, layout.canvas.background.data());
            renderer_.drawTextured(src, u);
            renderer_.endPass();
            renderer_.presentDisplay(d.id);
            continue;
        }
        if (int(d.ring.size()) != held + 1 || d.ringW != st.width || d.ringH != st.height) {
            freeRing(d);
            for (int i = 0; i <= held; ++i) d.ring.push_back(renderer_.createTexture(st.width, st.height, TexUsage::RenderTarget));
            d.ringW = st.width;
            d.ringH = st.height;
        }
        // Every frame goes into the ring, shown or not, so the delay stays the same.
        renderer_.beginPass(d.ring[d.head], layout.canvas.background.data());
        renderer_.drawTextured(src, u);
        renderer_.endPass();
        int size = static_cast<int>(d.ring.size());
        d.filled = std::min(d.filled + 1, size);
        int oldest = d.filled == size ? (d.head + 1) % size : 0;  // until it's full: the first frame held
        d.head = (d.head + 1) % size;
        TexId target = renderer_.displayTarget(d.id);
        if (!target) continue;
        Uniforms copy = makeUniforms(0, 0, float(st.width), float(st.height));
        copy.crop[2] = copy.sizes[0] = float(st.width);
        copy.crop[3] = copy.sizes[1] = float(st.height);
        renderer_.beginPass(target, nullptr);
        renderer_.drawTextured(d.ring[oldest], copy);
        renderer_.endPass();
        renderer_.presentDisplay(d.id);
    }
    if (frameId % 30 == 0 && !displays_.empty()) {
        // What reached each monitor, for metrics() (the renderer's displays are render-thread only).
        json status = json::object();
        double secs = std::chrono::duration<double>(tick - displayStatsAt_).count();
        for (auto& [oid, d] : displays_) {
            json j = displayStatusOf(d.monitor, d.error, d.modeError, d.id != 0);
            if (d.id) {
                DisplayStats st = renderer_.displayStats(d.id);
                double fps = secs > 0 && displayPresents_.count(oid) ? (st.presents - displayPresents_[oid]) / secs : 0;
                displayPresents_[oid] = st.presents;
                j.update({{"width", st.width}, {"height", st.height}, {"refreshHz", st.refreshHz}, {"fps", fps}, {"presents", st.presents},
                          {"busy", st.busy}, {"repeats", st.repeats}, {"heldFrames", d.ring.empty() ? 0 : int(d.ring.size()) - 1}});
                if (!st.error.empty()) j["error"] = st.error;
            }
            status[oid] = j;
        }
        displayStatsAt_ = tick;
        std::lock_guard l(displayStatusMutex_);
        displayStatus_ = status;
    }
    for (auto& ch : active) {
        const OutputConfig* o = layout.findOutput(ch->id);
        int slot = ch->acquireSlot({frameId, tick, timecode});
        if (slot < 0) {
            ch->countNoSlotDrop();
            continue;
        }
        TexId src = 0;
        Uniforms u = mapping(*o, ch->width, ch->height, src);
        if (ch->uyvy) {
            // Map, sample and pack to UYVY in one pass: half the readback bytes, NDI's
            // native format, and no full-size BGRA intermediate. The viewport is the
            // half-width packed target; sizes[2..3] stay the output's size.
            Uniforms p = u;
            p.dstRect[2] = float(ch->width / 2);
            renderer_.beginPass(ch->packed(), nullptr);
            renderer_.drawTexturedUYVY(src, p);
        } else {
            renderer_.beginPass(ch->target(), layout.canvas.background.data());
            renderer_.drawTextured(src, u);
        }
        renderer_.endPass();
        outputDraws_[ch->id] = {src, u};
        renderer_.copyToReadback(ch->readbackSource(), ch->readback(), slot);
        ++expected;
    }

    if (frameId % kPreviewEvery == 0) renderPreviews(layout);

    renderer_.endFrame();
    encodeMs_.add(msBetween(t0, Clock::now()));

    std::lock_guard l(syncMutex_);
    sync_.push_back({frameId, expected, 0, 0});
    while (sync_.size() > kSyncWindow) {
        auto& e = sync_.front();
        if (e.expected > 0) (e.delivered == e.expected ? framesComplete_ : framesPartial_)++;
        sync_.pop_front();
    }
}

// Canvas: needed sources in order, each blended over the ones before it.
void Engine::renderCanvas(const Layout& layout, const std::set<std::string>& needed, Clock::time_point tick) {
    renderer_.beginPass(canvas_, layout.canvas.background.data());
    for (auto& sc : layout.sources) {
        if (!needed.count(sc.id) || !sc.enabled || sc.opacity <= 0.f) continue;
        auto it = sources_.find(sc.id);
        if (it == sources_.end()) continue;
        SourceState& s = it->second;
        Uniforms u = makeUniforms(float(sc.rect.x), float(sc.rect.y), float(sc.rect.w), float(sc.rect.h));
        u.fparams[3] = sc.opacity;
        if ((sc.type == SourceType::Ndi || sc.type == SourceType::Capture) && s.signalLost) {
            if (const ImageTex* fb = fallbackFor(layout, sc)) {
                u.sizes[0] = u.crop[2] = float(fb->w);
                u.sizes[1] = u.crop[3] = float(fb->h);
                if (fb->hasAlpha) u.iparams[3] |= 2;
                renderer_.drawTextured(fb->tex, u);
                continue;
            }
        }
        if (sc.type == SourceType::Solid || !s.hasFrame) {
            const float* c = sc.type == SourceType::Solid ? sc.color.data() : kNoSignal;
            std::copy(c, c + 3, u.bg);
            u.fparams[3] = sc.opacity * c[3];
            u.iparams[3] = 1;  // solid
            renderer_.drawTextured(canvas_, u);
            continue;
        }
        u.sizes[0] = float(s.w);
        u.sizes[1] = float(s.h);
        bool cropped = sc.crop.w > 0 && sc.crop.h > 0;
        u.crop[0] = cropped ? float(sc.crop.x) : 0.f;
        u.crop[1] = cropped ? float(sc.crop.y) : 0.f;
        u.crop[2] = cropped ? float(sc.crop.w) : float(s.w);
        u.crop[3] = cropped ? float(sc.crop.h) : float(s.h);
        if (s.hasAlpha) u.iparams[3] |= 2;  // blend by the image's own alpha too
        renderer_.drawTextured(s.tex[s.current], u);
    }
    if (fadeMs_ > 0) {
        double t = msBetween(fadeStart_, tick) / fadeMs_;
        if (t < 1.0) {
            const Rect& r = fadeRect_;
            Uniforms u = makeUniforms(float(r.x), float(r.y), float(r.w), float(r.h));
            u.crop[0] = float(r.x);
            u.crop[1] = float(r.y);
            u.crop[2] = float(r.w);
            u.crop[3] = float(r.h);
            u.sizes[0] = float(canvasW_);
            u.sizes[1] = float(canvasH_);
            u.fparams[3] = static_cast<float>(1.0 - t * t * (3.0 - 2.0 * t));  // smoothstep fade-out
            renderer_.drawTextured(fadeTex_, u);
        } else {
            fadeMs_ = 0;
        }
    }
    renderer_.endPass();
}

void Engine::syncDelivered(uint64_t frameId, bool delivered) {
    std::lock_guard l(syncMutex_);
    for (auto it = sync_.rbegin(); it != sync_.rend(); ++it) {
        if (it->frameId == frameId) {
            (delivered ? it->delivered : it->dropped)++;
            return;
        }
        if (it->frameId < frameId) return;
    }
}

// No frame for 2 s (or none at all 3 s after connecting) counts as a lost signal.
constexpr auto kSignalLostAfter = std::chrono::seconds(2);
constexpr auto kNoFirstFrameAfter = std::chrono::seconds(3);

const Engine::ImageTex* Engine::fallbackFor(const Layout& layout, const SourceConfig& sc) const {
    const WallConfig* w = layout.findWall(sc.group);
    const std::string& name = w && !w->fallbackImage.empty() ? w->fallbackImage : layout.fallbackImage;
    auto it = name.empty() ? fallbacks_.end() : fallbacks_.find(name);
    return it != fallbacks_.end() && it->second.uploaded ? &it->second : nullptr;
}

void Engine::updateSources(const Layout& layout, const std::set<std::string>& needed, uint64_t frameId, double seconds) {
    for (auto& [name, fb] : fallbacks_)
        if (!fb.pending.empty()) {
            renderer_.uploadBGRA(fb.tex, fb.pending.data(), fb.w * 4);
            fb.pending.clear();
            fb.pending.shrink_to_fit();
            fb.uploaded = true;
        }
    auto now = Clock::now();
    std::vector<std::string> restarts;
    {
        std::lock_guard l(videoRestartMutex_);
        restarts.swap(videoRestarts_);
    }
    for (auto& id : restarts) {
        auto it = sources_.find(id);
        if (it == sources_.end() || !it->second.video) continue;
        it->second.video->restart();
        it->second.playStart = now;
        if (it->second.pausedAt >= 0) it->second.pausedAt = 0;
    }
    json status = json::array();
    for (auto& [id, s] : sources_) {
        json st = {{"id", id}, {"type", toString(s.cfg.type)}, {"width", s.w}, {"height", s.h}};
        if (s.cfg.type == SourceType::TestPattern) {
            st["status"] = needed.count(id) ? "ok" : "hidden (not rendered)";
            st["inputFps"] = renderRate_.rate();
            // A frozen pattern only needs drawing once; a hidden one not at all.
            if (!needed.count(id) || (!s.cfg.animated && s.patternDrawn)) {
                status.push_back(st);
                continue;
            }
            s.patternDrawn = true;
            Uniforms u = makeUniforms(0, 0, float(s.w), float(s.h));
            u.fparams[0] = s.cfg.animated ? static_cast<float>(seconds) : 0.f;
            u.fparams[1] = kTileW;
            u.fparams[2] = kTileH;
            u.iparams[0] = s.cfg.animated ? static_cast<uint32_t>(frameId) : 0u;
            renderer_.beginPass(s.tex[0], nullptr);
            renderer_.drawTestPattern(u);
            renderer_.endPass();
        } else if (s.cfg.type == SourceType::Ndi || s.cfg.type == SourceType::Capture) {
            if (!s.rx) {
                st["status"] = s.cfg.type == SourceType::Capture ? s.error : ndi_ ? "receiver failed" : "NDI unavailable";
                s.signalLost = now - s.createdAt > kNoFirstFrameAfter;  // nothing will arrive
                st["signal"] = s.signalLost ? "lost" : "ok";
                if (s.signalLost && fallbackFor(layout, s.cfg)) st["showing"] = "fallback image";
            } else {
                InputFrame f;
                if (s.rx->acquireLatest(f)) {
                    bool uyvy = f.uyvy && f.width % 2 == 0;
                    if (f.width != s.w || f.height != s.h || uyvy != (s.packed[0] != 0)) {
                        // New resolution or format: replace the textures (in-flight
                        // frames keep the old ones alive until they finish).
                        for (int i = 0; i < s.texCount; ++i) renderer_.destroyTexture(s.tex[i]);
                        for (TexId& t : s.packed)
                            if (t) renderer_.destroyTexture(t), t = 0;
                        s.texCount = 3;
                        for (int i = 0; i < 3; ++i) {
                            s.tex[i] = renderer_.createTexture(f.width, f.height, uyvy ? TexUsage::RenderTarget : TexUsage::Upload);
                            if (uyvy) s.packed[i] = renderer_.createTexture(f.width / 2, f.height, TexUsage::Upload);
                        }
                        s.w = f.width;
                        s.h = f.height;
                    }
                    // Rotate through 3 textures so we never write one the GPU may still read.
                    s.current = (s.current + 1) % s.texCount;
                    if (uyvy) {
                        renderer_.uploadBGRA(s.packed[s.current], f.data, f.stride);  // 4 bytes = 2 pixels
                        renderer_.beginPass(s.tex[s.current], nullptr);
                        renderer_.drawUnpackUYVY(s.packed[s.current]);
                        renderer_.endPass();
                    } else {
                        renderer_.uploadBGRA(s.tex[s.current], f.data, f.stride);
                    }
                    s.rx->release(f);
                    s.hasFrame = true;
                    s.lastFrameAt = now;
                }
                s.signalLost = s.hasFrame ? now - s.lastFrameAt > kSignalLostAfter : now - s.createdAt > kNoFirstFrameAfter;
                st["signal"] = s.signalLost ? "lost" : "ok";
                if (s.signalLost && fallbackFor(layout, s.cfg)) st["showing"] = "fallback image";
                st["status"] = s.rx->connected() ? "ok" : (s.hasFrame ? "no signal (holding last frame)" : "waiting for source");
                st["inputFps"] = s.rx->inputFps();
                st["framesReceived"] = s.rx->framesReceived();
                if (double sent = s.rx->sentFps(); sent > 0) st["sentFps"] = sent;
                st["framesDropped"] = s.rx->framesDropped();
                st["width"] = s.w;
                st["height"] = s.h;
            }
        } else if (s.cfg.type == SourceType::Video) {
            if (!s.video) {
                st["status"] = s.error;
            } else {
                double t = s.pausedAt >= 0 ? s.pausedAt : std::chrono::duration<double>(now - s.playStart).count();
                if (auto f = s.video->frameDue(t)) {
                    bool gpu = f->gpuTexture != nullptr;
                    if (!s.texCount || s.w != f->width || s.h != f->height || s.gpuFrames != gpu) {
                        for (int i = 0; i < s.texCount; ++i) renderer_.destroyTexture(s.tex[i]);
                        s.tex[0] = renderer_.createTexture(f->width, f->height, gpu ? TexUsage::RenderTarget : TexUsage::Upload);
                        s.texCount = 1;
                        s.current = 0;
                        s.w = f->width;
                        s.h = f->height;
                        s.gpuFrames = gpu;
                    }
                    // Hidden videos keep time but aren't copied to the GPU.
                    if (needed.count(id)) {
                        bool ok = gpu ? renderer_.blitVideoFrame(s.tex[0], f->gpuTexture, f->arraySlice, f->colorSpace) : (renderer_.uploadBGRA(s.tex[0], f->bgra, f->stride), true);
                        if (ok) s.hasFrame = true;
                        else s.error = "the GPU couldn't convert the video frame";
                    }
                }
                VideoInfo vi = s.video->info();
                std::string vs = s.video->status();
                st["status"] = vs != "ok" ? vs : !s.error.empty() ? s.error : s.video->finished() ? "ended (holding the last frame)" : s.pausedAt >= 0 ? "paused" : "playing";
                st["video"] = {{"width", vi.width},  {"height", vi.height},       {"fps", vi.fps},
                               {"codec", vi.codec},  {"hardware", vi.hardware}, {"decoder", vi.decoder},   {"durationS", vi.durationS},
                               {"positionS", vi.durationS > 0 ? std::fmod(t, vi.durationS) : t},
                               {"decodeFps", s.video->decodeFps()}, {"playing", s.pausedAt < 0},
                               {"hdr", vi.hdr},      {"bitDepth", vi.bitDepth}};
                st["width"] = s.w;
                st["height"] = s.h;
            }
        } else if (s.cfg.type == SourceType::Image) {
            if (!s.pending.empty()) {
                renderer_.uploadBGRA(s.tex[0], s.pending.data(), s.w * 4);
                s.pending.clear();
                s.pending.shrink_to_fit();
                s.hasFrame = true;
            }
            st["status"] = s.error.empty() ? "ok" : s.error;
        } else {
            st["status"] = "ok";
        }
        status.push_back(st);
    }
    std::lock_guard l(statusMutex_);
    sourceStatus_ = std::move(status);
}

void Engine::renderPreviews(const Layout& layout) {
    auto now = Clock::now();
    std::vector<std::string> wanted;
    {
        std::lock_guard l(previewReqMutex_);
        for (auto it = previewRequests_.begin(); it != previewRequests_.end();) {
            if (now - it->second > kPreviewKeepAlive) it = previewRequests_.erase(it);
            else wanted.push_back((it++)->first);
        }
    }
    for (auto& target : wanted) {
        TexId src = 0;
        int sw = 0, sh = 0;
        const OutputDraw* draw = nullptr;  // outputs: redrawn small with this frame's mapping
        if (target == "canvas") {
            src = view_.tex;
            sw = canvasW_;
            sh = canvasH_;
        } else {
            auto it = outputDraws_.find(target);
            const OutputConfig* o = layout.findOutput(target);
            if (it == outputDraws_.end() || !o || !o->enabled) continue;
            draw = &it->second;
            src = draw->src;
            sw = static_cast<int>(draw->u.sizes[2]);
            sh = static_cast<int>(draw->u.sizes[3]);
        }
        int maxW = target == "canvas" ? 960 : 480;
        int pw = std::min(maxW, sw);
        int ph = std::max(1, static_cast<int>(std::lround(double(pw) * sh / sw)));
        if (ph > maxW) {
            ph = maxW;
            pw = std::max(1, static_cast<int>(std::lround(double(ph) * sw / sh)));
        }

        std::shared_ptr<PreviewState> pv;
        {
            std::lock_guard l(channelsMutex_);
            auto it = previews_.find(target);
            // A size change is handled by reconcile() between frames (it needs a GPU wait).
            if (it != previews_.end() && (it->second->w != pw || it->second->h != ph)) continue;
            if (it == previews_.end()) {
                pv = std::make_shared<PreviewState>();
                pv->target = target;
                pv->w = pw;
                pv->h = ph;
                pv->tex = renderer_.createTexture(pw, ph, TexUsage::RenderTarget);
                pv->rb = renderer_.createReadback(pw, ph, 2);
                previews_[target] = pv;
                previewByReadback_[pv->rb] = pv;
            } else {
                pv = it->second;
            }
        }
        int slot = pv->ring.acquire();
        if (slot < 0) continue;
        Uniforms u = makeUniforms(0, 0, float(pw), float(ph));
        if (draw) {
            u = draw->u;
            u.dstRect[2] = u.sizes[2] = float(pw);
            u.dstRect[3] = u.sizes[3] = float(ph);
        } else {
            u.crop[2] = u.sizes[0] = view_.w;
            u.crop[3] = u.sizes[1] = view_.h;
        }
        renderer_.beginPass(pv->tex, nullptr);
        renderer_.drawTextured(src, u);
        renderer_.endPass();
        renderer_.copyToReadback(pv->tex, pv->rb, slot);
    }
}

// ---------------------------------------------------------------------------
// Keeping GPU resources in step with the layout
// ---------------------------------------------------------------------------

std::unique_ptr<VideoSink> Engine::makeSink(const std::string& ndiName, std::string& kind) {
    if (ndi_) {
        // A sender that just closed can hold the name for a moment; retry briefly.
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (auto s = ndi_->createSender(ndiName)) return s;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::cerr << "[engine] NDI sender '" << ndiName << "' failed; using null sink\n";
    }
    return createNullSink();
}

void Engine::restartVideo(const std::string& sourceId) {
    std::lock_guard l(videoRestartMutex_);
    videoRestarts_.push_back(sourceId);
}

void Engine::destroySource(SourceState& s) {
    s.rx.reset();
    s.video.reset();  // before its textures: hardware decoders hold GPU surfaces
    for (int i = 0; i < s.texCount; ++i) renderer_.destroyTexture(s.tex[i]);
    for (TexId& t : s.packed)
        if (t) renderer_.destroyTexture(t), t = 0;
    s.texCount = 0;
    s.hasFrame = false;
}

// Display outputs: a window opened on the monitor an output names, closed when the output goes,
// is turned off or names another monitor. One that couldn't open is tried again when asked.
void Engine::freeRing(DisplayOut& d) {
    for (TexId t : d.ring) renderer_.destroyTexture(t);
    d.ring.clear();
    d.ringW = d.ringH = d.head = d.filled = 0;
}

// Monitors in use by display outputs, across all canvases: one window per monitor (two would take
// turns on top of each other, the screen flashing between them).
namespace {
std::mutex monitorsMutex;
std::map<std::string, std::string> monitorOwners;  // monitor -> "canvas / output"
}  // namespace

void Engine::closeDisplay(DisplayOut& d) {
    if (d.claimed) {
        std::lock_guard l(monitorsMutex);
        monitorOwners.erase(d.monitor);
        d.claimed = false;
    }
    freeRing(d);
    if (d.id) renderer_.destroyDisplay(d.id);
    d.id = 0;
}

// Display outputs: a window opened on the monitor an output names, closed when the output goes,
// is turned off, names another monitor or another mode. One that couldn't open is tried again when asked.
void Engine::syncDisplays(const Layout& layout, bool retryFailed) {
    for (auto it = displays_.begin(); it != displays_.end();) {
        const OutputConfig* o = layout.findOutput(it->first);
        bool keep = o && o->enabled && o->display == it->second.monitor && o->modeWidth == it->second.modeW && o->modeHeight == it->second.modeH &&
                    o->modeHz == it->second.modeHz && (it->second.id || !retryFailed);
        if (keep) {
            ++it;
            continue;
        }
        closeDisplay(it->second);
        it = displays_.erase(it);
    }
    for (auto& o : layout.outputs) {
        if (o.display.empty() || !o.enabled || displays_.count(o.id)) continue;
        DisplayOut d;
        d.monitor = o.display;
        d.modeW = o.modeWidth;
        d.modeH = o.modeHeight;
        d.modeHz = o.modeHz;
        for (auto& [oid, other] : displays_)
            if (other.monitor == o.display) d.error = o.display + " already shows the output " + oid;
        if (d.error.empty()) {
            std::lock_guard l(monitorsMutex);
            auto [it, fresh] = monitorOwners.emplace(o.display, "the canvas \"" + layout.name + "\" (" + o.name + ")");
            if (fresh) d.claimed = true;
            else d.error = o.display + " is already used by " + it->second + ": a monitor shows one output";
        }
        // The mode first: the window and its swap chain take the monitor's size when they open.
        if (d.error.empty() && o.modeWidth > 0 && !renderer_.setDisplayMode(o.display, o.modeWidth, o.modeHeight, o.modeHz, d.modeError))
            std::cerr << "[engine] display output '" << o.id << "': " << d.modeError << "\n";
        if (d.error.empty()) d.id = renderer_.createDisplay(o.display, d.error);
        if (!d.id && !retryFailed) std::cerr << "[engine] display output '" << o.id << "': " << d.error << "\n";
        displays_[o.id] = d;
    }
    std::lock_guard l(displayStatusMutex_);
    json status = json::object();
    for (auto& [oid, d] : displays_) {
        // Keeps what reached the monitor (refreshed every 30 frames) while it's the same window.
        json j = displayStatus_.contains(oid) && d.id && displayStatus_[oid].value("open", false) && displayStatus_[oid].value("monitor", "") == d.monitor
                     ? displayStatus_[oid] : json::object();
        j.update(displayStatusOf(d.monitor, d.error, d.modeError, d.id != 0));
        if (d.modeError.empty()) j.erase("modeError");
        status[oid] = j;
    }
    displayStatus_ = status;
}

void Engine::reconcile(const Layout& layout) {
    // Sound outputs line up with the wall by default: a frame reaches the screens about half a
    // frame (waiting for the next render) + ~55 ms (rendering, NDI to the receivers, their
    // refresh and scan-out) + their buffer after it arrives here. The TVs' own processing isn't
    // known: each output's trim adds it.
    audio_->configure(layout.audioOutputs, layout.receiverBufferMs + 500.0 / layout.frameRate + 55.0);
    bool canvasChanged = layout.canvas.width != canvasW_ || layout.canvas.height != canvasH_;
    bool outputsResized = false;
    {
        std::lock_guard l(channelsMutex_);
        for (auto& [id, ch] : channels_) {
            const OutputConfig* o = layout.findOutput(id);
            if (!o || o->width != ch->width || o->height != ch->height) outputsResized = true;
        }
    }
    if (canvasChanged || outputsResized) {
        // Preview sizes depend on these; drop them all (recreated on demand).
        renderer_.waitIdle();
        std::lock_guard l(channelsMutex_);
        for (auto& [id, pv] : previews_) {
            renderer_.destroyReadback(pv->rb);
            renderer_.destroyTexture(pv->tex);
        }
        previews_.clear();
        previewByReadback_.clear();
    }

    // Canvas
    if (canvasChanged) {
        // reached only after the waitIdle above, so the fade texture is not in use
        if (fadeTex_) renderer_.destroyTexture(fadeTex_);
        fadeTex_ = 0;
        fadeMs_ = 0;
        if (canvas_) renderer_.destroyTexture(canvas_);
        canvas_ = renderer_.createTexture(layout.canvas.width, layout.canvas.height, TexUsage::RenderTarget);
        canvasW_ = layout.canvas.width;
        canvasH_ = layout.canvas.height;
    }

    // Sources: recreate only those whose type, NDI name or pattern size changed. An NDI
    // source that goes away hands its receiver (and last frame) to a new source showing
    // the same stream, so recalling a preset doesn't reconnect streams it keeps.
    std::multimap<std::string, SourceState> spare;
    for (auto it = sources_.begin(); it != sources_.end();) {
        const SourceConfig* c = layout.findSource(it->first);
        bool keep = c && c->type == it->second.cfg.type && c->ndiName == it->second.cfg.ndiName &&
                    c->image == it->second.cfg.image && c->video == it->second.cfg.video && c->device == it->second.cfg.device &&
                    (c->type != SourceType::TestPattern || (c->rect.w == it->second.w && c->rect.h == it->second.h));
        if (keep) {
            if (c->animated != it->second.cfg.animated) it->second.patternDrawn = false;
            SourceState& v = it->second;
            if (v.video) {
                if (c->loop != v.cfg.loop) v.video->setLoop(c->loop);
                double pos = std::chrono::duration<double>(Clock::now() - v.playStart).count();
                if (!c->playing && v.pausedAt < 0) v.pausedAt = pos;  // pause: hold the position
                if (c->playing && v.pausedAt >= 0) {                  // resume from it
                    v.playStart = Clock::now() - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(v.pausedAt));
                    v.pausedAt = -1;
                }
            }
            it->second.cfg = *c;
            ++it;
        } else {
            if (it->second.cfg.type == SourceType::Ndi && it->second.rx) spare.emplace(it->second.cfg.ndiName, std::move(it->second));
            else destroySource(it->second);
            it = sources_.erase(it);
        }
    }
    for (auto& c : layout.sources) {
        if (sources_.count(c.id)) continue;
        if (c.type == SourceType::Ndi) {
            auto sp = spare.find(c.ndiName);
            if (sp != spare.end()) {
                SourceState& s = sources_[c.id];
                s = std::move(sp->second);
                s.cfg = c;
                spare.erase(sp);
                continue;
            }
        }
        SourceState& s = sources_[c.id];
        s.cfg = c;
        if (c.type == SourceType::TestPattern) {
            s.w = c.rect.w;
            s.h = c.rect.h;
            s.tex[0] = renderer_.createTexture(s.w, s.h, TexUsage::RenderTarget);
            s.texCount = 1;
            s.hasFrame = true;
        } else if (c.type == SourceType::Ndi && ndi_) {
            s.rx = ndi_->createReceiver(c.ndiName);
        } else if (c.type == SourceType::Capture) {
            // Shared with any other canvas showing the same device (it can only be opened once).
            if (c.device.empty()) s.error = "no capture device chosen";
            else if (!(s.rx = openCapture(c.device, s.error))) std::cerr << "[engine] capture '" << c.device << "': " << s.error << "\n";
        } else if (c.type == SourceType::Video) {
            bool safe = !c.video.empty() && c.video.find_first_of("/\\") == std::string::npos && c.video.find("..") == std::string::npos;
            if (!safe) s.error = c.video.empty() ? "no video chosen" : "bad video name";
            else {
                std::string err;
                s.video = openVideoFile(utf8(mediaDir_ / pathFromUtf8(c.video)), renderer_.nativeDevice(), c.loop, err);
                if (!s.video) s.error = "cannot play " + c.video + ": " + err;
            }
            s.playStart = Clock::now();
            s.pausedAt = c.playing ? -1 : 0;
        } else if (c.type == SourceType::Image) {
            // Decoded here, uploaded on the next frame. Names are plain file names inside the media folder.
            std::string err;
            bool safe = !c.image.empty() && c.image.find_first_of("/\\") == std::string::npos && c.image.find("..") == std::string::npos;
            if (!safe) s.error = c.image.empty() ? "no image chosen" : "bad image name";
            else if (!loadImageBGRA(utf8(mediaDir_ / pathFromUtf8(c.image)), s.pending, s.w, s.h, err)) s.error = "image not found or unreadable: " + err;
            else {
                for (size_t i = 3; i < s.pending.size() && !s.hasAlpha; i += 4) s.hasAlpha = s.pending[i] < 255;
                s.tex[0] = renderer_.createTexture(s.w, s.h, TexUsage::Upload);
                s.texCount = 1;
            }
        }
    }
    for (auto& [name, s] : spare) destroySource(s);

    // Fallback images: load the ones the layout names, free the rest.
    std::set<std::string> wanted;
    if (!layout.fallbackImage.empty()) wanted.insert(layout.fallbackImage);
    for (auto& w : layout.walls)
        if (!w.fallbackImage.empty()) wanted.insert(w.fallbackImage);
    for (auto it = fallbacks_.begin(); it != fallbacks_.end();) {
        if (wanted.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second.tex) renderer_.destroyTexture(it->second.tex);
        it = fallbacks_.erase(it);
    }
    for (auto& name : wanted) {
        if (fallbacks_.count(name) || name.find_first_of("/\\") != std::string::npos || name.find("..") != std::string::npos) continue;
        ImageTex& fb = fallbacks_[name];
        if (!loadImageBGRA(utf8(mediaDir_ / pathFromUtf8(name)), fb.pending, fb.w, fb.h, fb.error)) {
            std::cerr << "[engine] fallback image '" << name << "': " << fb.error << "\n";
            continue;
        }
        for (size_t i = 3; i < fb.pending.size() && !fb.hasAlpha; i += 4) fb.hasAlpha = fb.pending[i] < 255;
        fb.tex = renderer_.createTexture(fb.w, fb.h, TexUsage::Upload);
    }

    // Outputs: a channel is recreated only when its resolution or stream name
    // changes. Crop, rotation, scaling, source and enable apply live per frame.
    std::vector<std::shared_ptr<OutputChannel>> retired;
    {
        std::lock_guard l(channelsMutex_);
        for (auto it = channels_.begin(); it != channels_.end();) {
            const OutputConfig* o = layout.findOutput(it->first);
            bool keep = o && o->display.empty() && o->width == it->second->width && o->height == it->second->height &&
                        o->ndiName == it->second->ndiName && (o->pixelFormat == PixelFormat::Uyvy) == it->second->uyvy;
            if (keep) {
                ++it;
            } else {
                retired.push_back(it->second);
                channelByReadback_.erase(it->second->readback());
                it = channels_.erase(it);
            }
        }
    }
    if (!retired.empty()) {
        for (auto& ch : retired) ch->shutdown();  // also releases their NDI names
        renderer_.waitIdle();
        for (auto& ch : retired) ch->destroyGpuResources();
        retired.clear();
        // Frames in flight counted on the streams just closed: they can't arrive now, and that's
        // the change asked for, not a delivery failure. Not counted either way.
        std::lock_guard l(syncMutex_);
        sync_.clear();
    }
    for (auto& o : layout.outputs) {
        if (!o.display.empty()) continue;  // a display output: no stream (see syncDisplays)
        {
            std::lock_guard l(channelsMutex_);
            if (channels_.count(o.id)) continue;
        }
        std::string kind;
        auto ch = std::make_shared<OutputChannel>(o.id, o.width, o.height, o.ndiName, layout.frameRate,
                                                  o.pixelFormat == PixelFormat::Uyvy, renderer_, makeSink(o.ndiName, kind));
        ch->onDelivered = [this](uint64_t f) { syncDelivered(f, true); };
        ch->onDropped = [this](uint64_t f) { syncDelivered(f, false); };
        std::lock_guard l(channelsMutex_);
        channelByReadback_[ch->readback()] = ch;
        channels_[o.id] = ch;
    }
    syncDisplays(layout, false);
    view_ = {};  // may point at a source texture destroyed above; set again next frame
    std::cerr << "[engine] applied layout '" << layout.id << "': canvas " << canvasW_ << "x" << canvasH_ << ", "
              << sources_.size() << " source(s), " << layout.outputs.size() << " output(s)\n";
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

json Engine::metrics() {
    auto layout = store_.snapshot(canvasId_);
    if (!layout) return json::object();
    json outputs = json::array();
    std::vector<std::pair<std::string, std::shared_ptr<OutputChannel>>> chans;
    {
        std::lock_guard l(channelsMutex_);
        for (auto& [id, ch] : channels_) chans.emplace_back(id, ch);
    }
    uint64_t minId = UINT64_MAX, maxId = 0;
    double readbackBytesPerSec = 0;
    json displays;
    {
        std::lock_guard l(displayStatusMutex_);
        displays = displayStatus_;
    }
    for (auto& o : layout->outputs) {
        if (!o.display.empty()) {
            // A display output: the monitor it's on and what reached it.
            json d = displays.contains(o.id) ? displays[o.id] : json{{"monitor", o.display}, {"error", "starting"}, {"open", false}};
            json m = {{"id", o.id}, {"name", o.name}, {"sink", "display"}, {"display", o.display}, {"enabled", o.enabled}};
            for (auto& [k, v] : d.items()) m[k] = v;
            outputs.push_back(m);
            continue;
        }
        auto it = std::find_if(chans.begin(), chans.end(), [&](auto& p) { return p.first == o.id; });
        if (it == chans.end()) continue;
        OutputMetrics m = it->second->metrics();
        if (o.enabled) readbackBytesPerSec += double(it->second->bytesPerFrame()) * m.fps;
        if (o.enabled && m.sent > 0) {
            minId = std::min(minId, m.lastFrameId);
            maxId = std::max(maxId, m.lastFrameId);
        }
        outputs.push_back({{"id", o.id},
                           {"name", o.name},
                           {"ndiName", o.ndiName},
                           {"sink", it->second->sinkKind()},
                           {"pixelFormat", it->second->uyvy ? "uyvy" : "bgra"},
                           {"enabled", o.enabled},
                           {"width", o.width},
                           {"height", o.height},
                           {"fps", m.fps},
                           {"framesSent", m.sent},
                           {"droppedNoBuffer", m.droppedNoSlot},
                           {"droppedQueue", m.droppedQueue},
                           {"queueDepth", m.queueDepth},
                           {"buffersInUse", m.slotsInUse},
                           {"sendMs", m.sendMs},
                           {"sendMsMax", m.sendMsMax},
                           {"latencyMs", m.latencyMs},
                           {"lastFrameId", m.lastFrameId},
                           {"connections", m.connections}});
    }
    json sync;
    {
        std::lock_guard l(syncMutex_);
        uint64_t total = framesComplete_ + framesPartial_;
        sync = {{"framesComplete", framesComplete_},
                {"framesPartial", framesPartial_},
                {"completeRatio", total ? double(framesComplete_) / double(total) : 1.0},
                {"lastFrameIdSpread", maxId >= minId && maxId ? maxId - minId : 0}};
    }
    json src;
    {
        std::lock_guard l(statusMutex_);
        src = sourceStatus_;
    }
    return {{"engine",
             {{"backend", renderer_.backendName()},
              {"device", renderer_.deviceName()},
              {"targetFps", targetFps_.load()},
              {"renderFps", renderRate_.rate()},
              {"frameId", frameId_.load()},
              {"encodeMs", encodeMs_.value()},
              {"gpuWaitMs", waitMs_.value()},
              {"gpuMs", gpuMs_.value()},
              {"frameMs", frameMs_.value()},
              {"frameMsMax", frameMs_.takeMax()},
              {"tickJitterMs", jitterMs_.value()},
              {"tickJitterMsMax", jitterMs_.takeMax()},
              {"gpuUtilization", renderer_.gpuUtilization()},
              {"readbackMBps", readbackBytesPerSec / 1e6},
              {"lateFrames", lateFrames_.load()},
              {"directFrames", directFrames_.load()},
              {"ndi", ndi_ ? ndi_->version() : "unavailable"},
              {"layout", layout->id},
              {"uptimeSec", std::chrono::duration<double>(Clock::now() - start_).count()}}},
            {"sources", src},
            {"outputs", outputs},
            {"sync", sync}};
}

json Engine::sourceStatus() {
    std::lock_guard l(statusMutex_);
    return sourceStatus_;
}

std::vector<std::string> Engine::discoveredNdiSources() { return ndi_ ? ndi_->discoveredSources() : std::vector<std::string>{}; }

bool Engine::previewJpeg(const std::string& target, std::string& jpeg) {
    {
        std::lock_guard l(previewReqMutex_);
        previewRequests_[target] = Clock::now();
    }
    std::shared_ptr<PreviewState> pv;
    {
        std::lock_guard l(channelsMutex_);
        auto it = previews_.find(target);
        if (it != previews_.end()) pv = it->second;
    }
    if (!pv) return false;
    std::vector<uint8_t> bgra;
    {
        std::lock_guard l(pv->m);
        if (!pv->has) return false;
        bgra = pv->bgra;
    }
    jpeg = encodeJpegBGRA(bgra.data(), pv->w, pv->h, pv->w * 4, 80);
    return !jpeg.empty();
}

bool Engine::snapshotJpeg(const std::string& outputId, std::string& jpeg, uint64_t& frameId) {
    std::vector<uint8_t> bgra;
    int w = 0, h = 0;
    if (!snapshotBgra(outputId, bgra, w, h, frameId)) return false;
    jpeg = encodeJpegBGRA(bgra.data(), w, h, w * 4, 92);
    return !jpeg.empty();
}

bool Engine::snapshotBgra(const std::string& outputId, std::vector<uint8_t>& bgra, int& w, int& h, uint64_t& frameId) {
    std::shared_ptr<OutputChannel> ch;
    {
        std::lock_guard l(channelsMutex_);
        auto it = channels_.find(outputId);
        if (it == channels_.end()) return false;
        ch = it->second;
    }
    std::vector<uint8_t> data;
    if (!ch->snapshot(data, w, h, frameId, 1000)) return false;
    if (ch->uyvy) decodeUyvyToBgra(data.data(), w, h, w * 2, bgra);
    else bgra.swap(data);
    return true;
}

}  // namespace mosaic
