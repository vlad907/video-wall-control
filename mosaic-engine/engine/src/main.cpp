// mosaic_engine: GPU NDI mosaic renderer with a REST API and web editor.
//
//   mosaic_engine [--port 8090] [--host 0.0.0.0] [--data ./data] [--layouts ./layouts]
//                 [--ui ./ui/dist] [--run-seconds N] [--snapshot-dir DIR] [--layout ID] [--software-gpu]
//   mosaic_engine --audio-self-test     # the audio delay logic against a simulated network
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "api/http_api.hpp"
#include "core/canvases.hpp"
#include "core/engine.hpp"
#include "core/layout_store.hpp"
#include "io/video_io.hpp"
#include "render/renderer.hpp"

namespace mosaic {
int audioSelfTest();  // audio/audio_selftest.cpp
}

namespace fs = std::filesystem;
using namespace mosaic;

namespace {
std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }

struct Options {
    std::string host = "0.0.0.0";
    int port = 8090;
    fs::path data = "data", layouts = "layouts", ui = "ui/dist", snapshotDir;
    double runSeconds = 0;
    std::string layout;
    bool softwareGpu = false;
    bool audioSelfTest = false;
};

Options parse(int argc, char** argv) {
    Options o;
    // Also settable from the environment so test scripts don't need to pass it.
    if (const char* env = std::getenv("MOSAIC_SOFTWARE_GPU")) o.softwareGpu = std::string(env) == "1";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "--port") o.port = std::stoi(next());
        else if (a == "--host") o.host = next();
        else if (a == "--data") o.data = next();
        else if (a == "--layouts") o.layouts = next();
        else if (a == "--ui") o.ui = next();
        else if (a == "--run-seconds") o.runSeconds = std::stod(next());
        else if (a == "--snapshot-dir") o.snapshotDir = next();
        else if (a == "--layout") o.layout = next();
        else if (a == "--software-gpu") o.softwareGpu = true;
        else if (a == "--audio-self-test") o.audioSelfTest = true;
        else if (a == "--help" || a == "-h") {
            std::cout << "usage: mosaic_engine [--port N] [--host ADDR] [--data DIR] [--layouts DIR] [--ui DIR]\n"
                         "                     [--layout ID] [--run-seconds N] [--snapshot-dir DIR] [--software-gpu]\n";
            std::exit(0);
        } else throw std::runtime_error("unknown option " + a);
    }
    return o;
}

void writeFile(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary);
    out << data;
}
}  // namespace

int main(int argc, char** argv) {
    Options opt;
    try {
        opt = parse(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
    if (opt.audioSelfTest) return audioSelfTest();
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    RendererOptions ro;
    ro.softwareGpu = opt.softwareGpu;

    std::string ndiWhyNot;
    auto ndi = createNdiSystem(ndiWhyNot);
    if (ndi) std::cerr << "[ndi] " << ndi->version() << "\n";
    else std::cerr << "[ndi] disabled: " << ndiWhyNot << "\n";

    LayoutStore store(opt.data);
    store.load(opt.layouts);
    if (!opt.layout.empty() && !store.activate(opt.layout)) {
        std::cerr << "unknown layout '" << opt.layout << "'\n";
        return 2;
    }

    // One engine per canvas, each on its own GPU device and render thread.
    std::filesystem::create_directories(std::filesystem::path(opt.data) / "media");
    Canvases canvases(store, ndi.get(), ro, std::filesystem::path(opt.data) / "media");
    canvases.sync();
    auto first = canvases.primary();
    if (!first || !first->engine) {
        std::cerr << "GPU init failed: " << (first ? first->error : "no canvas") << "\n";
        return 1;
    }
    std::cerr << "[gpu] " << first->renderer->backendName() << " on " << first->renderer->deviceName() << "\n";

    std::error_code binEc;
    auto binDir = std::filesystem::weakly_canonical(std::filesystem::absolute(argv[0]), binEc).parent_path();
    HttpApi api(canvases, store, opt.ui, binDir);
    std::thread http([&] {
        if (!api.listen(opt.host, opt.port)) {
            std::cerr << "cannot listen on " << opt.host << ":" << opt.port << "\n";
            g_quit = true;
        }
    });
    std::cerr << "[http] editor + API on http://" << (opt.host == "0.0.0.0" ? "localhost" : opt.host) << ":" << opt.port << "\n";

    auto started = Clock::now();
    bool snapped = false;
    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        canvases.sync();  // canvases added or removed in the editor
        auto primary = canvases.primary();
        if (!primary || !primary->engine) continue;
        Engine& engine = *primary->engine;
        double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        if (!opt.snapshotDir.empty() && !snapped && elapsed > 2.0) {
            // Full-resolution snapshot of every output, for checking the mapping.
            snapped = true;
            fs::create_directories(opt.snapshotDir);
            for (auto& o : store.active()->outputs) {
                std::string jpeg;
                uint64_t frameId = 0;
                if (engine.snapshotJpeg(o.id, jpeg, frameId)) {
                    writeFile(opt.snapshotDir / (o.id + ".jpg"), jpeg);
                    std::cerr << "[snapshot] " << o.id << " frame " << frameId << "\n";
                }
            }
            std::string jpeg;
            for (int i = 0; i < 20 && !engine.previewJpeg("canvas", jpeg); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!jpeg.empty()) writeFile(opt.snapshotDir / "canvas.jpg", jpeg);
        }
        if (opt.runSeconds > 0 && elapsed >= opt.runSeconds) break;
    }

    if (auto p = canvases.primary(); opt.runSeconds > 0 && p && p->engine) std::cout << p->engine->metrics().dump(2) << std::endl;
    api.stop();
    if (http.joinable()) http.join();
    return 0;  // the canvases' engines stop as `canvases` goes
}
