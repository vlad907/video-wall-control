// Puts frames on screen: one full-screen window per monitor, each showing one or more tiles
// (a tile is a stream drawn into a rectangle of the window). Only windows whose picture changes
// are drawn and presented (a flip-model window keeps showing its last picture), so a refresh
// costs as many windows as change in it. Windows: Direct3D 11. Elsewhere / tests: headless.
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "frames.hpp"

namespace mosaic::rx {

struct WindowSpec {
    int x = 0, y = 0, w = 1920, h = 1080;  // desktop rectangle (a monitor, or a spanned group of them)
};

// When each window's frames really reached its monitor (Windows: DXGI frame statistics).
struct PresentTiming {
    bool valid = false;             // the platform reported statistics for this window
    uint32_t presentCount = 0;      // number of the present just made
    uint32_t syncPresentCount = 0;  // the newest present that reached the screen ...
    uint32_t syncRefreshCount = 0;  // ... at this vertical refresh of the monitor ...
    int64_t syncMicros = 0;         // ... at this time (microseconds, one clock for all windows)
    int64_t refreshMicros = 0;      // the refresh (vertical blank) the loop pass that last presented this window started on ...
    int64_t submitMicros = 0;       // ... and when it presented it (same clock)
    bool presented = false;         // presented in the last present() call
};

struct TileDraw {
    int window = 0;
    int x = 0, y = 0, w = 0, h = 0;  // inside the window
    int stream = -1;                 // index of the stream texture; -1 = nothing (black)
    double cx = 0, cy = 0, cw = 1, ch = 1;  // the part of the stream's picture shown, as fractions of it (all of it by default)
    int back = 0;                    // pictures back in the stream's history (0 = newest); see kPictureHistory
};

// Each stream keeps its last few pictures, so a display Windows shows sooner than the others can
// be held back a refresh or two and still be drawn (sync compensation).
constexpr int kPictureHistory = 4;

struct Upload {
    int stream = 0;
    const RxFrame* frame = nullptr;
};

class Presenter {
public:
    virtual ~Presenter() = default;
    virtual std::string name() const = 0;
    // Creates the windows (replacing any before). False if it can't.
    virtual bool configure(const std::vector<WindowSpec>& windows, std::string& error) = 0;
    // Uploads new pictures (one per stream at most), each becoming its stream's newest; the
    // copies run in parallel.
    virtual void upload(const std::vector<Upload>& frames) = 0;
    // Draws and presents the windows with which[w] set. A window with flash[w] set is drawn plain
    // white instead (flash test: filmed, it shows at a glance how far apart the displays are).
    // A window whose swap chain can't take a frame yet is skipped (timings()[w].presented false):
    // present it again next refresh. Never blocks for more than a few milliseconds.
    virtual void present(const std::vector<TileDraw>& tiles, const std::vector<bool>& flash, const std::vector<bool>& which) = 0;
    // Per window, after present(): what reached the screen when.
    virtual std::vector<PresentTiming> timings() = 0;
    // Blocks until the next display refresh, without taking a frame from any swap chain. Also
    // handles window messages.
    virtual void waitForRefresh() = 0;
    virtual double refreshHz() const = 0;
    // For tests: with the probe on, probe() gives the colour (0xRRGGBB) at the centre of each
    // window as last drawn, read back from what was presented.
    virtual void setProbe(bool) {}
    virtual std::vector<uint32_t> probe() { return {}; }
};

// Local wall-clock time with milliseconds ("2026-11-05 23:08:42.979"), for log lines that are
// matched against other logs on the PC (Windows events, the dashboard).
std::string localTime(std::chrono::system_clock::time_point t = std::chrono::system_clock::now());

// Headless: keeps refresh timing (60 Hz) but draws nothing. Used by --self-test and on macOS.
// `extraLatency`: per window, refreshes later it "reaches the screen" (simulates a display that
// Windows composites, for testing sync compensation). `msPerWindow`: time each window's present
// takes (simulates a slow GPU: a refresh with every window presented runs over).
std::unique_ptr<Presenter> createHeadlessPresenter(double hz = 60.0, std::vector<int> extraLatency = {}, double msPerWindow = 0);
// Direct3D 11 on Windows; nullptr elsewhere.
std::unique_ptr<Presenter> createD3D11Presenter(bool softwareGpu, std::string& error);

}  // namespace mosaic::rx
