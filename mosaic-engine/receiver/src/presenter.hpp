// Puts frames on screen: one full-screen window per monitor, each showing one or more tiles
// (a tile is a stream drawn into a rectangle of the window). All windows are presented in
// the same refresh. Windows: Direct3D 11 flip-model swap chains. Elsewhere / tests: headless.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "frames.hpp"

namespace mosaic::rx {

struct WindowSpec {
    int x = 0, y = 0, w = 1920, h = 1080;  // desktop rectangle (a monitor, or a spanned group of them)
};

struct TileDraw {
    int window = 0;
    int x = 0, y = 0, w = 0, h = 0;  // inside the window
    int stream = -1;                 // index of the stream texture; -1 = nothing (black)
};

class Presenter {
public:
    virtual ~Presenter() = default;
    virtual std::string name() const = 0;
    // Creates the windows (replacing any before). False if it can't.
    virtual bool configure(const std::vector<WindowSpec>& windows, std::string& error) = 0;
    // Uploads a frame as stream `stream`'s picture (only when the picture changed).
    virtual void upload(int stream, const RxFrame& frame) = 0;
    // Draws every window and presents them all.
    virtual void present(const std::vector<TileDraw>& tiles) = 0;
    // Blocks until the next display refresh. Also handles window messages.
    virtual void waitForRefresh() = 0;
    virtual double refreshHz() const = 0;
};

// Headless: keeps refresh timing (60 Hz) but draws nothing. Used by --self-test and on macOS.
std::unique_ptr<Presenter> createHeadlessPresenter(double hz = 60.0);
// Direct3D 11 on Windows; nullptr elsewhere.
std::unique_ptr<Presenter> createD3D11Presenter(bool softwareGpu, std::string& error);

}  // namespace mosaic::rx
