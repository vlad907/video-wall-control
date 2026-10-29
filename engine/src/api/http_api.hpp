#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace mosaic {

class Engine;
class LayoutStore;

class HttpApi {
public:
    HttpApi(Engine& engine, LayoutStore& store, std::filesystem::path uiDir);
    ~HttpApi();
    // Blocks until stop() is called or binding fails.
    bool listen(const std::string& host, int port);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mosaic
