#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace mosaic {

class Canvases;
class LayoutStore;

class HttpApi {
public:
    // binDir: where the engine's own executable is; the Mosaic Receiver built next to it is
    // handed out to receiving PCs from there (GET /api/receiver/...).
    HttpApi(Canvases& canvases, LayoutStore& store, std::filesystem::path uiDir, std::filesystem::path binDir = {});
    ~HttpApi();
    // Blocks until stop() is called or binding fails.
    bool listen(const std::string& host, int port);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mosaic
