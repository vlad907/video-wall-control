#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mosaic {

// Encodes a BGRA image as JPEG (alpha dropped). Returns empty on failure.
std::string encodeJpegBGRA(const uint8_t* bgra, int width, int height, int stride, int quality);

// BT.709 limited-range UYVY 4:2:2 -> BGRA (for snapshots and tests; not on the hot path).
void decodeUyvyToBgra(const uint8_t* uyvy, int width, int height, int stride, std::vector<uint8_t>& bgra);

}  // namespace mosaic
