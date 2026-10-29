#include "util/image.hpp"

#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace mosaic {

static uint8_t clamp8(double v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }

void decodeUyvyToBgra(const uint8_t* src, int width, int height, int stride, std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * stride;
        uint8_t* d = out.data() + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x + 1 < width; x += 2, s += 4) {
            double cb = (s[0] - 128.0) / 224.0, cr = (s[2] - 128.0) / 224.0;
            for (int k = 0; k < 2; ++k, d += 4) {
                double yy = (s[1 + 2 * k] - 16.0) / 219.0;
                d[2] = clamp8(255.0 * (yy + 1.5748 * cr));
                d[1] = clamp8(255.0 * (yy - 0.1873 * cb - 0.4681 * cr));
                d[0] = clamp8(255.0 * (yy + 1.8556 * cb));
                d[3] = 255;
            }
        }
    }
}

std::string encodeJpegBGRA(const uint8_t* bgra, int width, int height, int stride, int quality) {
    if (!bgra || width <= 0 || height <= 0) return {};
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = bgra + static_cast<size_t>(y) * stride;
        uint8_t* d = rgb.data() + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x, s += 4, d += 3) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
        }
    }
    std::string out;
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<char*>(data), size); },
        &out, width, height, 3, rgb.data(), quality);
    return out;
}

}  // namespace mosaic
