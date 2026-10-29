#include "core/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

#include "util/image.hpp"
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

Uniforms makeUniforms(float dx, float dy, float dw, float dh) {
    Uniforms u{};
    u.dstRect[0] = dx;
    u.dstRect[1] = dy;
    u.dstRect[2] = dw;
    u.dstRect[3] = dh;
    u.sizes[2] = dw;
    u.sizes[3] = dh;
    u.bg[3] = 1.f;
    return u;
}
}  // namespace

Engine::Engine(Renderer& renderer, LayoutStore& store, NdiSystem* ndi) : renderer_(renderer), store_(store), ndi_(ndi) {
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
    // Tear down: stop senders, let the GPU finish, then free resources.
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
    sources_.clear();
    if (canvas_) renderer_.destroyTexture(canvas_);
    canvas_ = 0;
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

void Engine::loop() {
    raiseThreadPriority(ThreadRole::Render);
    PreciseSleeper sleeper;
    auto next = Clock::now();
    while (running_) {
        auto layout = store_.active();
        if (store_.version() != appliedVersion_) {
            appliedVersion_ = store_.version();
            reconcile(*layout);
        }
        targetFps_ = layout->frameRate;
        auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / layout->frameRate));

        sleeper.sleepUntil(next);
        auto tick = Clock::now();
        jitterMs_.add(msBetween(next, tick));  // how late the tick fired
        if (tick - next > interval) {
            // Fell behind by more than a frame: skip ahead instead of bursting.
            lateFrames_++;
            next = tick;
        }
        next += interval;

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

    updateSources(layout, frameId, seconds);

    // Canvas: every enabled source drawn into its rectangle.
    renderer_.beginPass(canvas_, layout.canvas.background.data());
    for (auto& sc : layout.sources) {
        if (!sc.enabled) continue;
        auto it = sources_.find(sc.id);
        if (it == sources_.end()) continue;
        SourceState& s = it->second;
        Uniforms u = makeUniforms(float(sc.rect.x), float(sc.rect.y), float(sc.rect.w), float(sc.rect.h));
        if (sc.type == SourceType::Solid || !s.hasFrame) {
            const float* c = sc.type == SourceType::Solid ? sc.color.data() : kNoSignal;
            std::copy(c, c + 4, u.bg);
            u.iparams[3] = 1;  // solid
            renderer_.drawTextured(canvas_, u);
            continue;
        }
        u.crop[2] = u.sizes[0] = float(s.w);
        u.crop[3] = u.sizes[1] = float(s.h);
        renderer_.drawTextured(s.tex[s.current], u);
    }
    renderer_.endPass();

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
    for (auto& ch : active) {
        const OutputConfig* o = layout.findOutput(ch->id);
        int slot = ch->acquireSlot({frameId, tick, timecode});
        if (slot < 0) {
            ch->countNoSlotDrop();
            continue;
        }
        TexId src = canvas_;
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
        Uniforms u = makeUniforms(0, 0, float(ch->width), float(ch->height));
        u.crop[0] = float(o->crop.x);
        u.crop[1] = float(o->crop.y);
        u.crop[2] = float(o->crop.w);
        u.crop[3] = float(o->crop.h);
        u.sizes[0] = sw;
        u.sizes[1] = sh;
        std::copy(layout.canvas.background.begin(), layout.canvas.background.end(), u.bg);
        u.iparams[0] = static_cast<uint32_t>(frameId);
        u.iparams[1] = static_cast<uint32_t>(o->rotation);
        u.iparams[2] = scaleModeIndex(o->scaleMode);
        u.iparams[3] = solid ? 1u : 0u;
        renderer_.beginPass(ch->target(), layout.canvas.background.data());
        renderer_.drawTextured(src, u);
        renderer_.endPass();
        if (ch->uyvy) {
            // Pack to UYVY on the GPU: half the readback bytes, and NDI's native format.
            renderer_.beginPass(ch->packed(), nullptr);
            renderer_.drawUYVY(ch->target(), makeUniforms(0, 0, float(ch->width / 2), float(ch->height)));
            renderer_.endPass();
        }
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

void Engine::updateSources(const Layout& layout, uint64_t frameId, double seconds) {
    json status = json::array();
    for (auto& [id, s] : sources_) {
        json st = {{"id", id}, {"type", toString(s.cfg.type)}, {"width", s.w}, {"height", s.h}};
        if (s.cfg.type == SourceType::TestPattern) {
            Uniforms u = makeUniforms(0, 0, float(s.w), float(s.h));
            u.fparams[0] = s.cfg.animated ? static_cast<float>(seconds) : 0.f;
            u.fparams[1] = kTileW;
            u.fparams[2] = kTileH;
            u.iparams[0] = s.cfg.animated ? static_cast<uint32_t>(frameId) : 0u;
            renderer_.beginPass(s.tex[0], nullptr);
            renderer_.drawTestPattern(u);
            renderer_.endPass();
            st["status"] = "ok";
            st["inputFps"] = renderRate_.rate();
        } else if (s.cfg.type == SourceType::Ndi) {
            if (!s.rx) {
                st["status"] = ndi_ ? "receiver failed" : "NDI unavailable";
            } else {
                InputFrame f;
                if (s.rx->acquireLatest(f)) {
                    if (f.width != s.w || f.height != s.h) {
                        // New resolution: replace the upload textures (in-flight
                        // frames keep the old ones alive until they finish).
                        for (int i = 0; i < s.texCount; ++i) renderer_.destroyTexture(s.tex[i]);
                        s.texCount = 3;
                        for (int i = 0; i < 3; ++i) s.tex[i] = renderer_.createTexture(f.width, f.height, TexUsage::Upload);
                        s.w = f.width;
                        s.h = f.height;
                    }
                    // Rotate through 3 textures so we never write one the GPU may still read.
                    s.current = (s.current + 1) % s.texCount;
                    renderer_.uploadBGRA(s.tex[s.current], f.data, f.stride);
                    s.rx->release(f);
                    s.hasFrame = true;
                }
                st["status"] = s.rx->connected() ? "ok" : (s.hasFrame ? "no signal (holding last frame)" : "waiting for source");
                st["inputFps"] = s.rx->inputFps();
                st["framesReceived"] = s.rx->framesReceived();
                st["width"] = s.w;
                st["height"] = s.h;
            }
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
        if (target == "canvas") {
            src = canvas_;
            sw = canvasW_;
            sh = canvasH_;
        } else {
            std::lock_guard l(channelsMutex_);
            auto it = channels_.find(target);
            const OutputConfig* o = layout.findOutput(target);
            if (it == channels_.end() || !o || !o->enabled) continue;
            src = it->second->target();
            sw = it->second->width;
            sh = it->second->height;
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
        u.crop[2] = u.sizes[0] = float(sw);
        u.crop[3] = u.sizes[1] = float(sh);
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
        if (auto s = ndi_->createSender(ndiName)) return s;
        std::cerr << "[engine] NDI sender '" << ndiName << "' failed; using null sink\n";
    }
    return createNullSink();
}

void Engine::destroySource(SourceState& s) {
    s.rx.reset();
    for (int i = 0; i < s.texCount; ++i) renderer_.destroyTexture(s.tex[i]);
    s.texCount = 0;
    s.hasFrame = false;
}

void Engine::reconcile(const Layout& layout) {
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
        if (canvas_) renderer_.destroyTexture(canvas_);
        canvas_ = renderer_.createTexture(layout.canvas.width, layout.canvas.height, TexUsage::RenderTarget);
        canvasW_ = layout.canvas.width;
        canvasH_ = layout.canvas.height;
    }

    // Sources: recreate only those whose type, NDI name or pattern size changed.
    for (auto it = sources_.begin(); it != sources_.end();) {
        const SourceConfig* c = layout.findSource(it->first);
        bool keep = c && c->type == it->second.cfg.type && c->ndiName == it->second.cfg.ndiName &&
                    (c->type != SourceType::TestPattern || (c->rect.w == it->second.w && c->rect.h == it->second.h));
        if (keep) {
            it->second.cfg = *c;
            ++it;
        } else {
            destroySource(it->second);
            it = sources_.erase(it);
        }
    }
    for (auto& c : layout.sources) {
        if (sources_.count(c.id)) continue;
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
        }
    }

    // Outputs: a channel is recreated only when its resolution or stream name
    // changes. Crop, rotation, scaling, source and enable apply live per frame.
    std::vector<std::shared_ptr<OutputChannel>> retired;
    {
        std::lock_guard l(channelsMutex_);
        for (auto it = channels_.begin(); it != channels_.end();) {
            const OutputConfig* o = layout.findOutput(it->first);
            bool keep = o && o->width == it->second->width && o->height == it->second->height &&
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
        for (auto& ch : retired) ch->shutdown();
        renderer_.waitIdle();
        for (auto& ch : retired) ch->destroyGpuResources();
    }
    for (auto& o : layout.outputs) {
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
    std::cerr << "[engine] applied layout '" << layout.id << "': canvas " << canvasW_ << "x" << canvasH_ << ", "
              << sources_.size() << " source(s), " << layout.outputs.size() << " output(s)\n";
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

json Engine::metrics() {
    auto layout = store_.active();
    json outputs = json::array();
    std::vector<std::pair<std::string, std::shared_ptr<OutputChannel>>> chans;
    {
        std::lock_guard l(channelsMutex_);
        for (auto& [id, ch] : channels_) chans.emplace_back(id, ch);
    }
    uint64_t minId = UINT64_MAX, maxId = 0;
    double readbackBytesPerSec = 0;
    for (auto& o : layout->outputs) {
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
