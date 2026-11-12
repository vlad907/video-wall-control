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
enum class SourceType { TestPattern, Ndi, Solid, Image, Video, Capture };
// Wire format of an output. UYVY is NDI's native 4:2:2 format (half the bytes of BGRA).
enum class PixelFormat { Uyvy, Bgra };

struct SourceConfig {
    std::string id;
    std::string name;
    SourceType type = SourceType::TestPattern;
    std::string ndiName;                      // for SourceType::Ndi
    std::string image;                        // for SourceType::Image: a file in the media folder (data/media)
    std::string video;                        // for SourceType::Video: a file in the media folder
    std::string device;                       // for SourceType::Capture: the capture device's name (GET /api/capture)
    bool loop = true;                         // video: start again at the end (else hold the last frame)
    bool playing = true;                      // video: false = paused
    Rect rect;                                // placement on the canvas, in canvas pixels
    Rect crop;                                // region of the source drawn, in source pixels (w/h 0 = all of it)
    float opacity = 1.f;                      // 0..1, blended over the sources below it
    std::array<float, 4> color{0.f, 0.f, 0.f, 1.f};  // for SourceType::Solid
    bool animated = true;                     // test pattern: false freezes the moving bar and frame counter
    bool enabled = true;
    std::string group;                        // wall group whose content this is (set by its presets), or empty
};

// What one wall group shows: its sources, placed relative to the group's top-left corner,
// so the preset still fits after the group is moved.
struct GroupPreset {
    std::string id;
    std::string name;
    std::vector<SourceConfig> sources;
};

// Colour correction of one output, to match displays to each other. Neutral = no change (and no cost).
struct ColorAdjust {
    float brightness = 0.f;  // -0.5..0.5, added
    float contrast = 1.f;    // 0..3, about mid grey
    float saturation = 1.f;  // 0 (grey)..3
    float gamma = 1.f;       // 0.2..5; above 1 lifts the mid tones
    float red = 1.f, green = 1.f, blue = 1.f;  // 0..2 gain per channel (white balance)
    bool neutral() const {
        return brightness == 0.f && contrast == 1.f && saturation == 1.f && gamma == 1.f && red == 1.f && green == 1.f && blue == 1.f;
    }
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
    // A monitor on this canvas's GPU ("\\.\DISPLAY3"): the output goes there full screen (DisplayPort /
    // HDMI to a video wall or its controller) instead of out as an NDI stream. Drawn at the
    // monitor's resolution, timed to its refresh.
    std::string display;
    // Display outputs: the mode to put the monitor in (0 x 0 = leave it as Windows has it), and how
    // long to hold the picture back (to line up with walls fed over NDI, which show it later).
    int modeWidth = 0, modeHeight = 0, modeHz = 0;
    int delayMs = 0;  // 0..1000
    ColorAdjust color;
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
    std::vector<GroupPreset> presets;             // saved content for this group
    std::string fallbackImage;                    // media file shown where one of its NDI streams has no signal
    std::string activePreset;                     // last preset recalled or saved ("" after show / clear)
};

// A saved arrangement of sources that can be recalled without touching outputs.
struct Scene {
    std::string id;
    std::string name;
    std::vector<SourceConfig> sources;
};

// A receiving PC (it runs the video-wall dashboard and NDI Studio Monitor). Each display
// slot puts one output on one of its screens; the PC's dashboard fetches this on check-in
// and sets itself up to match.
struct EndpointDisplay {
    std::string screen;  // Windows display device ("\\.\DISPLAY2"); empty = the next free screen
    std::string output;  // output id
    // The screen shows one cell of the output's picture cut into partCols x partRows (part 1..N,
    // row by row; 0 = the whole picture). A 2 x 2 of screens on one PC then needs one stream
    // instead of four, and its screens can't drift apart: they show pieces of the same frame.
    // Mosaic Receiver only.
    int partCols = 1, partRows = 1, part = 0;
    // Bezel correction for those cells: the gap between neighbouring pictures (both bezels), in
    // pixels of the stream. That much of the picture is left out between cells, as if it were
    // hidden behind the bezels, so a line runs straight from one screen onto the next.
    int partGapX = 0, partGapY = 0;
};

// A sound output of the media server: one NDI source's audio, played on one of the PC's sound
// devices (a channel pair of it), delayed to match the wall.
struct AudioOutputConfig {
    std::string id;
    std::string name;            // "Bar speakers"
    std::string device;          // Windows sound device id ("" = the default device)
    std::string deviceName;      // its name when chosen (shown if it's unplugged)
    std::string source;          // NDI source name whose audio it plays
    int channelPair = 0;         // 0 = channels 1-2, 1 = 3-4, ...
    double volumeDb = 0;         // -60..+12
    bool mute = false;
    bool matchWall = true;       // delay = the wall's delay, plus delayMs (a trim, may be negative)
    double delayMs = 0;          // with matchWall off: the whole delay
    bool enabled = true;
};

struct Endpoint {
    std::string id;
    std::string name;
    std::string host;  // the PC's name, matched without case and domain
    std::string player = "studio_monitor";  // what shows the streams there: "studio_monitor" or "mosaic" (Mosaic Receiver)
    bool flashTest = false;                 // Mosaic Receiver: flash every display white on frames divisible by 30
    std::vector<EndpointDisplay> displays;
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
    std::vector<Endpoint> endpoints;
    std::vector<AudioOutputConfig> audioOutputs;
    std::string fallbackImage;  // default for NDI streams with no signal (a group's own one wins)
    // Mosaic Receivers show each frame this long after it was made, so network stalls shorter
    // than this never reach the screens. 0 = lowest delay (live feeds); ~300 for playback.
    int receiverBufferMs = 0;
    // New outputs' NDI streams are named <ndiPrefix>_01, _02, ... Each canvas has its own, so
    // two canvases running at once don't send streams with the same name.
    std::string ndiPrefix = "Mosaic_Output";
    // The GPU this canvas renders on: its number on the media server (GET /api/gpus), -1 = the
    // default one. gpuName is its name when chosen: if the numbering changes (a card added or
    // moved), the canvas finds its card by name.
    int gpu = -1;
    std::string gpuName;

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
// An id made from a display name: sanitized, with runs of "-" collapsed ("Camera + slides" -> "camera-slides").
std::string idFromName(const std::string& name);
// A PC name as endpoints match it: upper case, without the domain.
std::string hostKey(const std::string& host);
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
void to_json(nlohmann::json& j, const GroupPreset& p);
void from_json(const nlohmann::json& j, GroupPreset& p);
void to_json(nlohmann::json& j, const EndpointDisplay& d);
void from_json(const nlohmann::json& j, EndpointDisplay& d);
void to_json(nlohmann::json& j, const AudioOutputConfig& a);
void from_json(const nlohmann::json& j, AudioOutputConfig& a);
void to_json(nlohmann::json& j, const Endpoint& e);
void from_json(const nlohmann::json& j, Endpoint& e);
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

// Group content. The group's sources are those with source.group == group id; they are
// drawn over the layout's other sources.
// Saves the group's current sources as a preset (replacing one with the same id); returns its id, "" if no such group.
std::string saveGroupPreset(Layout& l, const std::string& groupId, const std::string& name);
// Replaces the group's sources with the preset's. False if there is no such group or preset.
bool recallGroupPreset(Layout& l, const std::string& groupId, const std::string& presetId);
bool deleteGroupPreset(Layout& l, const std::string& groupId, const std::string& presetId);
// Replaces the group's sources with one NDI stream filling the group ("" = clear the group).
bool showOnGroup(Layout& l, const std::string& groupId, const std::string& ndiName);

// Saves the current sources as a scene (replacing one with the same id); returns its id.
std::string saveScene(Layout& l, const std::string& name);
// Replaces the sources with the scene's. Returns false if there is no such scene.
bool recallScene(Layout& l, const std::string& id);

const char* toString(ScaleMode m);
const char* toString(SourceType t);
const char* toString(PixelFormat f);

}  // namespace mosaic
