#pragma once

#include <cstdint>
#include <string>

namespace mosaic {

// Encodes a BGRA image as JPEG (alpha dropped). Returns empty on failure.
std::string encodeJpegBGRA(const uint8_t* bgra, int width, int height, int stride, int quality);

}  // namespace mosaic
