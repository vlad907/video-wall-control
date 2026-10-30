#include "core/layout.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

using nlohmann::json;

namespace mosaic {

const SourceConfig* Layout::findSource(const std::string& sid) const {
    for (auto& s : sources)
        if (s.id == sid) return &s;
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
    return SourceType::TestPattern;
}

static PixelFormat pixelFormatFrom(const std::string& s) { return s == "bgra" ? PixelFormat::Bgra : PixelFormat::Uyvy; }

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
}

void from_json(const json& j, SourceConfig& s) {
    s.id = j.value("id", s.id);
    s.name = j.value("name", s.name);
    s.type = sourceTypeFrom(j.value("type", std::string(toString(s.type))));
    s.ndiName = j.value("ndiName", s.ndiName);
    if (j.contains("rect")) s.rect = j.at("rect").get<Rect>();
    if (j.contains("crop")) from_json(j.at("crop"), s.crop);
    s.opacity = j.value("opacity", s.opacity);
    if (j.contains("color")) s.color = j.at("color").get<std::array<float, 4>>();
    s.enabled = j.value("enabled", s.enabled);
    s.animated = j.value("animated", s.animated);
}

void to_json(json& j, const OutputConfig& o) {
    j = json{{"id", o.id},           {"name", o.name},         {"ndiName", o.ndiName},
             {"width", o.width},     {"height", o.height},     {"crop", o.crop},
             {"rotation", o.rotation}, {"scaleMode", toString(o.scaleMode)},
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
    if (p.contains("scaleMode")) o.scaleMode = scaleModeFrom(p.at("scaleMode").get<std::string>());
    o.source = p.value("source", o.source);
    if (p.contains("pixelFormat")) o.pixelFormat = pixelFormatFrom(p.at("pixelFormat").get<std::string>());
    o.group = p.value("group", o.group);
    o.enabled = p.value("enabled", o.enabled);
}

void to_json(json& j, const WallConfig& w) {
    j = json{{"cols", w.cols},       {"rows", w.rows},         {"outputWidth", w.outputWidth},
             {"outputHeight", w.outputHeight},                 {"rotation", w.rotation},
             {"gapX", w.gapX},       {"gapY", w.gapY},         {"x", w.x},
             {"y", w.y},             {"pixelFormat", toString(w.pixelFormat)}, {"fitCanvas", w.fitCanvas}};
}

void from_json(const json& j, WallConfig& w) {
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
             {"canvas", l.canvas}, {"sources", l.sources}, {"outputs", l.outputs}, {"scenes", l.scenes}};
    if (l.wall) j["wall"] = *l.wall;
}

void from_json(const json& j, Layout& l) {
    l.id = j.value("id", l.id);
    l.name = j.value("name", l.name);
    l.frameRate = j.value("frameRate", l.frameRate);
    if (j.contains("canvas")) l.canvas = j.at("canvas").get<CanvasConfig>();
    if (j.contains("sources")) l.sources = j.at("sources").get<std::vector<SourceConfig>>();
    if (j.contains("outputs")) l.outputs = j.at("outputs").get<std::vector<OutputConfig>>();
    if (j.contains("wall")) {
        if (j.at("wall").is_null()) l.wall.reset();
        else l.wall = j.at("wall").get<WallConfig>();
    }
    if (j.contains("scenes")) l.scenes = j.at("scenes").get<std::vector<Scene>>();
}

static int clampDim(int v) { return std::clamp(v, 1, kMaxDimension); }

std::vector<std::string> normalize(Layout& l) {
    std::vector<std::string> problems;
    l.id = sanitizeId(l.id.empty() ? l.name : l.id);
    if (l.id.empty()) l.id = "layout";
    if (l.name.empty()) l.name = l.id;
    l.frameRate = std::clamp(l.frameRate, 1.0, 240.0);
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
        if (o.crop.w <= 0 || o.crop.h <= 0) o.crop = {0, 0, o.width, o.height};
        o.crop.w = clampDim(o.crop.w);
        o.crop.h = clampDim(o.crop.h);
        if (o.source != "canvas" && !l.findSource(o.source)) {
            problems.push_back("output '" + o.id + "' references unknown source '" + o.source + "'; using canvas");
            o.source = "canvas";
        }
    }

    if (l.wall) {
        auto& w = *l.wall;
        w.cols = std::clamp(w.cols, 1, 16);
        w.rows = std::clamp(w.rows, 1, 16);
        w.outputWidth = clampDim(w.outputWidth);
        w.outputHeight = clampDim(w.outputHeight);
        w.rotation = (((w.rotation % 360) + 360) % 360) / 90 * 90;
        w.gapX = std::clamp(w.gapX, 0, kMaxDimension);
        w.gapY = std::clamp(w.gapY, 0, kMaxDimension);
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

void applyWall(Layout& l, WallConfig w) {
    Layout probe;
    probe.wall = w;
    normalize(probe);  // clamp the wall's values
    w = *probe.wall;

    bool sideways = w.rotation == 90 || w.rotation == 270;
    int cellW = sideways ? w.outputHeight : w.outputWidth;  // panel size on the canvas
    int cellH = sideways ? w.outputWidth : w.outputHeight;
    if (w.pixelFormat == PixelFormat::Uyvy && w.outputWidth % 2) w.outputWidth += 1;

    if (w.fitCanvas) {
        int cw = std::clamp(w.x + w.cols * cellW + (w.cols - 1) * w.gapX, 1, kMaxDimension);
        int ch = std::clamp(w.y + w.rows * cellH + (w.rows - 1) * w.gapY, 1, kMaxDimension);
        // Sources that covered the whole old canvas keep covering it.
        for (auto& s : l.sources)
            if (s.rect.x == 0 && s.rect.y == 0 && s.rect.w == l.canvas.width && s.rect.h == l.canvas.height)
                s.rect = {0, 0, cw, ch};
        l.canvas.width = cw;
        l.canvas.height = ch;
    }

    int count = w.cols * w.rows;
    std::vector<OutputConfig> others;
    std::vector<OutputConfig> wall(count);
    for (auto& o : l.outputs) {
        // out-N within the grid is reused (keeps its name and NDI stream);
        // other wall outputs are dropped; everything else stays.
        int idx = 0;
        if (o.id.rfind("out-", 0) == 0) {
            try {
                idx = std::stoi(o.id.substr(4));
            } catch (...) {
                idx = 0;
            }
        }
        if (idx >= 1 && idx <= count && std::to_string(idx) == o.id.substr(4)) wall[idx - 1] = o;
        else if (o.group != "wall") others.push_back(o);
    }
    for (int r = 0; r < w.rows; ++r)
        for (int c = 0; c < w.cols; ++c) {
            int i = r * w.cols + c;
            OutputConfig& o = wall[i];
            if (o.id.empty()) {
                o.id = "out-" + std::to_string(i + 1);
                char buf[32];
                std::snprintf(buf, sizeof buf, "Mosaic_Output_%02d", i + 1);
                o.ndiName = buf;
            }
            // Named by position (the old name may describe another place); the NDI name is kept.
            o.name = "Panel R" + std::to_string(r + 1) + " C" + std::to_string(c + 1);
            o.width = w.outputWidth;
            o.height = w.outputHeight;
            o.rotation = w.rotation;
            o.scaleMode = ScaleMode::Stretch;
            o.source = "canvas";
            o.pixelFormat = w.pixelFormat;
            o.group = "wall";
            o.crop = {w.x + c * (cellW + w.gapX), w.y + r * (cellH + w.gapY), cellW, cellH};
        }
    // Wall NDI names win over any other output that happened to use them.
    for (auto& o : others)
        for (auto& wo : wall)
            if (o.ndiName == wo.ndiName) o.ndiName.clear();
    wall.insert(wall.end(), others.begin(), others.end());
    l.outputs = std::move(wall);
    l.wall = w;
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
