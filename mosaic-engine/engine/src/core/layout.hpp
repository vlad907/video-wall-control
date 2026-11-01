// Layout model: virtual canvas, sources placed on it, and outputs that map a
// region of the canvas (or of one source) to an independent video stream.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mosaic {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

enum class ScaleMode { Stretch, Fit, Fill };
enum class SourceType { TestPattern, Ndi, Solid };
// Wire format of an output. UYVY is NDI's native 4:2:2 format (half the bytes of BGRA).
enum class PixelFormat { Uyvy, Bgra };

struct SourceConfig {
    std::string id;
    std::string name;
    SourceType type = SourceType::TestPattern;
    std::string ndiName;                      // for SourceType::Ndi
    Rect rect;                                // placement on the canvas, in canvas pixels
    Rect crop;                                // region of the source drawn, in source pixels (w/h 0 = all of it)
    float opacity = 1.f;                      // 0..1, blended over the sources below it
    std::array<float, 4> color{0.f, 0.f, 0.f, 1.f};  // for SourceType::Solid
    bool animated = true;                     // test pattern: false freezes the moving bar and frame counter
    bool enabled = true;
};

struct OutputConfig {
    std::string id;
    std::string name;
    std::string ndiName;                      // published NDI stream name
    int width = 1920, height = 1080;          // output resolution
    Rect crop;                                // region of the sampled image, in its pixels
    int rotation = 0;                         // 0, 90, 180, 270 (clockwise): how the panel is mounted
    float angle = 0.f;                        // free tilt of the crop on the canvas, degrees clockwise about its centre
    ScaleMode scaleMode = ScaleMode::Stretch;
    std::string source = "canvas";            // "canvas" or a source id (crop is then in source pixels)
    PixelFormat pixelFormat = PixelFormat::Uyvy;
    std::string group;                        // id of the wall group that generated it (empty = standalone)
    bool enabled = true;
};

// A wall group: a grid of identical panels with gaps for their bezels (1 x 1 for a
// single display). Generates one output per panel, row-major, with ids <id>-1..<id>-N
// (out-1..out-N for the group "wall", as layouts made before groups had); a regenerated
// group keeps its outputs' NDI stream names. A layout can hold several groups.
struct WallConfig {
    std::string id = "wall";
    std::string name;
    int cols = 2, rows = 2;
    int outputWidth = 1920, outputHeight = 1080;  // panel resolution as sent
    int rotation = 0;                             // panels mounted rotated (90 = portrait)
    int gapX = 0, gapY = 0;                       // bezel gap between panels, in canvas pixels
    int x = 0, y = 0;                             // top-left of the wall on the canvas
    PixelFormat pixelFormat = PixelFormat::Uyvy;
    bool fitCanvas = true;                        // resize the canvas to the wall
};

// A saved arrangement of sources that can be recalled without touching outputs.
struct Scene {
    std::string id;
    std::string name;
    std::vector<SourceConfig> sources;
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
    std::vector<WallConfig> walls;
    std::vector<Scene> scenes;

    const SourceConfig* findSource(const std::string& sid) const;
    const OutputConfig* findOutput(const std::string& oid) const;
    OutputConfig* findOutput(const std::string& oid);
    const WallConfig* findWall(const std::string& wid) const;
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
void to_json(nlohmann::json& j, const WallConfig& w);
void from_json(const nlohmann::json& j, WallConfig& w);
void to_json(nlohmann::json& j, const Scene& s);
void from_json(const nlohmann::json& j, Scene& s);
void to_json(nlohmann::json& j, const Layout& l);
void from_json(const nlohmann::json& j, Layout& l);

// Merge a partial JSON object (PATCH semantics) into an output.
void patchOutput(OutputConfig& o, const nlohmann::json& patch);

// Creates or regenerates the wall group w.id: replaces its outputs with a cols x rows
// grid (see WallConfig) and stores it in the layout. Other outputs are left alone.
void applyWall(Layout& l, WallConfig w);
// Moves a wall group to (x, y), shifting its outputs with it (their own tweaks are kept).
bool moveWall(Layout& l, const std::string& id, int x, int y);
// Forgets a wall group. Its outputs stay as standalone outputs, or are removed.
bool removeWall(Layout& l, const std::string& id, bool removeOutputs);
// Size of a wall group on the canvas, bezel gaps included.
Rect wallBounds(const WallConfig& w);

// Saves the current sources as a scene (replacing one with the same id); returns its id.
std::string saveScene(Layout& l, const std::string& name);
// Replaces the sources with the scene's. Returns false if there is no such scene.
bool recallScene(Layout& l, const std::string& id);

const char* toString(ScaleMode m);
const char* toString(SourceType t);
const char* toString(PixelFormat f);

}  // namespace mosaic
