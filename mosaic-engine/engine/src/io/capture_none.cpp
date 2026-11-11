// Capture devices on platforms without a backend yet (macOS): none but the built-in test device.
#include "io/capture.hpp"

namespace mosaic {

std::vector<CaptureDevice> platformCaptureDevices(std::string&) { return {}; }

std::unique_ptr<CaptureStream> openPlatformCapture(const std::string& name, CaptureFrameFn, std::string& error) {
    error = "no capture device '" + name + "' (capture cards work on Windows)";
    return nullptr;
}

}  // namespace mosaic
