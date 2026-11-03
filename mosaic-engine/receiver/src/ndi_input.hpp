// NDI input for the receiver: one receive thread per stream, filling a StreamQueue with
// frames that keep their engine frame id (NDI metadata) and timecode.
#pragma once

#include <memory>
#include <string>

#include "frames.hpp"

namespace mosaic::rx {

// nullptr (with a reason) when built without the NDI SDK or the NDI runtime is missing.
std::unique_ptr<FrameInput> createNdiInput(const std::string& sourceName, std::string& whyNot);

// Parses `<mosaic frame_id="N"/>` out of NDI frame metadata; 0 if absent.
uint64_t parseFrameId(const char* metadata);

}  // namespace mosaic::rx
