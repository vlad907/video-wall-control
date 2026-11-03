// Built without the NDI SDK: the receiver can only show synthetic streams.
#include "ndi_input.hpp"

#include <cstdlib>
#include <cstring>

namespace mosaic::rx {

std::unique_ptr<FrameInput> createNdiInput(const std::string&, std::string& whyNot) {
    whyNot = "built without the NDI SDK";
    return nullptr;
}

uint64_t parseFrameId(const char* metadata) {
    const char* p = metadata ? std::strstr(metadata, "frame_id=\"") : nullptr;
    return p ? std::strtoull(p + 10, nullptr, 10) : 0;
}

}  // namespace mosaic::rx
