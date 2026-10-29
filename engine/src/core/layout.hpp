// Layout model: virtual canvas, sources placed on it, and outputs that map a
// region of the canvas (or of one source) to an independent video stream.
#pragma once

#include <array>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mosaic {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

enum class ScaleMode { Stretch, Fit, Fill };
enum class SourceType { TestPattern, Ndi, Solid };

struct SourceConfig {
    std::string id;
    std::string name;
    SourceType type = SourceType::TestPattern;
    std::string ndiName;                      // for SourceType::Ndi
    Rect rect;                                // placement on the canvas, in canvas pixels
    std::array<float, 4> color{0.f, 0.f, 0.f, 1.f};  // for SourceType::Solid
    bool enabled = true;
};

struct OutputConfig {
    std::string id;
    std::string name;
    std::string ndiName;                      // published NDI stream name
    int width = 1920, height = 1080;          // output resolution
    Rect crop;                                // region of the sampled image, in its pixels
    int rotation = 0;                         // 0, 90, 180, 270 (clockwise)
    ScaleMode scaleMode = ScaleMode::Stretch;
    std::string source = "canvas";            // "canvas" or a source id (crop is then in source pixels)
    bool enabled = true;
};

struct CanvasConfig {
    int width = 3840, height = 2160;
    std::array<float, 4> background{0.f, 0.f, 0.f, 1.f};
};

struct Layout {
    std::string id;
    std::string name;
    double frameRate = 60.0;
    CanvasConfig canvas;
    std::vector<SourceConfig> sources;
    std::vector<OutputConfig> outputs;

    const SourceConfig* findSource(const std::string& sid) const;
    const OutputConfig* findOutput(const std::string& oid) const;
    OutputConfig* findOutput(const std::string& oid);
};

constexpr int kMaxDimension = 16384;

// Clamps values into valid ranges and fills missing ids/names.
// Returns a list of human-readable problems that could not be fixed silently.
std::vector<std::string> normalize(Layout& layout);

std::string sanitizeId(const std::string& raw);
Layout defaultLayout();

void to_json(nlohmann::json& j, const Rect& r);
void from_json(const nlohmann::json& j, Rect& r);
void to_json(nlohmann::json& j, const SourceConfig& s);
void from_json(const nlohmann::json& j, SourceConfig& s);
void to_json(nlohmann::json& j, const OutputConfig& o);
void from_json(const nlohmann::json& j, OutputConfig& o);
void to_json(nlohmann::json& j, const CanvasConfig& c);
void from_json(const nlohmann::json& j, CanvasConfig& c);
void to_json(nlohmann::json& j, const Layout& l);
void from_json(const nlohmann::json& j, Layout& l);

// Merge a partial JSON object (PATCH semantics) into an output.
void patchOutput(OutputConfig& o, const nlohmann::json& patch);

const char* toString(ScaleMode m);
const char* toString(SourceType t);

}  // namespace mosaic
