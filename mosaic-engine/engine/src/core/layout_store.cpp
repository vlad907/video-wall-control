#include "core/layout_store.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace mosaic {

LayoutStore::LayoutStore(fs::path dataDir)
    : dir_(std::move(dataDir)), layoutsDir_(dir_ / "layouts"), historyDir_(dir_ / "history") {}

constexpr size_t kHistoryKeep = 50;
constexpr auto kHistoryGap = std::chrono::seconds(30);

static std::optional<Layout> readLayout(const fs::path& p) {
    try {
        std::ifstream in(p);
        json j = json::parse(in);
        Layout l = j.get<Layout>();
        if (l.id.empty()) l.id = p.stem().string();
        normalize(l);
        return l;
    } catch (const std::exception& e) {
        std::cerr << "[layouts] cannot read " << p << ": " << e.what() << "\n";
        return std::nullopt;
    }
}

void LayoutStore::load(const fs::path& examplesDir) {
    std::lock_guard lock(mutex_);
    fs::create_directories(layoutsDir_);

    bool empty = fs::is_empty(layoutsDir_);
    if (empty && fs::is_directory(examplesDir)) {
        for (auto& e : fs::directory_iterator(examplesDir))
            if (e.path().extension() == ".json") fs::copy_file(e.path(), layoutsDir_ / e.path().filename());
    }
    for (auto& e : fs::directory_iterator(layoutsDir_)) {
        if (e.path().extension() != ".json") continue;
        if (auto l = readLayout(e.path())) layouts_.push_back(std::move(*l));
    }
    if (layouts_.empty()) {
        layouts_.push_back(defaultLayout());
        writeFile(layouts_.back());
    }
    std::sort(layouts_.begin(), layouts_.end(), [](auto& a, auto& b) { return a.id < b.id; });

    try {
        std::ifstream in(dir_ / "state.json");
        if (in) activeId_ = json::parse(in).value("active", "");
    } catch (...) {
    }
    auto it = std::find_if(layouts_.begin(), layouts_.end(), [&](auto& l) { return l.id == activeId_; });
    if (it == layouts_.end()) {
        // Prefer the 1080p test layout on first run, since it renders on any GPU.
        auto def = std::find_if(layouts_.begin(), layouts_.end(), [](auto& l) { return l.id == "quad-1080p"; });
        it = def != layouts_.end() ? def : layouts_.begin();
    }
    activeId_ = it->id;
    activeSnapshot_ = std::make_shared<const Layout>(*it);
    version_++;
    writeState();
}

std::shared_ptr<const Layout> LayoutStore::active() const {
    std::lock_guard lock(mutex_);
    return activeSnapshot_;
}

std::vector<LayoutSummary> LayoutStore::list() const {
    std::lock_guard lock(mutex_);
    std::vector<LayoutSummary> out;
    for (auto& l : layouts_) out.push_back({l.id, l.name, static_cast<int>(l.outputs.size()), l.id == activeId_});
    return out;
}

std::optional<Layout> LayoutStore::get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    for (auto& l : layouts_)
        if (l.id == id) return l;
    return std::nullopt;
}

std::vector<std::string> LayoutStore::save(Layout layout) {
    auto problems = normalize(layout);
    std::lock_guard lock(mutex_);
    auto it = std::find_if(layouts_.begin(), layouts_.end(), [&](auto& l) { return l.id == layout.id; });
    if (it != layouts_.end()) *it = layout;
    else layouts_.push_back(layout);
    snapshot(layout.id, false);
    writeFile(layout);
    if (layout.id == activeId_) publish(layout);
    return problems;
}

bool LayoutStore::activate(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (auto& l : layouts_) {
        if (l.id == id) {
            activeId_ = id;
            publish(l);
            writeState();
            return true;
        }
    }
    return false;
}

bool LayoutStore::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    if (id == activeId_) return false;  // can't delete the layout on air
    auto it = std::find_if(layouts_.begin(), layouts_.end(), [&](auto& l) { return l.id == id; });
    if (it == layouts_.end()) return false;
    layouts_.erase(it);
    snapshot(id, true);  // a deleted layout can still be restored from its history
    std::error_code ec;
    fs::remove(fileFor(id), ec);
    return true;
}

bool LayoutStore::mutateActive(const std::function<bool(Layout&)>& fn, std::vector<std::string>* problems) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(layouts_.begin(), layouts_.end(), [&](auto& l) { return l.id == activeId_; });
    if (it == layouts_.end()) return false;
    Layout copy = *it;
    if (!fn(copy)) return false;
    copy.id = activeId_;  // the active layout keeps its id
    auto p = normalize(copy);
    if (problems) *problems = std::move(p);
    *it = copy;
    snapshot(copy.id, false);
    writeFile(copy);
    publish(std::move(copy));
    return true;
}

void LayoutStore::snapshot(const std::string& id, bool force) const {
    std::error_code ec;
    fs::path src = fileFor(id);
    if (!fs::exists(src, ec)) return;
    fs::path dir = historyDir_ / id;
    fs::create_directories(dir, ec);
    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".json") entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    auto last = lastSnapshot_.find(id);
    if (!force && last != lastSnapshot_.end() && std::chrono::steady_clock::now() - last->second < kHistoryGap) return;
    lastSnapshot_[id] = std::chrono::steady_clock::now();
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream name;
    name << std::put_time(&tm, "%Y%m%d-%H%M%S") << '-' << std::setw(3) << std::setfill('0') << ms << ".json";
    fs::copy_file(src, dir / name.str(), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[layouts] cannot keep a history copy of " << id << ": " << ec.message() << "\n";
        return;
    }
    entries.push_back(dir / name.str());
    while (entries.size() > kHistoryKeep) {
        fs::remove(entries.front(), ec);
        entries.erase(entries.begin());
    }
}

std::vector<LayoutVersion> LayoutStore::history(const std::string& id) const {
    std::lock_guard lock(mutex_);
    std::vector<LayoutVersion> out;
    std::error_code ec;
    if (sanitizeId(id) != id) return out;
    for (auto& e : fs::directory_iterator(historyDir_ / id, ec)) {
        if (e.path().extension() != ".json") continue;
        LayoutVersion v;
        v.version = e.path().stem().string();
        const std::string& s = v.version;  // 20261101-154212-123
        if (s.size() >= 15) v.time = s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2) + " " + s.substr(9, 2) + ":" + s.substr(11, 2) + ":" + s.substr(13, 2);
        try {
            std::ifstream in(e.path());
            json j = json::parse(in);
            v.name = j.value("name", "");
            v.outputs = j.contains("outputs") ? static_cast<int>(j["outputs"].size()) : 0;
        } catch (...) {
            continue;
        }
        out.push_back(v);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.version > b.version; });
    return out;
}

bool LayoutStore::restore(const std::string& id, const std::string& version, std::vector<std::string>* problems) {
    if (sanitizeId(id) != id || sanitizeId(version) != version) return false;
    auto old = readLayout(historyDir_ / id / (version + ".json"));
    if (!old) return false;
    old->id = id;
    std::lock_guard lock(mutex_);
    auto p = normalize(*old);
    if (problems) *problems = std::move(p);
    snapshot(id, true);
    auto it = std::find_if(layouts_.begin(), layouts_.end(), [&](auto& l) { return l.id == id; });
    if (it != layouts_.end()) *it = *old;
    else layouts_.push_back(*old);  // a deleted layout comes back
    writeFile(*old);
    if (id == activeId_) publish(*old);
    return true;
}

fs::path LayoutStore::fileFor(const std::string& id) const { return layoutsDir_ / (id + ".json"); }

void LayoutStore::writeFile(const Layout& l) const {
    // Write-then-rename so a crash mid-write never leaves a truncated layout.
    auto path = fileFor(l.id);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << json(l).dump(2) << "\n";
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) std::cerr << "[layouts] cannot write " << path << ": " << ec.message() << "\n";
}

void LayoutStore::writeState() const {
    std::ofstream out(dir_ / "state.json", std::ios::trunc);
    out << json{{"active", activeId_}}.dump(2) << "\n";
}

void LayoutStore::publish(Layout l) {
    activeSnapshot_ = std::make_shared<const Layout>(std::move(l));
    version_++;
}

}  // namespace mosaic
