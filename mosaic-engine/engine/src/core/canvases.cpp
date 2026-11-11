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

void Canvases::sync() {
    auto slots = store_.canvases();
    std::vector<std::shared_ptr<CanvasRun>> stopping;  // destroyed outside the lock (joins render threads)
    {
        std::lock_guard l(m_);
        for (auto it = runs_.begin(); it != runs_.end();) {
            bool keep = std::any_of(slots.begin(), slots.end(), [&](auto& c) { return c.id == it->first; });
            if (keep) {
                ++it;
                continue;
            }
            std::cerr << "[canvas] " << it->first << " stopped\n";
            stopping.push_back(std::move(it->second));
            it = runs_.erase(it);
        }
        for (auto& c : slots) {
            if (runs_.count(c.id)) continue;
            auto run = std::make_shared<CanvasRun>();
            run->renderer = createRenderer(rendererOptions_, run->error);
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
