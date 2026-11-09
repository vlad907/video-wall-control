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
    if (s.type == SourceType::Video) {
        j["video"] = s.video;
        j["loop"] = s.loop;
        j["playing"] = s.playing;
    }
    if (!s.group.empty()) j["group"] = s.group;
}

void from_json(const json& j, SourceConfig& s) {
    s.id = j.value("id", s.id);
    s.name = j.value("name", s.name);
    s.type = sourceTypeFrom(j.value("type", std::string(toString(s.type))));
    s.ndiName = j.value("ndiName", s.ndiName);
    s.image = j.value("image", s.image);
    s.video = j.value("video", s.video);
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

void to_json(json& j, const GroupPreset& p) { j = json{{"id", p.id}, {"name", p.name}, {"sources", p.sources}}; }

void from_json(const json& j, GroupPreset& p) {
    p.id = j.value("id", p.id);
    p.name = j.value("name", p.name);
    if (j.contains("sources")) p.sources = j.at("sources").get<std::vector<SourceConfig>>();
}

void to_json(json& j, const OutputConfig& o) {
    j = json{{"id", o.id},           {"name", o.name},         {"ndiName", o.ndiName},
             {"width", o.width},     {"height", o.height},     {"crop", o.crop},
             {"rotation", o.rotation}, {"angle", o.angle}, {"scaleMode", toString(o.scaleMode)},
             {"source", o.source},   {"pixelFormat", toString(o.pixelFormat)}, {"enabled", o.enabled}};
    if (!o.group.empty()) j["group"] = o.group;
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
    o.enabled = p.value("enabled", o.enabled);
}

void to_json(json& j, const WallConfig& w) {
    j = json{{"id", w.id},           {"name", w.name},         {"cols", w.cols},       {"rows", w.rows},         {"outputWidth", w.outputWidth},
             {"outputHeight", w.outputHeight},                 {"rotation", w.rotation},
             {"gapX", w.gapX},       {"gapY", w.gapY},         {"x", w.x},
             {"y", w.y},             {"pixelFormat", toString(w.pixelFormat)}, {"fitCanvas", w.fitCanvas},
             {"presets", w.presets}, {"activePreset", w.activePreset}, {"fallbackImage", w.fallbackImage}};
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
}

void to_json(json& j, const EndpointDisplay& d) { j = json{{"screen", d.screen}, {"output", d.output}}; }

void from_json(const json& j, EndpointDisplay& d) {
    d.screen = j.value("screen", d.screen);
    d.output = j.value("output", d.output);
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
             {"receiverBufferMs", l.receiverBufferMs}};
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
}

static int clampDim(int v) { return std::clamp(v, 1, kMaxDimension); }

std::vector<std::string> normalize(Layout& l) {
    std::vector<std::string> problems;
    l.id = sanitizeId(l.id.empty() ? l.name : l.id);
    if (l.id.empty()) l.id = "layout";
    if (l.name.empty()) l.name = l.id;
    l.frameRate = std::clamp(l.frameRate, 1.0, 240.0);
    l.receiverBufferMs = std::clamp(l.receiverBufferMs, 0, 1000);
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
            char buf[32];
            std::snprintf(buf, sizeof buf, "Mosaic_Output_%02d", n);
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
            char buf[32];
            std::snprintf(buf, sizeof buf, "Mosaic_Output_%02d", k);
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

bool showOnGroup(Layout& l, const std::string& groupId, const std::string& ndiName) {
    WallConfig* w = findWallMut(l, groupId);
    if (!w) return false;
    std::vector<SourceConfig> content;
    if (!ndiName.empty()) {
        SourceConfig s;
        s.id = "live";
        s.name = ndiName;
        s.type = SourceType::Ndi;
        s.ndiName = ndiName;
        content.push_back(s);  // no rect: fills the group
    }
    replaceGroupSources(l, *w, content);
    w->activePreset.clear();
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
