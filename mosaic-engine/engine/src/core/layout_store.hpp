// Persists layouts as JSON files and publishes the active layout as an
// immutable snapshot the render thread can pick up without locking.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/layout.hpp"

namespace mosaic {

struct LayoutSummary {
    std::string id, name;
    int outputs = 0;
    bool active = false;
};

class LayoutStore {
public:
    explicit LayoutStore(std::filesystem::path dataDir);

    // Loads saved layouts; seeds from examplesDir (or a built-in default) on first run.
    void load(const std::filesystem::path& examplesDir);

    std::shared_ptr<const Layout> active() const;
    uint64_t version() const { return version_.load(); }

    std::vector<LayoutSummary> list() const;
    std::optional<Layout> get(const std::string& id) const;

    // Validates, writes to disk, and republishes if it is the active layout.
    // Returns problems found during normalization (empty = clean).
    std::vector<std::string> save(Layout layout);
    bool activate(const std::string& id);
    bool remove(const std::string& id);

    // Copy-modify-publish on the active layout. fn returns false to abort.
    bool mutateActive(const std::function<bool(Layout&)>& fn, std::vector<std::string>* problems = nullptr);

private:
    std::filesystem::path fileFor(const std::string& id) const;
    void writeFile(const Layout& l) const;
    void writeState() const;
    void publish(Layout l);

    std::filesystem::path dir_, layoutsDir_;
    mutable std::mutex mutex_;
    std::vector<Layout> layouts_;
    std::string activeId_;
    std::shared_ptr<const Layout> activeSnapshot_;
    std::atomic<uint64_t> version_{0};
};

}  // namespace mosaic
