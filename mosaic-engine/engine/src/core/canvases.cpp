#include "core/canvases.hpp"

#include <algorithm>
#include <iostream>
#include <set>

namespace mosaic {

CanvasRun::~CanvasRun() {
    if (engine) engine->stop();
    engine.reset();  // before the device it renders on
    renderer.reset();
}

Canvases::Canvases(LayoutStore& store, NdiSystem* ndi, RendererOptions renderer, std::filesystem::path mediaDir)
    : store_(store), ndi_(ndi), rendererOptions_(renderer), mediaDir_(std::move(mediaDir)) {}

Canvases::~Canvases() {
    std::map<std::string, std::shared_ptr<CanvasRun>> runs;
    {
        std::lock_guard l(m_);
        runs.swap(runs_);
    }
}

std::vector<GpuInfo> Canvases::gpus() const {
    std::lock_guard l(gpuMutex_);
    auto now = std::chrono::steady_clock::now();
    if (gpusAt_ == std::chrono::steady_clock::time_point{} || now - gpusAt_ > std::chrono::seconds(5)) {
        gpus_ = listGpus();
        gpusAt_ = now;
    }
    return gpus_;
}

int Canvases::resolveGpu(const Layout& l) const {
    if (l.gpu < 0) return -1;
    auto list = gpus();
    if (l.gpuName.empty() || (l.gpu < static_cast<int>(list.size()) && list[static_cast<size_t>(l.gpu)].name == l.gpuName)) return l.gpu;
    for (auto& g : list)
        if (g.name == l.gpuName) return g.index;  // moved: the same card under another number
    return l.gpu;  // not found by name: as numbered (it fails clearly if there's no such number)
}

void Canvases::sync() {
    auto slots = store_.canvases();
    std::vector<std::shared_ptr<CanvasRun>> stopping;  // destroyed outside the lock (joins render threads)
    std::map<std::string, int> wantGpu;
    for (auto& c : slots)
        if (auto l = store_.snapshot(c.id)) wantGpu[c.id] = resolveGpu(*l);
    {
        std::lock_guard l(m_);
        for (auto it = runs_.begin(); it != runs_.end();) {
            bool keep = std::any_of(slots.begin(), slots.end(), [&](auto& c) { return c.id == it->first; });
            auto& run = it->second;
            bool failed = !run->engine ? !run->error.empty() : run->engine->failed();
            if (keep && failed && std::chrono::steady_clock::now() - run->started > std::chrono::seconds(3 + std::min(run->restarts, 10) * 3)) {
                // Its GPU device was lost or it couldn't start: try again (waiting longer each time).
                std::cerr << "[canvas] " << it->first << " starts again (" << (run->engine ? run->engine->failure() : run->error) << ")\n";
                restarts_[it->first] = run->restarts + 1;
                keep = false;
            }
            if (keep && wantGpu.count(it->first) && wantGpu[it->first] != it->second->gpu) {
                // Moved to another GPU: started again there (its streams pause for a moment).
                std::cerr << "[canvas] " << it->first << " moves to " << (wantGpu[it->first] < 0 ? std::string("the default GPU") : "GPU " + std::to_string(wantGpu[it->first] + 1)) << "\n";
                keep = false;
            }
            if (keep) {
                ++it;
                continue;
            }
            std::cerr << "[canvas] " << it->first << " stopped\n";
            stopping.push_back(std::move(it->second));
            it = runs_.erase(it);
        }
    }
    // Stopped (render threads joined, NDI senders closed) before anything starts again, so a
    // canvas moving to another GPU never sends its streams twice. A request still holding one
    // keeps it until it finishes.
    stopping.clear();
    {
        std::lock_guard l(m_);
        for (auto& c : slots) {
            if (runs_.count(c.id)) continue;
            auto run = std::make_shared<CanvasRun>();
            RendererOptions ro = rendererOptions_;
            ro.gpu = run->gpu = wantGpu.count(c.id) ? wantGpu[c.id] : -1;
            run->restarts = restarts_.count(c.id) ? restarts_[c.id] : 0;
            run->renderer = createRenderer(ro, run->error);
            if (run->renderer) {
                run->engine = std::make_unique<Engine>(*run->renderer, store_, ndi_, c.id);
                run->engine->setMediaDir(mediaDir_);
                run->engine->start();
                std::cerr << "[canvas] " << c.id << " (" << c.layout << ") on " << run->renderer->deviceName() << "\n";
            } else {
                std::cerr << "[canvas] " << c.id << ": GPU init failed: " << run->error << "\n";
            }
            runs_[c.id] = run;
        }
    }
}

std::shared_ptr<CanvasRun> Canvases::get(const std::string& canvasId) const {
    std::lock_guard l(m_);
    auto it = runs_.find(canvasId);
    return it == runs_.end() ? nullptr : it->second;
}

std::shared_ptr<CanvasRun> Canvases::current() const { return get(store_.currentCanvas()); }

std::shared_ptr<CanvasRun> Canvases::primary() const {
    auto slots = store_.canvases();
    return slots.empty() ? nullptr : get(slots.front().id);
}

std::vector<std::pair<std::string, std::shared_ptr<CanvasRun>>> Canvases::all() const {
    std::vector<std::pair<std::string, std::shared_ptr<CanvasRun>>> out;
    for (auto& c : store_.canvases()) out.emplace_back(c.id, get(c.id));
    return out;
}

std::vector<std::string> Canvases::problems() const {
    std::vector<std::string> out;
    std::map<std::string, std::string> owner;  // NDI name -> layout sending it
    for (auto& l : store_.running())
        for (auto& o : l->outputs) {
            if (!o.enabled || o.ndiName.empty()) continue;
            auto [it, fresh] = owner.emplace(o.ndiName, l->name.empty() ? l->id : l->name);
            if (!fresh && it->second != (l->name.empty() ? l->id : l->name))
                out.push_back("NDI stream '" + o.ndiName + "' is sent by both " + it->second + " and " + (l->name.empty() ? l->id : l->name) +
                              ": rename one, or receivers may show the wrong canvas");
        }
    return out;
}

}  // namespace mosaic
