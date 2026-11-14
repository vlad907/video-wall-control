#include "core/layout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

using nlohmann::json;

namespace mosaic {

const SourceConfig* Layout::findSource(const std::string& sid) const {
    for (auto& s : sources)
        if (s.id == sid) return &s;
    return nullptr;
}

const WallConfig* Layout::findWall(const std::string& wid) const {
    for (auto& w : walls)
        if (w.id == wid) return &w;
    return nullptr;
}

const OutputConfig* Layout::findOutput(const std::string& oid) const {
    for (auto& o : outputs)
        if (o.id == oid) return &o;
    return nullptr;
}

OutputConfig* Layout::findOutput(const std::string& oid) {
    for (auto& o : outputs)
        if (o.id == oid) return &o;
    return nullptr;
}

const char* toString(ScaleMode m) {
    switch (m) {
        case ScaleMode::Fit: return "fit";
        case ScaleMode::Fill: return "fill";
        default: return "stretch";
    }
}

const char* toString(SourceType t) {
    switch (t) {
        case SourceType::Ndi: return "ndi";
        case SourceType::Solid: return "solid";
        case SourceType::Image: return "image";
        case SourceType::Video: return "video";
        case SourceType::Capture: return "capture";
        default: return "test_pattern";
    }
}

const char* toString(PixelFormat f) { return f == PixelFormat::Bgra ? "bgra" : "uyvy"; }

static ScaleMode scaleModeFrom(const std::string& s) {
    if (s == "fit") return ScaleMode::Fit;
    if (s == "fill") return ScaleMode::Fill;
    return ScaleMode::Stretch;
}

static SourceType sourceTypeFrom(const std::string& s) {
    if (s == "ndi") return SourceType::Ndi;
    if (s == "solid") return SourceType::Solid;
    if (s == "image") return SourceType::Image;
    if (s == "video") return SourceType::Video;
    if (s == "capture") return SourceType::Capture;
    return SourceType::TestPattern;
}

static PixelFormat pixelFormatFrom(const std::string& s) { return s == "bgra" ? PixelFormat::Bgra : PixelFormat::Uyvy; }

std::string idFromName(const std::string& name) {
    std::string out;
    for (char c : sanitizeId(name))
        if (c != '-' || (!out.empty() && out.back() != '-')) out += c;
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::string sanitizeId(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_') out += static_cast<char>(std::tolower(u));
        else if (c == ' ' || c == '.') out += '-';
    }
    if (out.size() > 64) out.resize(64);
    return out;
}

void to_json(json& j, const Rect& r) { j = json{{"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}}; }

void from_json(const json& j, Rect& r) {
    r.x = j.value("x", r.x);
    r.y = j.value("y", r.y);
    r.w = j.value("w", r.w);
    r.h = j.value("h", r.h);
}

void to_json(json& j, const SourceConfig& s) {
    j = json{{"id", s.id},       {"name", s.name},       {"type", toString(s.type)}, {"rect", s.rect},
             {"crop", s.crop},   {"opacity", s.opacity}, {"enabled", s.enabled}};
    if (s.type == SourceType::Ndi) j["ndiName"] = s.ndiName;
    if (s.type == SourceType::Solid) j["color"] = s.color;
    if (s.type == SourceType::TestPattern) j["animated"] = s.animated;
    if (s.type == SourceType::Image) j["image"] = s.image;
    if (s.type == SourceType::Capture) j["device"] = s.device;
    if (s.type == SourceType::Video) {
        j["video"] = s.video;
        if (!s.playlist.empty()) j["playlist"] = s.playlist;
        j["loop"] = s.loop;
        j["playing"] = s.playing;
    }
    if (!s.group.empty()) j["group"] = s.group;
    if (s.slot > 0) j["slot"] = s.slot;
}

void from_json(const json& j, SourceConfig& s) {
    s.id = j.value("id", s.id);
    s.name = j.value("name", s.name);
    s.type = sourceTypeFrom(j.value("type", std::string(toString(s.type))));
    s.ndiName = j.value("ndiName", s.ndiName);
    s.image = j.value("image", s.image);
    s.video = j.value("video", s.video);
    s.playlist = j.value("playlist", s.playlist);
    s.slot = j.value("slot", s.slot);
    s.device = j.value("device", s.device);
    s.loop = j.value("loop", s.loop);
    s.playing = j.value("playing", s.playing);
    if (j.contains("rect")) s.rect = j.at("rect").get<Rect>();
    if (j.contains("crop")) from_json(j.at("crop"), s.crop);
    s.opacity = j.value("opacity", s.opacity);
    if (j.contains("color")) s.color = j.at("color").get<std::array<float, 4>>();
    s.enabled = j.value("enabled", s.enabled);
    s.animated = j.value("animated", s.animated);
    s.group = j.value("group", s.group);
}

void to_json(json& j, const GroupPreset& p) {
    j = json{{"id", p.id}, {"name", p.name}, {"sources", p.sources}};
    if (!p.arrangement.empty()) j["arrangement"] = p.arrangement;
}

void from_json(const json& j, GroupPreset& p) {
    p.id = j.value("id", p.id);
    p.name = j.value("name", p.name);
    p.arrangement = j.value("arrangement", p.arrangement);
    if (j.contains("sources")) p.sources = j.at("sources").get<std::vector<SourceConfig>>();
}

void to_json(json& j, const OutputConfig& o) {
    j = json{{"id", o.id},           {"name", o.name},         {"ndiName", o.ndiName},
             {"width", o.width},     {"height", o.height},     {"crop", o.crop},
             {"rotation", o.rotation}, {"angle", o.angle}, {"scaleMode", toString(o.scaleMode)},
             {"source", o.source},   {"pixelFormat", toString(o.pixelFormat)}, {"enabled", o.enabled}};
    if (!o.group.empty()) j["group"] = o.group;
    if (!o.display.empty()) j["display"] = o.display;
    if (o.modeWidth > 0) j["displayMode"] = {{"width", o.modeWidth}, {"height", o.modeHeight}, {"refreshHz", o.modeHz}};
    if (o.delayMs > 0) j["delayMs"] = o.delayMs;
    if (!o.color.neutral())
        j["color"] = {{"brightness", o.color.brightness}, {"contrast", o.color.contrast}, {"saturation", o.color.saturation}, {"gamma", o.color.gamma},
                      {"red", o.color.red},             {"green", o.color.green},       {"blue", o.color.blue}};
}

void from_json(const json& j, OutputConfig& o) {
    o.id = j.value("id", o.id);
    patchOutput(o, j);
}

void patchOutput(OutputConfig& o, const json& p) {
    o.name = p.value("name", o.name);
    o.ndiName = p.value("ndiName", o.ndiName);
    o.width = p.value("width", o.width);
    o.height = p.value("height", o.height);
    if (p.contains("crop")) {
        Rect r = o.crop;
        from_json(p.at("crop"), r);  // allows partial crop updates
        o.crop = r;
    }
    o.rotation = p.value("rotation", o.rotation);
    o.angle = p.value("angle", o.angle);
    if (p.contains("scaleMode")) o.scaleMode = scaleModeFrom(p.at("scaleMode").get<std::string>());
    o.source = p.value("source", o.source);
    if (p.contains("pixelFormat")) o.pixelFormat = pixelFormatFrom(p.at("pixelFormat").get<std::string>());
    o.group = p.value("group", o.group);
    o.display = p.value("display", o.display);
    if (p.contains("displayMode")) {
        const auto& m = p.at("displayMode");
        if (m.is_object() && m.value("width", 0) > 0 && m.value("height", 0) > 0) {
            o.modeWidth = std::clamp(m.value("width", 0), 16, 16384);
            o.modeHeight = std::clamp(m.value("height", 0), 16, 16384);
            o.modeHz = std::clamp(m.value("refreshHz", 60), 1, 1000);
        } else {
            o.modeWidth = o.modeHeight = o.modeHz = 0;  // null: as Windows has it
        }
    }
    if (p.contains("delayMs")) o.delayMs = std::clamp(p.at("delayMs").get<int>(), 0, 1000);
    if (p.contains("color") && p.at("color").is_object()) {
        const auto& c = p.at("color");  // partial updates allowed
        o.color.brightness = std::clamp(c.value("brightness", o.color.brightness), -0.5f, 0.5f);
        o.color.contrast = std::clamp(c.value("contrast", o.color.contrast), 0.f, 3.f);
        o.color.saturation = std::clamp(c.value("saturation", o.color.saturation), 0.f, 3.f);
        o.color.gamma = std::clamp(c.value("gamma", o.color.gamma), 0.2f, 5.f);
        o.color.red = std::clamp(c.value("red", o.color.red), 0.f, 2.f);
        o.color.green = std::clamp(c.value("green", o.color.green), 0.f, 2.f);
        o.color.blue = std::clamp(c.value("blue", o.color.blue), 0.f, 2.f);
    } else if (p.contains("color") && p.at("color").is_null()) {
        o.color = {};  // reset
    }
    o.enabled = p.value("enabled", o.enabled);
}

void to_json(json& j, const WallConfig& w) {
    j = json{{"id", w.id},           {"name", w.name},         {"cols", w.cols},       {"rows", w.rows},         {"outputWidth", w.outputWidth},
             {"outputHeight", w.outputHeight},                 {"rotation", w.rotation},
             {"gapX", w.gapX},       {"gapY", w.gapY},         {"x", w.x},
             {"y", w.y},             {"pixelFormat", toString(w.pixelFormat)}, {"fitCanvas", w.fitCanvas},
             {"presets", w.presets}, {"activePreset", w.activePreset}, {"fallbackImage", w.fallbackImage}};
    if (!w.arrangement.empty()) j["arrangement"] = w.arrangement;
    if (!w.parked.empty()) j["parked"] = w.parked, j["parkedArrangement"] = w.parkedArrangement;
}

void from_json(const json& j, WallConfig& w) {
    w.id = j.value("id", w.id);
    w.name = j.value("name", w.name);
    w.cols = j.value("cols", w.cols);
    w.rows = j.value("rows", w.rows);
    w.outputWidth = j.value("outputWidth", w.outputWidth);
    w.outputHeight = j.value("outputHeight", w.outputHeight);
    w.rotation = j.value("rotation", w.rotation);
    w.gapX = j.value("gapX", w.gapX);
    w.gapY = j.value("gapY", w.gapY);
    w.x = j.value("x", w.x);
    w.y = j.value("y", w.y);
    if (j.contains("pixelFormat")) w.pixelFormat = pixelFormatFrom(j.at("pixelFormat").get<std::string>());
    w.fitCanvas = j.value("fitCanvas", w.fitCanvas);
    if (j.contains("presets")) w.presets = j.at("presets").get<std::vector<GroupPreset>>();
    w.activePreset = j.value("activePreset", w.activePreset);
    w.fallbackImage = j.value("fallbackImage", w.fallbackImage);
    w.arrangement = j.value("arrangement", w.arrangement);
    if (j.contains("parked")) w.parked = j.at("parked").get<std::vector<SourceConfig>>();
    w.parkedArrangement = j.value("parkedArrangement", w.parkedArrangement);
}

void to_json(json& j, const EndpointDisplay& d) {
    j = json{{"screen", d.screen}, {"output", d.output}};
    if (d.part > 0) {
        j["part"] = {{"cols", d.partCols}, {"rows", d.partRows}, {"index", d.part}};
        if (d.partGapX) j["part"]["gapX"] = d.partGapX;
        if (d.partGapY) j["part"]["gapY"] = d.partGapY;
    }
}

void from_json(const json& j, EndpointDisplay& d) {
    d.screen = j.value("screen", d.screen);
    d.output = j.value("output", d.output);
    d.partCols = d.partRows = 1;
    d.part = 0;
    d.partGapX = d.partGapY = 0;
    if (j.contains("part") && j.at("part").is_object()) {
        const auto& p = j.at("part");
        d.partCols = std::clamp(p.value("cols", 1), 1, 8);
        d.partRows = std::clamp(p.value("rows", 1), 1, 8);
        d.part = std::clamp(p.value("index", 0), 0, d.partCols * d.partRows);
        if (d.partCols * d.partRows == 1) d.part = 0;
        if (d.part > 0) {
            // Only between cells: a single column has no gap across, a single row none down.
            d.partGapX = d.partCols > 1 ? std::clamp(p.value("gapX", 0), 0, 2000) : 0;
            d.partGapY = d.partRows > 1 ? std::clamp(p.value("gapY", 0), 0, 2000) : 0;
        }
    }
}

void to_json(json& j, const AudioOutputConfig& a) {
    j = json{{"id", a.id},           {"name", a.name},         {"device", a.device},       {"deviceName", a.deviceName},
             {"source", a.source},   {"channelPair", a.channelPair}, {"volumeDb", a.volumeDb}, {"mute", a.mute},
             {"matchWall", a.matchWall}, {"delayMs", a.delayMs}, {"enabled", a.enabled}};
}

void from_json(const json& j, AudioOutputConfig& a) {
    a.id = j.value("id", a.id);
    a.name = j.value("name", a.name);
    a.device = j.value("device", a.device);
    a.deviceName = j.value("deviceName", a.deviceName);
    a.source = j.value("source", a.source);
    a.channelPair = j.value("channelPair", a.channelPair);
    a.volumeDb = j.value("volumeDb", a.volumeDb);
    a.mute = j.value("mute", a.mute);
    a.matchWall = j.value("matchWall", a.matchWall);
    a.delayMs = j.value("delayMs", a.delayMs);
    a.enabled = j.value("enabled", a.enabled);
}

void to_json(json& j, const Endpoint& e) {
    j = json{{"id", e.id}, {"name", e.name}, {"host", e.host}, {"player", e.player}, {"flashTest", e.flashTest}, {"displays", e.displays}};
}

void from_json(const json& j, Endpoint& e) {
    e.id = j.value("id", e.id);
    e.name = j.value("name", e.name);
    e.host = j.value("host", e.host);
    e.player = j.value("player", e.player);
    e.flashTest = j.value("flashTest", e.flashTest);
    if (j.contains("displays")) e.displays = j.at("displays").get<std::vector<EndpointDisplay>>();
}

std::string hostKey(const std::string& host) {
    std::string out;
    for (char c : host) {
        if (c == '.') break;
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

void to_json(json& j, const Scene& s) { j = json{{"id", s.id}, {"name", s.name}, {"sources", s.sources}}; }

void from_json(const json& j, Scene& s) {
    s.id = j.value("id", s.id);
    s.name = j.value("name", s.name);
    if (j.contains("sources")) s.sources = j.at("sources").get<std::vector<SourceConfig>>();
}

void to_json(json& j, const CanvasConfig& c) {
    j = json{{"width", c.width}, {"height", c.height}, {"background", c.background}};
}

void from_json(const json& j, CanvasConfig& c) {
    c.width = j.value("width", c.width);
    c.height = j.value("height", c.height);
    if (j.contains("background")) c.background = j.at("background").get<std::array<float, 4>>();
}

void to_json(json& j, const Layout& l) {
    j = json{{"id", l.id},         {"name", l.name},       {"frameRate", l.frameRate},
             {"canvas", l.canvas}, {"sources", l.sources}, {"outputs", l.outputs}, {"walls", l.walls},
             {"scenes", l.scenes}, {"endpoints", l.endpoints}, {"fallbackImage", l.fallbackImage},
             {"receiverBufferMs", l.receiverBufferMs}, {"audioOutputs", l.audioOutputs}, {"ndiPrefix", l.ndiPrefix},
             {"gpu", l.gpu}, {"gpuName", l.gpuName}};
}

void from_json(const json& j, Layout& l) {
    l.id = j.value("id", l.id);
    l.name = j.value("name", l.name);
    l.frameRate = j.value("frameRate", l.frameRate);
    if (j.contains("canvas")) l.canvas = j.at("canvas").get<CanvasConfig>();
    if (j.contains("sources")) l.sources = j.at("sources").get<std::vector<SourceConfig>>();
    if (j.contains("outputs")) l.outputs = j.at("outputs").get<std::vector<OutputConfig>>();
    if (j.contains("walls")) l.walls = j.at("walls").get<std::vector<WallConfig>>();
    else if (j.contains("wall")) {
        // Layouts saved before wall groups had at most one wall, the group "wall".
        l.walls.clear();
        if (!j.at("wall").is_null()) l.walls.push_back(j.at("wall").get<WallConfig>());
        for (auto& w : l.walls) w.id = "wall";
    }
    if (j.contains("scenes")) l.scenes = j.at("scenes").get<std::vector<Scene>>();
    if (j.contains("endpoints")) l.endpoints = j.at("endpoints").get<std::vector<Endpoint>>();
    l.fallbackImage = j.value("fallbackImage", l.fallbackImage);
    l.receiverBufferMs = j.value("receiverBufferMs", l.receiverBufferMs);
    l.ndiPrefix = j.value("ndiPrefix", l.ndiPrefix);
    l.gpu = j.value("gpu", l.gpu);
    l.gpuName = j.value("gpuName", l.gpuName);
    if (j.contains("audioOutputs")) l.audioOutputs = j.at("audioOutputs").get<std::vector<AudioOutputConfig>>();
}

static int clampDim(int v) { return std::clamp(v, 1, kMaxDimension); }

std::vector<std::string> normalize(Layout& l) {
    std::vector<std::string> problems;
    l.id = sanitizeId(l.id.empty() ? l.name : l.id);
    if (l.id.empty()) l.id = "layout";
    if (l.name.empty()) l.name = l.id;
    l.frameRate = std::clamp(l.frameRate, 1.0, 240.0);
    l.receiverBufferMs = std::clamp(l.receiverBufferMs, 0, 1000);
    l.gpu = std::clamp(l.gpu, -1, 15);
    if (l.gpu < 0) l.gpuName.clear();
    {
        std::string p;
        for (char c : l.ndiPrefix)
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') p += c;
            else if (c == ' ') p += '_';
        l.ndiPrefix = p.empty() ? "Mosaic_Output" : p.substr(0, 40);
    }
    l.canvas.width = clampDim(l.canvas.width);
    l.canvas.height = clampDim(l.canvas.height);

    std::set<std::string> ids;
    int n = 0;
    for (auto& s : l.sources) {
        ++n;
        s.id = sanitizeId(s.id);
        if (s.id.empty() || ids.count(s.id)) s.id = "source-" + std::to_string(n);
        ids.insert(s.id);
        if (s.name.empty()) s.name = s.id;
        if (s.rect.w <= 0 || s.rect.h <= 0) s.rect = {0, 0, l.canvas.width, l.canvas.height};
        s.rect.w = clampDim(s.rect.w);
        s.rect.h = clampDim(s.rect.h);
        s.crop.w = std::clamp(s.crop.w, 0, kMaxDimension);
        s.crop.h = std::clamp(s.crop.h, 0, kMaxDimension);
        if (!(s.opacity >= 0.f)) s.opacity = 0.f;  // also catches NaN
        s.opacity = std::min(s.opacity, 1.f);
        if (s.type == SourceType::Ndi && s.ndiName.empty()) problems.push_back("source '" + s.id + "' has no ndiName");
    }

    ids.clear();
    std::set<std::string> ndiNames;
    n = 0;
    for (auto& o : l.outputs) {
        ++n;
        o.id = sanitizeId(o.id);
        if (o.id.empty() || ids.count(o.id)) o.id = "out-" + std::to_string(n);
        ids.insert(o.id);
        if (o.name.empty()) o.name = "Display " + std::to_string(n);
        if (o.ndiName.empty() || ndiNames.count(o.ndiName)) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s_%02d", l.ndiPrefix.c_str(), n);
            o.ndiName = buf;
        }
        ndiNames.insert(o.ndiName);
        o.width = clampDim(o.width);
        o.height = clampDim(o.height);
        // UYVY packs pixel pairs, so the width must be even.
        if (o.pixelFormat == PixelFormat::Uyvy && o.width % 2) o.width += 1;
        o.rotation = ((o.rotation % 360) + 360) % 360;
        o.rotation = (o.rotation / 90) * 90;
        if (!std::isfinite(o.angle)) o.angle = 0.f;
        o.angle = std::remainder(o.angle, 360.f);  // -180..180
        if (o.crop.w <= 0 || o.crop.h <= 0) o.crop = {0, 0, o.width, o.height};
        o.crop.w = clampDim(o.crop.w);
        o.crop.h = clampDim(o.crop.h);
        if (o.source != "canvas" && !l.findSource(o.source)) {
            problems.push_back("output '" + o.id + "' references unknown source '" + o.source + "'; using canvas");
            o.source = "canvas";
        }
    }

    ids.clear();
    n = 0;
    for (auto& w : l.walls) {
        ++n;
        w.id = w.id.empty() ? idFromName(w.name) : sanitizeId(w.id);
        if (w.id.empty() || ids.count(w.id)) w.id = "wall-" + std::to_string(n);
        ids.insert(w.id);
        if (w.name.empty()) w.name = w.id == "wall" ? "Wall" : w.id;
        w.cols = std::clamp(w.cols, 1, 16);
        w.rows = std::clamp(w.rows, 1, 16);
        w.outputWidth = clampDim(w.outputWidth);
        w.outputHeight = clampDim(w.outputHeight);
        w.rotation = (((w.rotation % 360) + 360) % 360) / 90 * 90;
        w.gapX = std::clamp(w.gapX, 0, kMaxDimension);
        w.gapY = std::clamp(w.gapY, 0, kMaxDimension);
        std::set<std::string> presetIds;
        int k = 0;
        for (auto& p : w.presets) {
            ++k;
            p.id = p.id.empty() ? idFromName(p.name) : sanitizeId(p.id);
            if (p.id.empty() || presetIds.count(p.id)) p.id = "preset-" + std::to_string(k);
            presetIds.insert(p.id);
            if (p.name.empty()) p.name = p.id;
        }
    }

    ids.clear();
    n = 0;
    for (auto& e : l.endpoints) {
        ++n;
        e.id = e.id.empty() ? idFromName(e.name.empty() ? e.host : e.name) : sanitizeId(e.id);
        if (e.id.empty() || ids.count(e.id)) e.id = "endpoint-" + std::to_string(n);
        ids.insert(e.id);
        if (e.name.empty()) e.name = e.host.empty() ? e.id : e.host;
        if (e.player != "mosaic") e.player = "studio_monitor";
    }

    ids.clear();
    n = 0;
    for (auto& a : l.audioOutputs) {
        ++n;
        a.id = sanitizeId(a.id.empty() ? a.name : a.id);
        if (a.id.empty() || ids.count(a.id)) a.id = "audio-" + std::to_string(n);
        ids.insert(a.id);
        if (a.name.empty()) a.name = "Audio " + std::to_string(n);
        a.channelPair = std::clamp(a.channelPair, 0, 31);
        a.volumeDb = std::clamp(a.volumeDb, -60.0, 12.0);
        a.delayMs = std::clamp(a.delayMs, a.matchWall ? -1000.0 : 0.0, 5000.0);
    }

    ids.clear();
    n = 0;
    for (auto& sc : l.scenes) {
        ++n;
        sc.id = sanitizeId(sc.id.empty() ? sc.name : sc.id);
        if (sc.id.empty() || ids.count(sc.id)) sc.id = "scene-" + std::to_string(n);
        ids.insert(sc.id);
        if (sc.name.empty()) sc.name = sc.id;
    }
    return problems;
}

Rect wallBounds(const WallConfig& w) {
    bool sideways = w.rotation == 90 || w.rotation == 270;
    int cellW = sideways ? w.outputHeight : w.outputWidth;  // panel size on the canvas
    int cellH = sideways ? w.outputWidth : w.outputHeight;
    return {w.x, w.y, w.cols * cellW + (w.cols - 1) * w.gapX, w.rows * cellH + (w.rows - 1) * w.gapY};
}

// Output ids of a group: out-N for "wall" (as before groups existed), else <id>-N.
static std::string wallPrefix(const std::string& id) { return id == "wall" ? "out-" : id + "-"; }

// The canvas grows or shrinks to hold every wall group and every standalone output.
static void fitCanvasToWalls(Layout& l) {
    int cw = 1, ch = 1;
    for (auto& w : l.walls) {
        Rect b = wallBounds(w);
        cw = std::max(cw, b.x + b.w);
        ch = std::max(ch, b.y + b.h);
    }
    for (auto& o : l.outputs)
        if (o.source == "canvas" && !l.findWall(o.group)) {
            cw = std::max(cw, o.crop.x + o.crop.w);
            ch = std::max(ch, o.crop.y + o.crop.h);
        }
    cw = std::clamp(cw, 1, kMaxDimension);
    ch = std::clamp(ch, 1, kMaxDimension);
    // Sources that covered the whole old canvas keep covering it.
    for (auto& s : l.sources)
        if (s.rect.x == 0 && s.rect.y == 0 && s.rect.w == l.canvas.width && s.rect.h == l.canvas.height)
            s.rect = {0, 0, cw, ch};
    l.canvas.width = cw;
    l.canvas.height = ch;
}

void applyWall(Layout& l, WallConfig w) {
    if (w.id.empty()) w.id = w.name.empty() ? "wall" : idFromName(w.name);
    Layout probe;
    probe.walls = {w};
    normalize(probe);  // clamp the wall's values, sanitize its id
    w = probe.walls[0];
    if (w.pixelFormat == PixelFormat::Uyvy && w.outputWidth % 2) w.outputWidth += 1;

    bool stored = false;
    for (auto& existing : l.walls)
        if (existing.id == w.id) {
            // Presets aren't part of the grid settings; keep the stored ones.
            w.presets = existing.presets;
            w.activePreset = existing.activePreset;
            // The group's content follows it: what filled the group still fills it, the rest moves along.
            Rect before = wallBounds(existing), after = wallBounds(w);
            for (auto& src : l.sources) {
                if (src.group != w.id) continue;
                if (src.rect.x == before.x && src.rect.y == before.y && src.rect.w == before.w && src.rect.h == before.h) src.rect = after;
                else src.rect.x += after.x - before.x, src.rect.y += after.y - before.y;
            }
            existing = w;
            stored = true;
        }
    if (!stored) l.walls.push_back(w);

    bool sideways = w.rotation == 90 || w.rotation == 270;
    int cellW = sideways ? w.outputHeight : w.outputWidth;
    int cellH = sideways ? w.outputWidth : w.outputHeight;
    int count = w.cols * w.rows;
    std::string prefix = wallPrefix(w.id);

    // <prefix>N within the grid is reused (keeps its NDI stream); the group's other
    // outputs are dropped; everything else stays where it is in the list.
    std::vector<OutputConfig> panels(count);
    std::vector<OutputConfig> kept;
    size_t insertAt = std::string::npos;
    for (auto& o : l.outputs) {
        int idx = 0;
        if (o.id.rfind(prefix, 0) == 0) {
            std::string tail = o.id.substr(prefix.size());
            if (!tail.empty() && tail.size() < 4 && std::all_of(tail.begin(), tail.end(), ::isdigit)) idx = std::stoi(tail);
            if (std::to_string(idx) != tail) idx = 0;
        }
        bool mine = o.group == w.id;
        if ((idx >= 1 && idx <= count) || mine) {
            if (insertAt == std::string::npos) insertAt = kept.size();
            if (idx >= 1 && idx <= count) panels[idx - 1] = o;
        } else {
            kept.push_back(o);
        }
    }

    std::set<std::string> taken;
    for (auto& o : kept) taken.insert(o.ndiName);
    for (auto& o : panels) taken.insert(o.ndiName);
    auto freeNdiName = [&]() {
        for (int k = 1;; ++k) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s_%02d", l.ndiPrefix.c_str(), k);
            if (!taken.count(buf)) return taken.insert(buf), std::string(buf);
        }
    };

    std::string label = w.id == "wall" && (w.name.empty() || w.name == "Wall") ? "Panel" : w.name;
    for (int r = 0; r < w.rows; ++r)
        for (int c = 0; c < w.cols; ++c) {
            int i = r * w.cols + c;
            OutputConfig& o = panels[i];
            if (o.id.empty()) o.id = prefix + std::to_string(i + 1);
            if (o.ndiName.empty()) o.ndiName = freeNdiName();
            // Named by position (the old name may describe another place); the NDI name is kept.
            o.name = count == 1 ? w.name : label + " R" + std::to_string(r + 1) + " C" + std::to_string(c + 1);
            o.width = w.outputWidth;
            o.height = w.outputHeight;
            o.rotation = w.rotation;
            o.angle = 0.f;
            o.scaleMode = ScaleMode::Stretch;
            o.source = "canvas";
            o.pixelFormat = w.pixelFormat;
            o.group = w.id;
            o.crop = {w.x + c * (cellW + w.gapX), w.y + r * (cellH + w.gapY), cellW, cellH};
        }
    if (insertAt == std::string::npos) insertAt = kept.size();
    kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(insertAt), panels.begin(), panels.end());
    l.outputs = std::move(kept);
    if (w.fitCanvas) fitCanvasToWalls(l);
}

bool moveWall(Layout& l, const std::string& id, int x, int y) {
    for (auto& w : l.walls) {
        if (w.id != id) continue;
        x = std::clamp(x, 0, kMaxDimension);
        y = std::clamp(y, 0, kMaxDimension);
        int dx = x - w.x, dy = y - w.y;
        w.x = x;
        w.y = y;
        for (auto& o : l.outputs)
            if (o.group == id && o.source == "canvas") o.crop.x += dx, o.crop.y += dy;
        for (auto& src : l.sources)
            if (src.group == id) src.rect.x += dx, src.rect.y += dy;
        if (w.fitCanvas) fitCanvasToWalls(l);
        return true;
    }
    return false;
}

bool removeWall(Layout& l, const std::string& id, bool removeOutputs) {
    auto it = std::find_if(l.walls.begin(), l.walls.end(), [&](const WallConfig& w) { return w.id == id; });
    if (it == l.walls.end()) return false;
    l.walls.erase(it);
    if (removeOutputs) {
        l.outputs.erase(std::remove_if(l.outputs.begin(), l.outputs.end(), [&](const OutputConfig& o) { return o.group == id; }),
                        l.outputs.end());
    } else {
        for (auto& o : l.outputs)
            if (o.group == id) o.group.clear();
    }
    for (auto& src : l.sources)
        if (src.group == id) src.group.clear();
    return true;
}

static WallConfig* findWallMut(Layout& l, const std::string& id) {
    for (auto& w : l.walls)
        if (w.id == id) return &w;
    return nullptr;
}

// Group sources go on top of the others, in the given order (the last one on top).
static void replaceGroupSources(Layout& l, const WallConfig& w, const std::vector<SourceConfig>& relative) {
    std::erase_if(l.sources, [&](const SourceConfig& s) { return s.group == w.id; });
    Rect area = wallBounds(w);
    std::string prefix = w.id + "--";
    for (const auto& rel : relative) {
        SourceConfig s = rel;
        s.group = w.id;
        s.rect.x += area.x;
        s.rect.y += area.y;
        if (s.rect.w <= 0 || s.rect.h <= 0) s.rect = area;  // no size = fill the group
        std::string base = sanitizeId(prefix + (rel.id.empty() ? "source" : rel.id));
        s.id = base;
        for (int n = 2; l.findSource(s.id); ++n) s.id = base + "-" + std::to_string(n);
        l.sources.push_back(s);
    }
}

std::string saveGroupPreset(Layout& l, const std::string& groupId, const std::string& name) {
    WallConfig* w = findWallMut(l, groupId);
    if (!w) return "";
    GroupPreset p;
    p.name = name.empty() ? "Preset " + std::to_string(w->presets.size() + 1) : name;
    p.id = idFromName(p.name);
    if (p.id.empty()) p.id = "preset-" + std::to_string(w->presets.size() + 1);
    Rect area = wallBounds(*w);
    std::string prefix = w->id + "--";
    for (const auto& s : l.sources) {
        if (s.group != groupId) continue;
        SourceConfig rel = s;
        rel.group.clear();
        rel.rect.x -= area.x;
        rel.rect.y -= area.y;
        if (rel.id.rfind(prefix, 0) == 0) rel.id = rel.id.substr(prefix.size());
        p.sources.push_back(rel);
    }
    p.arrangement = w->arrangement;
    bool replaced = false;
    for (auto& existing : w->presets)
        if (existing.id == p.id) existing = p, replaced = true;
    if (!replaced) w->presets.push_back(p);
    w->activePreset = p.id;
    return p.id;
}

bool recallGroupPreset(Layout& l, const std::string& groupId, const std::string& presetId) {
    WallConfig* w = findWallMut(l, groupId);
    if (!w) return false;
    for (const auto& p : w->presets)
        if (p.id == presetId) {
            replaceGroupSources(l, *w, p.sources);
            w->activePreset = p.id;
            w->arrangement = p.arrangement;
            return true;
        }
    return false;
}

bool deleteGroupPreset(Layout& l, const std::string& groupId, const std::string& presetId) {
    WallConfig* w = findWallMut(l, groupId);
    if (!w) return false;
    auto n = w->presets.size();
    std::erase_if(w->presets, [&](const GroupPreset& p) { return p.id == presetId; });
    if (w->activePreset == presetId) w->activePreset.clear();
    return w->presets.size() != n;
}

// The group's content as it is now, relative to the group (as presets keep it).
static std::vector<SourceConfig> groupContent(const Layout& l, const WallConfig& w) {
    std::vector<SourceConfig> out;
    Rect area = wallBounds(w);
    std::string prefix = w.id + "--";
    for (const auto& s : l.sources) {
        if (s.group != w.id) continue;
        SourceConfig rel = s;
        rel.group.clear();
        rel.rect.x -= area.x;
        rel.rect.y -= area.y;
        if (rel.id.rfind(prefix, 0) == 0) rel.id = rel.id.substr(prefix.size());
        out.push_back(rel);
    }
    return out;
}

bool showOnGroup(Layout& l, const std::string& groupId, const std::string& ndiName) {
    WallConfig* w = findWallMut(l, groupId);
    if (!w) return false;
    if (ndiName.empty()) {
        std::string error;
        return setGroupContentOn(l, groupId, false, error);
    }
    SlotContent c;
    c.ndiName = ndiName;
    std::vector<SlotContent> content{c};
    std::string error;
    return arrangeGroup(l, groupId, "full", &content, error);
}

std::vector<Arrangement> groupArrangements(const Layout& l, const WallConfig& w) {
    Rect b = wallBounds(w);
    const int W = b.w, H = b.h;
    std::vector<Arrangement> out;
    out.push_back({"full", "One stream", {{"Whole", {0, 0, W, H}}}});
    if (w.cols * w.rows > 1) {
        // One per panel: where each panel's output looks on the canvas, so bezel gaps stay out of the pictures.
        Arrangement a{"panels", "One stream per panel", {}};
        for (auto& o : l.outputs)
            if (o.group == w.id && o.source == "canvas")
                a.slots.push_back({"Panel " + std::to_string(a.slots.size() + 1), {o.crop.x - b.x, o.crop.y - b.y, o.crop.w, o.crop.h}});
        if (a.slots.size() > 1) out.push_back(a);
    }
    out.push_back({"two", "Two side by side", {{"Left", {0, 0, W / 2, H}}, {"Right", {W / 2, 0, W - W / 2, H}}}});
    out.push_back({"quad",
                   "Four (2 x 2)",
                   {{"Top left", {0, 0, W / 2, H / 2}},
                    {"Top right", {W / 2, 0, W - W / 2, H / 2}},
                    {"Bottom left", {0, H / 2, W / 2, H - H / 2}},
                    {"Bottom right", {W / 2, H / 2, W - W / 2, H - H / 2}}}});
    // A main picture in the middle half, two stacked in each outer quarter. On a 32:9 group the
    // main picture and the four small ones are all 16:9.
    const int q = W / 4;
    out.push_back({"five",
                   "Main in the middle, two each side",
                   {{"Main", {q, 0, W - 2 * q, H}},
                    {"Left top", {0, 0, q, H / 2}},
                    {"Left bottom", {0, H / 2, q, H - H / 2}},
                    {"Right top", {W - q, 0, q, H / 2}},
                    {"Right bottom", {W - q, H / 2, q, H - H / 2}}}});
    return out;
}

SlotContent contentOf(const SourceConfig& s) {
    SlotContent c;
    if (s.type == SourceType::Ndi) c.ndiName = s.ndiName;
    else if (s.type == SourceType::Video && !s.playlist.empty()) c.playlist = s.playlist;
    else if (s.type == SourceType::Video) c.video = s.video;
    return c;
}

// The source that plays `c`, without a place yet.
static SourceConfig sourceFor(const SlotContent& c) {
    SourceConfig s;
    if (!c.ndiName.empty()) {
        s.type = SourceType::Ndi;
        s.ndiName = c.ndiName;
        s.name = c.ndiName;
    } else {
        s.type = SourceType::Video;
        s.playlist = c.playlist;
        s.video = c.playlist.empty() ? c.video : "";
        s.name = c.playlist.empty() ? c.video : c.playlist;
    }
    return s;
}

static const Arrangement* findArrangement(const std::vector<Arrangement>& all, const std::string& id) {
    for (auto& a : all)
        if (a.id == id) return &a;
    return nullptr;
}

std::vector<SlotContent> groupSlots(const Layout& l, const WallConfig& w) {
    auto all = groupArrangements(l, w);
    const Arrangement* a = findArrangement(all, w.arrangement);
    std::vector<SlotContent> out(a ? a->slots.size() : 0);
    for (auto& s : l.sources)
        if (s.group == w.id && s.slot >= 1 && s.slot <= static_cast<int>(out.size())) out[static_cast<size_t>(s.slot - 1)] = contentOf(s);
    return out;
}

bool arrangeGroup(Layout& l, const std::string& groupId, const std::string& arrangement, const std::vector<SlotContent>* content, std::string& error) {
    WallConfig* w = findWallMut(l, groupId);
    if (!w) return error = "no such wall group", false;
    auto all = groupArrangements(l, *w);
    const Arrangement* a = findArrangement(all, arrangement);
    if (!a) return error = "group '" + groupId + "' has no arrangement '" + arrangement + "'", false;
    std::vector<SlotContent> fill = content ? *content : groupSlots(l, *w);
    fill.resize(a->slots.size());
    std::vector<SourceConfig> sources;
    for (size_t i = 0; i < a->slots.size(); ++i) {
        if (fill[i].empty()) continue;
        SourceConfig s = sourceFor(fill[i]);
        s.id = "slot-" + std::to_string(i + 1);
        s.slot = static_cast<int>(i + 1);
        s.rect = a->slots[i].rect;
        sources.push_back(s);
    }
    replaceGroupSources(l, *w, sources);
    w->arrangement = a->id;
    w->activePreset.clear();
    w->parked.clear();
    w->parkedArrangement.clear();
    return true;
}

bool setGroupSlot(Layout& l, const std::string& groupId, int slot, const SlotContent& content, std::string& error) {
    WallConfig* w = findWallMut(l, groupId);
    if (!w) return error = "no such wall group", false;
    // Switched off: change what it will show when it comes back on.
    if (!w->parked.empty() && std::none_of(l.sources.begin(), l.sources.end(), [&](auto& s) { return s.group == groupId; })) {
        std::string ignored;
        setGroupContentOn(l, groupId, true, ignored);
    }
    auto all = groupArrangements(l, *w);
    const Arrangement* a = findArrangement(all, w->arrangement);
    if (!a) a = &all.front();  // hand-placed content or none: "full"
    if (slot < 1 || slot > static_cast<int>(a->slots.size()))
        return error = "arrangement '" + a->id + "' has slots 1 to " + std::to_string(a->slots.size()), false;
    std::vector<SlotContent> fill = a->id == w->arrangement ? groupSlots(l, *w) : std::vector<SlotContent>{};
    fill.resize(a->slots.size());
    fill[static_cast<size_t>(slot - 1)] = content;
    return arrangeGroup(l, groupId, a->id, &fill, error);
}

bool setGroupContentOn(Layout& l, const std::string& groupId, bool on, std::string& error) {
    WallConfig* w = findWallMut(l, groupId);
    if (!w) return error = "no such wall group", false;
    bool showing = std::any_of(l.sources.begin(), l.sources.end(), [&](auto& s) { return s.group == groupId; });
    if (!on) {
        if (showing) {
            w->parked = groupContent(l, *w);
            w->parkedArrangement = w->arrangement;
            replaceGroupSources(l, *w, {});
        }
        w->activePreset.clear();
        return true;
    }
    if (showing) return true;
    if (w->parked.empty()) return error = "group '" + groupId + "' has nothing to switch back on; choose what it shows first", false;
    replaceGroupSources(l, *w, w->parked);
    w->arrangement = w->parkedArrangement;
    w->parked.clear();
    w->parkedArrangement.clear();
    return true;
}

const SourceConfig* backgroundSource(const Layout& l) {
    for (auto& s : l.sources)
        if (s.id == "background" && s.group.empty()) return &s;
    return nullptr;
}

bool setBackground(Layout& l, bool on, const SlotContent* content, std::string& error) {
    auto it = std::find_if(l.sources.begin(), l.sources.end(), [](auto& s) { return s.id == "background" && s.group.empty(); });
    if (content && !content->empty()) {
        SourceConfig s = sourceFor(*content);
        s.id = "background";
        s.name = "Background: " + s.name;
        if (it != l.sources.end()) l.sources.erase(it);
        l.sources.insert(l.sources.begin(), s);  // under everything
        it = l.sources.begin();
    }
    if (it == l.sources.end()) {
        if (!on) return true;  // nothing to switch off
        return error = "no background chosen yet: send ndiName, playlist or video", false;
    }
    it->rect = {0, 0, l.canvas.width, l.canvas.height};
    it->enabled = on;
    return true;
}

std::string saveScene(Layout& l, const std::string& name) {
    Scene s;
    s.name = name.empty() ? "Scene " + std::to_string(l.scenes.size() + 1) : name;
    s.id = sanitizeId(s.name);
    if (s.id.empty()) s.id = "scene-" + std::to_string(l.scenes.size() + 1);
    s.sources = l.sources;
    for (auto& existing : l.scenes)
        if (existing.id == s.id) {
            existing = s;
            return s.id;
        }
    l.scenes.push_back(s);
    return s.id;
}

bool recallScene(Layout& l, const std::string& id) {
    for (auto& sc : l.scenes)
        if (sc.id == id) {
            l.sources = sc.sources;
            return true;
        }
    return false;
}

Layout defaultLayout() {
    Layout l;
    l.id = "quad-1080p";
    l.name = "2x2 test (4 x 1080p outputs)";
    l.canvas = {3840, 2160, {0, 0, 0, 1}};
    SourceConfig pattern;
    pattern.id = "pattern";
    pattern.name = "Test pattern";
    pattern.rect = {0, 0, 3840, 2160};
    l.sources.push_back(pattern);
    const Rect regions[4] = {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}, {0, 1080, 1920, 1080}, {1920, 1080, 1920, 1080}};
    for (int i = 0; i < 4; ++i) {
        OutputConfig o;
        o.id = "out-" + std::to_string(i + 1);
        o.name = "Display " + std::to_string(i + 1);
        o.crop = regions[i];
        l.outputs.push_back(o);
    }
    normalize(l);
    return l;
}

}  // namespace mosaic
