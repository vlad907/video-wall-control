// Capture devices (PCIe / USB capture cards, webcams): what this PC has, and frames from one as
// BGRA, through the same interface as NDI inputs. A device can be opened by one program at a
// time, so the engine opens each once and shares its frames with every canvas that shows it.
//
// Windows: Media Foundation (capture cards appear as video capture devices; their driver's
// formats are converted to BGRA by Windows' video processor). Elsewhere: only the built-in test
// device, which works everywhere (moving colour bars, for trying the whole path without a card).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "io/video_io.hpp"

namespace mosaic {

struct CaptureDevice {
    std::string name;  // what the device calls itself (Device Manager's name); how a source names it
    std::string kind;  // "device" or "test"
};

constexpr const char* kTestCaptureName = "Mosaic test capture";

// The capture devices on this PC, the test device last. `error` set if listing failed.
std::vector<CaptureDevice> listCaptureDevices(std::string& error);

// Frames from a device (shared: opening one that's open already just adds a viewer). Null, with
// `error` set, if there's no such device or it can't be opened (another program has it).
std::unique_ptr<NdiReceiver> openCapture(const std::string& name, std::string& error);

// Status of the open devices: name, size, frame rate, viewers, error (for the API).
struct CaptureStatus {
    std::string name, error;
    int width = 0, height = 0, viewers = 0;
    double fps = 0;
};
std::vector<CaptureStatus> captureStatus();

// ---- platform backend (capture_mf.cpp on Windows, capture_none.cpp elsewhere) ----
// Hands every frame (BGRA; `stride` from one row to the next, negative for a bottom-up buffer, the
// pointer always at the top row) to `onFrame` from its own thread until destroyed.
class CaptureStream {
public:
    virtual ~CaptureStream() = default;
    virtual std::string error() const = 0;  // set if the device stopped
};
using CaptureFrameFn = std::function<void(const uint8_t* bgra, int width, int height, long stride)>;
std::vector<CaptureDevice> platformCaptureDevices(std::string& error);
std::unique_ptr<CaptureStream> openPlatformCapture(const std::string& name, CaptureFrameFn onFrame, std::string& error);

}  // namespace mosaic
