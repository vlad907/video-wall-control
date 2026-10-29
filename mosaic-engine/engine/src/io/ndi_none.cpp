// Built when the NDI SDK was not found at configure time.
#include "io/video_io.hpp"

namespace mosaic {

std::unique_ptr<NdiSystem> createNdiSystem(std::string& whyNot) {
    whyNot = "built without the NDI SDK (re-run CMake with -DNDI_SDK_DIR=<sdk folder>; see docs/NDI.md)";
    return nullptr;
}

}  // namespace mosaic
