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
    j = json{{"id", s.id}, {"name", s.name}, {"type", toString(s.type)}, {"rect", s.rect}, {"enabled", s.enabled}};
    if (s.type == SourceType::Ndi) j["ndiName"] = s.ndiName;
    if (s.type == SourceType::Solid) j["color"] = s.color;
}

void from_json(const json& j, SourceConfig& s) {
    s.id = j.value("id", s.id);
    s.name = j.value("name", s.name);
    s.type = sourceTypeFrom(j.value("type", std::string(toString(s.type))));
    s.ndiName = j.value("ndiName", s.ndiName);
    if (j.contains("rect")) s.rect = j.at("rect").get<Rect>();
    if (j.contains("color")) s.color = j.at("color").get<std::array<float, 4>>();
    s.enabled = j.value("enabled", s.enabled);
}

void to_json(json& j, const OutputConfig& o) {
    j = json{{"id", o.id},           {"name", o.name},         {"ndiName", o.ndiName},
             {"width", o.width},     {"height", o.height},     {"crop", o.crop},
             {"rotation", o.rotation}, {"scaleMode", toString(o.scaleMode)},
             {"source", o.source},   {"enabled", o.enabled}};
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
    o.enabled = p.value("enabled", o.enabled);
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
             {"canvas", l.canvas}, {"sources", l.sources}, {"outputs", l.outputs}};
}

void from_json(const json& j, Layout& l) {
    l.id = j.value("id", l.id);
    l.name = j.value("name", l.name);
    l.frameRate = j.value("frameRate", l.frameRate);
    if (j.contains("canvas")) l.canvas = j.at("canvas").get<CanvasConfig>();
    if (j.contains("sources")) l.sources = j.at("sources").get<std::vector<SourceConfig>>();
    if (j.contains("outputs")) l.outputs = j.at("outputs").get<std::vector<OutputConfig>>();
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
    return problems;
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
