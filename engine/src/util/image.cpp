#include "util/image.hpp"

#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace mosaic {

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
