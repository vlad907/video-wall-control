// Persists layouts as JSON files and publishes the running ones as immutable snapshots the
// render threads pick up without locking.
//
// Canvases: several layouts can run at once, each on its own render thread. A canvas has a
// stable id ("canvas-1") and shows one layout; showing another layout on it changes it in place,
// like switching the layout did before canvases. The first canvas is the primary one. "The
// active layout" in the API means the layout of the canvas the current request is about: the
// editor names it per request (X-Mosaic-Canvas header, see setRequestCanvas); without one it's
// the primary canvas, as before canvases existed.
#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/layout.hpp"

namespace mosaic {

// One saved earlier version of a layout (data/history/<layout id>/<version>.json).
struct LayoutVersion {
    std::string version;  // file stem, sortable: "20261101-154212-123"
    std::string time;     // "2026-11-01 15:42:12"
    std::string name;
    int outputs = 0;
};

struct CanvasSlot {
    std::string id;      // "canvas-1"
    std::string layout;  // the layout it shows
};

struct LayoutSummary {
    std::string id, name;
    int outputs = 0;
    bool active = false;   // the canvas this request is about
    bool running = false;  // running as a canvas
};

class LayoutStore {
public:
    explicit LayoutStore(std::filesystem::path dataDir);

    // Loads saved layouts; seeds from examplesDir (or a built-in default) on first run.
    void load(const std::filesystem::path& examplesDir);

    // The layout of this request's canvas (or the primary canvas).
    std::shared_ptr<const Layout> active() const;
    // A canvas's layout, or null if there's no such canvas.
    std::shared_ptr<const Layout> snapshot(const std::string& canvasId) const;
    // Every canvas's layout, primary first.
    std::vector<std::shared_ptr<const Layout>> running() const;
    std::vector<CanvasSlot> canvases() const;
    std::string currentCanvas() const;  // this request's canvas id (the primary if it names none or a gone one)
    // Changes whenever any running canvas's layout changes.
    uint64_t version() const { return version_.load(); }

    // The canvas the calling thread's request is about ("" = the primary). Set per request by
    // the HTTP server; affects active(), mutateActive(), activate() and list() on this thread.
    static void setRequestCanvas(const std::string& canvasId);
    static const std::string& requestCanvas();
    // Runs a layout as another canvas (after the others); returns the canvas id, "" if there's no
    // such layout or it's already on a canvas.
    std::string startCanvas(const std::string& layoutId);
    // Stops a canvas (its layout stays saved). False if there's no such canvas or it's the last one.
    bool stopCanvas(const std::string& canvasId);

    std::vector<LayoutSummary> list() const;
    std::optional<Layout> get(const std::string& id) const;

    // Validates, writes to disk, and republishes if it is the active layout.
    // Returns problems found during normalization (empty = clean).
    std::vector<std::string> save(Layout layout);
    // Shows another layout on this request's canvas (the canvas takes that layout's id). False
    // if there's no such layout; `alreadyRunning` is set if it's on another canvas (not changed).
    bool activate(const std::string& id, bool* alreadyRunning = nullptr);
    bool remove(const std::string& id);  // not while it's running

    // Copy-modify-publish on the active layout. fn returns false to abort.
    bool mutateActive(const std::function<bool(Layout&)>& fn, std::vector<std::string>* problems = nullptr);

    // Earlier versions, newest first. A version is kept before a change when the last one is
    // over 30 s old (so a drag doesn't make hundreds), always before a restore or a delete,
    // up to 50 per layout.
    std::vector<LayoutVersion> history(const std::string& id) const;
    // Puts an earlier version back (the current one goes into the history first).
    // Returns false if there is no such version.
    bool restore(const std::string& id, const std::string& version, std::vector<std::string>* problems = nullptr);

private:
    std::filesystem::path fileFor(const std::string& id) const;
    void snapshot(const std::string& id, bool force) const;  // copies the file on disk into the history
    void writeFile(const Layout& l) const;
    void writeState() const;
    void publish(const Layout& l);  // if it's running
    const CanvasSlot& current() const;  // this request's canvas, else the primary (lock held)
    bool layoutRunning(const std::string& layoutId) const;  // lock held

    std::filesystem::path dir_, layoutsDir_, historyDir_;
    mutable std::map<std::string, std::chrono::steady_clock::time_point> lastSnapshot_;
    mutable std::mutex mutex_;
    std::vector<Layout> layouts_;
    std::vector<CanvasSlot> canvases_;  // primary first
    std::map<std::string, std::shared_ptr<const Layout>> snapshots_;  // by canvas id
    std::atomic<uint64_t> version_{0};
};

}  // namespace mosaic
