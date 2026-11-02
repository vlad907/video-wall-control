#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mosaic {

// Encodes a BGRA image as JPEG (alpha dropped). Returns empty on failure.
std::string encodeJpegBGRA(const uint8_t* bgra, int width, int height, int stride, int quality);

// Reads a PNG / JPEG / BMP / GIF file into tightly packed BGRA. False (with a reason) on failure.
bool loadImageBGRA(const std::string& path, std::vector<uint8_t>& bgra, int& width, int& height, std::string& error);

// BT.709 limited-range UYVY 4:2:2 -> BGRA (for snapshots and tests; not on the hot path).
void decodeUyvyToBgra(const uint8_t* uyvy, int width, int height, int stride, std::vector<uint8_t>& bgra);

}  // namespace mosaic
