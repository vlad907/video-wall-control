// File names in layouts and the API are UTF-8. std::filesystem::path(std::string) reads the
// system code page on Windows, which mangles (or throws on) names like "Nature ｜ 8K.mp4".
#pragma once

#include <filesystem>
#include <string>

namespace mosaic {

inline std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

inline std::string utf8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

}  // namespace mosaic
