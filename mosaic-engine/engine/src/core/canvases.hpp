// The running canvases: one engine per canvas in the layout store (LayoutStore::canvases()),
// each with its own GPU device and render thread, started and stopped to match.
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/engine.hpp"
#include "core/layout_store.hpp"
#include "io/video_io.hpp"
#include "render/renderer.hpp"

namespace mosaic {

// One canvas's engine and the GPU device it renders on. Held by shared_ptr so a request that's
// using it finishes safely while the canvas is stopped.
struct CanvasRun {
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<Engine> engine;
    std::string error;  // why it couldn't start (no engine then)
    int gpu = -1;       // the GPU it was started on (RendererOptions::gpu)
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    int restarts = 0;   // times it was started again after failing
    ~CanvasRun();
};

class Canvases {
public:
    Canvases(LayoutStore& store, NdiSystem* ndi, RendererOptions renderer, std::filesystem::path mediaDir);
    ~Canvases();

    // Starts engines for new canvases and stops those of removed ones. Any thread.
    void sync();
    // A canvas's engine (null if there's no such canvas or it couldn't start).
    std::shared_ptr<CanvasRun> get(const std::string& canvasId) const;
    // This request's canvas (LayoutStore::currentCanvas()).
    std::shared_ptr<CanvasRun> current() const;
    std::shared_ptr<CanvasRun> primary() const;
    // Every canvas's run, in canvas order (an entry may have no engine: see error).
    std::vector<std::pair<std::string, std::shared_ptr<CanvasRun>>> all() const;
    // Problems across canvases (the same NDI stream name sent from two of them).
    std::vector<std::string> problems() const;
    // This PC's GPUs (cached for a few seconds).
    std::vector<GpuInfo> gpus() const;
    // Which GPU a layout asks for, by number, found again by name if the numbering changed.
    int resolveGpu(const Layout& l) const;
    const std::filesystem::path& mediaDir() const { return mediaDir_; }

private:
    LayoutStore& store_;
    NdiSystem* ndi_;
    RendererOptions rendererOptions_;
    std::filesystem::path mediaDir_;
    mutable std::mutex m_;
    std::map<std::string, std::shared_ptr<CanvasRun>> runs_;
    std::map<std::string, int> restarts_;  // by canvas: restarts after failures (for the back-off)
    mutable std::mutex gpuMutex_;
    mutable std::vector<GpuInfo> gpus_;
    mutable std::chrono::steady_clock::time_point gpusAt_{};
};

}  // namespace mosaic
