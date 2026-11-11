#include "api/http_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>  // GetComputerNameA (after httplib, which sets up winsock first)
#else
#include <unistd.h>
#endif

#include "audio/audio_device.hpp"
#include "core/canvases.hpp"
#include "io/capture.hpp"
#include "io/video_file.hpp"
#include "core/engine.hpp"
#include "core/layout_store.hpp"
#include "util/paths.hpp"

using nlohmann::json;

namespace mosaic {

namespace {

void sendJson(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(j.dump(), "application/json");
}

void sendError(httplib::Response& res, int status, const std::string& msg) { sendJson(res, {{"error", msg}}, status); }

json parseBody(const httplib::Request& req) {
    if (req.body.empty()) return json::object();
    return json::parse(req.body);  // throws on bad JSON -> 400
}

json withProblems(json j, const std::vector<std::string>& problems) {
    if (!problems.empty()) j["warnings"] = problems;
    return j;
}

std::string uniqueId(const Layout& l, const std::string& prefix, bool output) {
    for (int n = 1;; ++n) {
        std::string id = prefix + std::to_string(n);
        if (output ? !l.findOutput(id) : !l.findSource(id)) return id;
    }
}

// This PC's name as NDI puts it in front of stream names: "MEDIASERVER (Mosaic_Output_01)".
std::string machineName() {
#ifdef _WIN32
    char buf[256];
    DWORD len = sizeof buf;
    if (GetComputerNameA(buf, &len)) return hostKey(std::string(buf, len));
#else
    char buf[256] = {};
    if (gethostname(buf, sizeof buf - 1) == 0) return hostKey(buf);
#endif
    return "LOCALHOST";
}

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---- updates ----

// The git checkout this engine was built from: the first folder above the executable that has
// .git and update.ps1 (the repository root), or empty.
std::filesystem::path findAppDir(std::filesystem::path dir) {
    std::error_code ec;
    dir = std::filesystem::absolute(dir, ec);
    for (int i = 0; i < 6 && !dir.empty(); ++i, dir = dir.parent_path()) {
        if (std::filesystem::exists(dir / ".git", ec) && std::filesystem::exists(dir / "update.ps1", ec)) return dir;
        if (dir == dir.parent_path()) break;
    }
    return {};
}

std::string readFirstLine(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

// The checked-out commit, short (as `git log` shows it), read from .git without running git.
std::string gitVersion(const std::filesystem::path& appDir) {
    if (appDir.empty()) return "";
    auto git = appDir / ".git";
    std::string head = readFirstLine(git / "HEAD");
    std::string hash = head;
    if (head.rfind("ref: ", 0) == 0) {
        std::string ref = head.substr(5);
        hash = readFirstLine(git / ref);
        if (hash.empty()) {
            std::ifstream packed(git / "packed-refs");
            for (std::string line; std::getline(packed, line);)
                if (line.size() > ref.size() + 41 && line.compare(line.size() - ref.size(), ref.size(), ref) == 0) hash = line.substr(0, 40);
        }
    }
    return hash.size() >= 7 ? hash.substr(0, 7) : "";
}

// The outcome of the last update of this PC: the end of logs/update.log (update.ps1 -Unattended).
json lastUpdateResult(const std::filesystem::path& appDir) {
    auto log = appDir / "logs" / "update.log";
    std::error_code ec;
    if (appDir.empty() || !std::filesystem::exists(log, ec)) return nullptr;
    auto when = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::last_write_time(log, ec) - std::filesystem::file_time_type::clock::now() +
                                                                 std::chrono::system_clock::now().time_since_epoch())
                    .count();
    std::ifstream f(log);
    std::string line, result;
    bool ok = false;
    for (std::string l; std::getline(f, l);) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.find("Up to date:") != std::string::npos) result = l, ok = true;
        else if (l.find("failed") != std::string::npos || l.find("errors") != std::string::npos) result = l, ok = false;
    }
    if (result.empty()) result = "no result in logs\\update.log (still running, or stopped early)";
    return {{"time", when}, {"ok", ok}, {"result", result}};
}

#ifdef _WIN32
// Runs a command without a window; its exit code, or -1.
int runHidden(std::wstring cmd, DWORD flags = 0, bool wait = true) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | flags, nullptr, nullptr, &si, &pi)) return -1;
    DWORD code = 0;
    if (wait) {
        WaitForSingleObject(pi.hProcess, 15000);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
#endif

// Starts update.ps1 on this PC, outside the engine: it stops the engine to rebuild it. Through
// the "MosaicUpdate" scheduled task (install-engine / update registers it), else a detached
// PowerShell that breaks away from the engine's task.
bool startServerUpdate(const std::filesystem::path& appDir, std::string& error) {
    if (appDir.empty()) return error = "can't find the video-wall-control folder (with .git and update.ps1) above the engine", false;
#ifdef _WIN32
    if (runHidden(L"schtasks.exe /Run /TN MosaicUpdate") == 0) return true;
    std::wstring cmd = L"powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"" + (appDir / "update.ps1").wstring() + L"\" -Unattended";
    if (runHidden(cmd, CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB, false) == 0) return true;
    if (runHidden(cmd, CREATE_NEW_PROCESS_GROUP, false) == 0) return true;
    return error = "couldn't start update.ps1", false;
#else
    return error = "updating the media server from the editor works on Windows; here run git pull and rebuild", false;
#endif
}

constexpr double kEndpointOnlineSeconds = 20;

// A media file name we accept: letters, digits, "-", "_", "." and an image or video extension.
std::string mediaName(const std::string& raw) {
    std::string name;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') name += c;
        else if (c == ' ') name += '-';
    }
    if (name.empty() || name[0] == '.' || name.find("..") != std::string::npos) return "";
    auto dot = name.rfind('.');
    std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != "png" && ext != "jpg" && ext != "jpeg" && ext != "bmp" && ext != "gif" && ext != "mp4" && ext != "mov" && ext != "m4v") return "";
    return name.substr(0, dot + 1) + ext;
}

// "image" or "video" by the extension, "" if it's neither. Any other characters are fine
// (files copied into the folder by hand often have spaces, brackets and so on).
std::string mediaKind(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "gif") return "image";
    if (ext == "mp4" || ext == "mov" || ext == "m4v") return "video";
    return "";
}

// A file in the media folder named in a request: a plain name (no folders), of a media kind.
bool safeMediaName(const std::string& name) {
    return !name.empty() && name.find_first_of("/\\") == std::string::npos && name.find("..") == std::string::npos && !mediaKind(name).empty();
}

bool isVideoName(const std::string& name) { return name.ends_with(".mp4") || name.ends_with(".mov") || name.ends_with(".m4v"); }

const char* mediaType(const std::string& name) {
    if (name.ends_with(".mp4") || name.ends_with(".m4v")) return "video/mp4";
    if (name.ends_with(".mov")) return "video/quicktime";
    if (name.ends_with(".png")) return "image/png";
    if (name.ends_with(".bmp")) return "image/bmp";
    if (name.ends_with(".gif")) return "image/gif";
    return "image/jpeg";
}  // endpoints check in every 5 s

}  // namespace

// The canvas a request is about (LayoutStore::setRequestCanvas) and its engine, held for the
// request so a canvas stopped meanwhile finishes it safely. Set in the pre-routing handler.
thread_local std::shared_ptr<CanvasRun> tlRun;

struct HttpApi::Impl {
    Canvases& canvases;
    LayoutStore& store;
    // This request's canvas's engine.
    Engine& eng() {
        if (!tlRun) tlRun = canvases.current();
        if (!tlRun || !tlRun->engine) throw std::runtime_error("this canvas isn't running" + (tlRun ? ": " + tlRun->error : std::string()));
        return *tlRun->engine;
    }
    httplib::Server server;
    std::atomic<bool> running{true};
    const std::string machine = machineName();

    // Optional access keys (data/security.json). When either is set, requests that change
    // something need one of them in an X-Api-Key header (or ?key=). Reading stays open.
    std::mutex securityMutex;
    std::string editorPin, apiKey;

    std::filesystem::path securityFile() const { return canvases.mediaDir().parent_path() / "security.json"; }

    void loadSecurity() {
        try {
            std::ifstream in(securityFile());
            if (!in) return;
            json j = json::parse(in);
            std::lock_guard lock(securityMutex);
            editorPin = j.value("editorPin", "");
            apiKey = j.value("apiKey", "");
        } catch (const std::exception& e) {
            std::cerr << "[http] cannot read " << securityFile() << ": " << e.what() << "\n";
        }
    }

    void saveSecurity() {
        auto path = securityFile();
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << json{{"editorPin", editorPin}, {"apiKey", apiKey}}.dump(2) << "\n";
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
    }

    static bool sameKey(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        unsigned char diff = 0;  // no early exit, so timing doesn't reveal how much matched
        for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        return diff == 0;
    }

    // True if the request may change things.
    bool allowed(const httplib::Request& req) {
        std::lock_guard lock(securityMutex);
        if (editorPin.empty() && apiKey.empty()) return true;
        std::string key = req.has_header("X-Api-Key") ? req.get_header_value("X-Api-Key") : req.get_param_value("key");
        if (key.empty()) return false;
        return (!editorPin.empty() && sameKey(key, editorPin)) || (!apiKey.empty() && sameKey(key, apiKey));
    }

    // Last check-in of each receiving PC, by hostKey (not saved; they check in every few seconds).
    std::mutex checkinMutex;
    std::map<std::string, json> checkins;

    // Full NDI name of one of our outputs, as receivers see it on the network.
    std::string fullStreamName(const std::string& ndiName, const std::vector<std::string>& discovered) const {
        std::string suffix = " (" + ndiName + ")";
        std::string any;
        for (auto& n : discovered)
            if (n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
                if (hostKey(n.substr(0, n.size() - suffix.size())) == machine) return n;
                if (any.empty()) any = n;
            }
        return any.empty() ? machine + suffix : any;
    }

    // What each display slot of an endpoint should show.
    json endpointDisplays(const Layout& l, const Endpoint& e, const std::vector<std::string>& discovered) const {
        json out = json::array();
        int index = 0;
        for (auto& d : e.displays) {
            ++index;
            json j = {{"index", index}, {"screen", d.screen}, {"output", d.output}};
            if (const OutputConfig* o = l.findOutput(d.output)) {
                j["label"] = o->name;
                j["ndiName"] = o->ndiName;
                j["stream"] = fullStreamName(o->ndiName, discovered);
                j["width"] = o->width;
                j["height"] = o->height;
                j["enabled"] = o->enabled;
            } else {
                j["error"] = d.output.empty() ? "no output assigned" : "no such output '" + d.output + "'";
            }
            out.push_back(j);
        }
        return out;
    }

    // Config plus live status (from its last check-in) of one endpoint.
    json endpointView(const Layout& l, const Endpoint& e, const std::vector<std::string>& discovered) {
        json j = e;
        json displays = endpointDisplays(l, e, discovered);
        json last = json::object();  // empty until the PC has checked in
        {
            std::lock_guard lock(checkinMutex);
            auto it = checkins.find(hostKey(e.host));
            if (it != checkins.end()) last = it->second;
        }
        double ago = last.contains("time") ? nowSeconds() - last.value("time", 0.0) : -1;
        j["online"] = ago >= 0 && ago < kEndpointOnlineSeconds;
        j["lastSeenSecondsAgo"] = ago < 0 ? json(nullptr) : json(std::round(ago));
        j["address"] = last.value("address", "");
        j["receiverVersion"] = last.value("receiverVersion", "");
        j["receiverUpdate"] = last.value("receiverUpdate", "");
        j["version"] = last.value("version", "");
        j["appVersion"] = last.value("appVersion", "");
        j["screens"] = last.value("screens", json::array());
        j["diagnostics"] = last.value("diagnostics", json::object());  // GPU load, display-driver events, screen changes
        // With the Mosaic Receiver, how far apart its displays were (frames) and how often in step.
        json rx = last.value("receiver", json::object());
        if (!rx.empty())
            j["receiver"] = {{"spreadMax10s", rx.value("spreadMax10s", 0)},
                             {"alignedRatio", rx.value("alignedRatio", 1.0)},
                             {"refreshHz", rx.value("refreshHz", 0.0)},
                             {"presentFps", rx.value("presentFps", 0.0)},
                             {"timing", rx.value("timing", "")},
                             {"screenSpreadMs", rx.value("screenSpreadMs", 0.0)},
                             {"screenSpreadMaxMs", rx.value("screenSpreadMaxMs", 0.0)},
                             {"screenSpreadNowMs", rx.value("screenSpreadNowMs", 0.0)},
                             {"compensate", rx.value("compensate", true)},
                             {"uploadMs", rx.value("uploadMs", 0.0)},
                             {"bufferMs", rx.value("bufferMs", 0)},
                             {"bufferReady", rx.value("bufferReady", false)},
                             {"heldMs", rx.value("heldMs", 0.0)},
                             {"gapsAbsorbed", rx.value("gapsAbsorbed", 0)},
                             {"freezesLastMinute", rx.value("freezesLastMinute", 0)},
                             {"freezesTotal", rx.value("freezesTotal", 0)},
                             {"freezes", rx.value("freezes", json::array())},
                             {"presenter", rx.value("presenter", "")},
                             {"error", rx.value("error", "")}};
        json rxDisplays = rx.value("displays", json::array());
        json panels = last.value("panels", json::array());
        for (auto& d : displays) {
            // The dashboard numbers its panels like the display slots.
            for (auto& p : panels)
                if (p.value("index", 0) == d["index"]) {
                    d["reachable"] = p.value("reachable", false);
                    d["playing"] = p.value("playing", "");
                    d["onScreen"] = p.value("screen", "");
                }
            for (auto& r : rxDisplays)
                if (r.value("index", 0) == d["index"]) {
                    d["shownFrameId"] = r.value("shownFrameId", 0);
                    d["late"] = r.value("late", 0);
                    d["fps"] = r.value("fps", 0.0);
                    d["offsetMs"] = r.value("offsetMs", 0.0);
                    d["offsetMaxMs"] = r.value("offsetMaxMs", 0.0);
                    d["missedRefreshes"] = r.value("missedRefreshes", 0);
                    d["windowsLateMs"] = r.value("windowsLateMs", 0.0);
                    d["heldRefreshes"] = r.value("heldRefreshes", 0);
                }
            d["ok"] = j["online"].get<bool>() && d.value("reachable", false) && d.contains("stream") &&
                      d.value("playing", "") == d.value("stream", "");
        }
        j["displays"] = displays;
        return j;
    }

    Impl(Canvases& c, LayoutStore& s) : canvases(c), store(s) {}

    // ---- the Mosaic Receiver, handed out to receiving PCs (their dashboards update themselves) ----
    std::filesystem::path binDir;

    // ---- "Update everything": the request (data/update.json) receiving PCs pick up at check-in ----
    std::filesystem::path appDir;
    std::mutex updateMutex;
    json updateRequest = json::object();  // {id, requestedAt, target: all | receivers | server, serverError}
    std::filesystem::path updateFile() const { return canvases.mediaDir().parent_path() / "update.json"; }
    void loadUpdateRequest() {
        std::ifstream f(updateFile());
        json j = json::parse(f, nullptr, false);
        if (j.is_object()) updateRequest = j;
    }
    void saveUpdateRequest() {
        std::error_code ec;
        std::filesystem::create_directories(updateFile().parent_path(), ec);
        std::ofstream(updateFile()) << updateRequest.dump(2);
    }
    // Added to every check-in reply when receiving PCs should update.
    json updateForEndpoints() {
        std::lock_guard lock(updateMutex);
        if (updateRequest.value("id", 0) <= 0 || updateRequest.value("target", "all") == "server") return nullptr;
        return {{"id", updateRequest.value("id", 0)}};
    }

    static const std::vector<std::string>& receiverFileNames() {
#ifdef _WIN32
        static const std::vector<std::string> names = {"mosaic_receiver.exe", "Processing.NDI.Lib.x64.dll"};
#else
        static const std::vector<std::string> names = {"mosaic_receiver"};
#endif
        return names;
    }

    // The files that exist, and a version: size and modification time of each, so a rebuild
    // changes it. Receiving PCs compare it with what they installed.
    json receiverPackage() const {
        json files = json::array();
        std::string version;
        for (auto& name : receiverFileNames()) {
            std::error_code ec;
            auto p = binDir / name;
            auto size = std::filesystem::file_size(p, ec);
            if (ec) continue;
            long long mtime = static_cast<long long>(std::filesystem::last_write_time(p, ec).time_since_epoch().count());
            files.push_back({{"name", name}, {"bytes", size}});
            version += name + ":" + std::to_string(size) + ":" + std::to_string(mtime) + ";";
        }
        bool ok = !files.empty() && files[0]["name"] == receiverFileNames()[0];
        // Short and stable: FNV-1a of the description.
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : version) h = (h ^ c) * 1099511628211ull;
        char buf[17];
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
        return {{"available", ok}, {"version", ok ? std::string(buf) : ""}, {"files", files}};
    }

    // Wraps a handler so malformed JSON and unexpected errors become 400/500 replies.
    template <class F>
    auto guard(F f) {
        return [f](const httplib::Request& req, httplib::Response& res) {
            try {
                f(req, res);
            } catch (const json::exception& e) {
                sendError(res, 400, std::string("bad request: ") + e.what());
            } catch (const std::exception& e) {
                sendError(res, 500, e.what());
            }
        };
    }

    void routes() {
        auto& s = server;

        s.Get("/api/health", [](const httplib::Request&, httplib::Response& res) { sendJson(res, {{"ok", true}}); });

        // ---- access: optional editor PIN and API key ----
        loadSecurity();
        s.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            // Which canvas this request is about: the editor sends X-Mosaic-Canvas (?canvas= for
            // event streams, which can't set headers). None = the primary canvas.
            std::string canvas = req.has_header("X-Mosaic-Canvas") ? req.get_header_value("X-Mosaic-Canvas") : req.get_param_value("canvas");
            LayoutStore::setRequestCanvas(canvas);
            tlRun.reset();
            bool changes = req.method != "GET" && req.method != "HEAD" && req.method != "OPTIONS";
            if (changes && !canvas.empty() && !store.snapshot(canvas) && req.path.rfind("/api/", 0) == 0) {
                // Never let a change meant for a stopped canvas land on the primary one.
                sendError(res, 409, "canvas '" + canvas + "' isn't running any more: reload the editor");
                return httplib::Server::HandlerResponse::Handled;
            }
            // Receiving PCs check in without a key; that only reports their status and reads their assignment.
            if (!changes || req.path.rfind("/api/", 0) != 0 || req.path == "/api/endpoints/checkin" || allowed(req))
                return httplib::Server::HandlerResponse::Unhandled;
            sendError(res, 401, "a PIN or API key is needed to change things (X-Api-Key header)");
            return httplib::Server::HandlerResponse::Handled;
        });
        s.Get("/api/security", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard lock(securityMutex);
            bool locked = !editorPin.empty() || !apiKey.empty();
            sendJson(res, {{"pinSet", !editorPin.empty()}, {"apiKeySet", !apiKey.empty()}, {"locked", locked}});
        }));
        // {"editorPin": "...", "apiKey": "..."}: "" removes one. Needs the current PIN or key once one is set.
        s.Put("/api/security", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            {
                std::lock_guard lock(securityMutex);
                if (body.contains("editorPin")) editorPin = body.at("editorPin").get<std::string>();
                if (body.contains("apiKey")) apiKey = body.at("apiKey").get<std::string>();
                saveSecurity();
            }
            sendJson(res, {{"pinSet", !editorPin.empty()}, {"apiKeySet", !apiKey.empty()}});
        }));
        // Lets the editor check a PIN without changing anything.
        s.Post("/api/security/check", guard([](const httplib::Request&, httplib::Response& res) { sendJson(res, {{"ok", true}}); }));

        // ---- sources ----
        s.Get("/api/sources", guard([this](const httplib::Request&, httplib::Response& res) {
            auto layout = store.active();
            json status = eng().sourceStatus();
            json configured = json::array();
            for (auto& src : layout->sources) {
                json j = src;
                for (auto& st : status)
                    if (st["id"] == src.id) j["status"] = st;
                configured.push_back(j);
            }
            sendJson(res, {{"sources", configured},
                           {"ndi", {{"available", eng().ndiAvailable()}, {"discovered", eng().discoveredNdiSources()}}}});
        }));
        s.Post("/api/sources", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            SourceConfig created;
            std::vector<std::string> problems;
            store.mutateActive(
                [&](Layout& l) {
                    SourceConfig src;
                    src.rect = {0, 0, l.canvas.width, l.canvas.height};
                    from_json(body, src);
                    if (src.id.empty() || l.findSource(sanitizeId(src.id))) src.id = uniqueId(l, "source-", false);
                    l.sources.push_back(src);
                    return true;
                },
                &problems);
            created = store.active()->sources.back();
            sendJson(res, withProblems(created, problems), 201);
        }));
        s.Patch(R"(/api/sources/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            std::vector<std::string> problems;
            bool ok = store.mutateActive(
                [&](Layout& l) {
                    for (auto& src : l.sources)
                        if (src.id == id) {
                            from_json(body, src);
                            src.id = id;
                            return true;
                        }
                    return false;
                },
                &problems);
            if (!ok) return sendError(res, 404, "no such source");
            sendJson(res, withProblems(*store.active()->findSource(id), problems));
        }));
        // A video source from its first frame again.
        s.Post(R"(/api/sources/([A-Za-z0-9_\-]+)/restart)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            const SourceConfig* src = store.active()->findSource(id);
            if (!src) return sendError(res, 404, "no such source");
            if (src->type != SourceType::Video) return sendError(res, 400, "not a video source");
            eng().restartVideo(id);
            sendJson(res, {{"restarted", id}});
        }));
        s.Delete(R"(/api/sources/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            bool ok = store.mutateActive([&](Layout& l) {
                auto n = l.sources.size();
                std::erase_if(l.sources, [&](auto& x) { return x.id == id; });
                for (auto& o : l.outputs)
                    if (o.source == id) o.source = "canvas";
                return l.sources.size() != n;
            });
            if (!ok) return sendError(res, 404, "no such source");
            sendJson(res, {{"deleted", id}});
        }));

        // ---- outputs ----
        s.Get("/api/outputs", guard([this](const httplib::Request&, httplib::Response& res) {
            sendJson(res, {{"outputs", store.active()->outputs}});
        }));
        s.Post("/api/outputs", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            std::string newId;
            std::vector<std::string> problems;
            store.mutateActive(
                [&](Layout& l) {
                    OutputConfig o;
                    o.crop = {0, 0, std::min(1920, l.canvas.width), std::min(1080, l.canvas.height)};
                    from_json(body, o);
                    o.id = sanitizeId(o.id);
                    if (o.id.empty() || l.findOutput(o.id)) o.id = uniqueId(l, "out-", true);
                    newId = o.id;
                    l.outputs.push_back(o);
                    return true;
                },
                &problems);
            sendJson(res, withProblems(*store.active()->findOutput(newId), problems), 201);
        }));
        s.Patch(R"(/api/outputs/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            std::vector<std::string> problems;
            bool ok = store.mutateActive(
                [&](Layout& l) {
                    OutputConfig* o = l.findOutput(id);
                    if (!o) return false;
                    patchOutput(*o, body);
                    return true;
                },
                &problems);
            if (!ok) return sendError(res, 404, "no such output");
            sendJson(res, withProblems(*store.active()->findOutput(id), problems));
        }));
        s.Delete(R"(/api/outputs/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            bool ok = store.mutateActive([&](Layout& l) {
                auto n = l.outputs.size();
                std::erase_if(l.outputs, [&](auto& o) { return o.id == id; });
                return l.outputs.size() != n;
            });
            if (!ok) return sendError(res, 404, "no such output");
            sendJson(res, {{"deleted", id}});
        }));
        s.Get(R"(/api/outputs/([A-Za-z0-9_\-]+)/snapshot\.jpg)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string jpeg;
            uint64_t frameId = 0;
            if (!eng().snapshotJpeg(req.matches[1], jpeg, frameId))
                return sendError(res, 503, "no frame (output missing, disabled or not sending)");
            res.set_header("X-Frame-Id", std::to_string(frameId));
            res.set_header("Cache-Control", "no-store");
            res.set_content(jpeg, "image/jpeg");
        }));

        s.Get(R"(/api/outputs/([A-Za-z0-9_\-]+)/frame\.bgra)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::vector<uint8_t> bgra;
            int w = 0, h = 0;
            uint64_t frameId = 0;
            if (!eng().snapshotBgra(req.matches[1], bgra, w, h, frameId))
                return sendError(res, 503, "no frame (output missing, disabled or not sending)");
            res.set_header("X-Width", std::to_string(w));
            res.set_header("X-Height", std::to_string(h));
            res.set_header("X-Frame-Id", std::to_string(frameId));
            res.set_header("Cache-Control", "no-store");
            res.set_content(std::string(bgra.begin(), bgra.end()), "application/octet-stream");
        }));

        // ---- previews (low-res, ~10 fps while requested) ----
        s.Get(R"(/api/preview/([A-Za-z0-9_\-]+)\.jpg)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string jpeg;
            if (!eng().previewJpeg(req.matches[1], jpeg)) {
                res.status = 202;  // first request starts the preview; try again shortly
                return;
            }
            res.set_header("Cache-Control", "no-store");
            res.set_content(jpeg, "image/jpeg");
        }));

        // ---- wall groups: grids of panels with bezel gaps (1 x 1 for a single display) ----
        s.Get("/api/walls", guard([this](const httplib::Request&, httplib::Response& res) {
            sendJson(res, {{"walls", store.active()->walls}});
        }));
        // Creates a group; its id comes from the body's id or name and is made unique.
        s.Post("/api/walls", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            std::vector<std::string> problems;
            std::string newId;
            store.mutateActive(
                [&](Layout& l) {
                    WallConfig w;
                    w.id.clear();
                    w.fitCanvas = false;
                    from_json(body, w);
                    std::string base = w.id.empty() ? idFromName(w.name) : sanitizeId(w.id);
                    if (base.empty()) base = "group";
                    newId = base;
                    for (int k = 2; l.findWall(newId); ++k) newId = base + "-" + std::to_string(k);
                    w.id = newId;
                    applyWall(l, w);
                    return true;
                },
                &problems);
            sendJson(res, withProblems(*store.active(), problems), 201);
        }));
        // Creates or regenerates a group (its outputs keep their NDI names).
        s.Put(R"(/api/walls/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            std::vector<std::string> problems;
            store.mutateActive(
                [&](Layout& l) {
                    const WallConfig* cur = l.findWall(id);
                    WallConfig w = cur ? *cur : WallConfig{};
                    from_json(body, w);
                    w.id = id;
                    applyWall(l, w);
                    return true;
                },
                &problems);
            sendJson(res, withProblems(*store.active(), problems));
        }));
        // Changes a group's name or fallback image without regenerating its outputs.
        s.Patch(R"(/api/walls/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            bool ok = store.mutateActive([&](Layout& l) {
                for (auto& w : l.walls)
                    if (w.id == id) {
                        w.name = body.value("name", w.name);
                        w.fallbackImage = body.value("fallbackImage", w.fallbackImage);
                        return true;
                    }
                return false;
            });
            if (!ok) return sendError(res, 404, "no such wall group");
            sendJson(res, *store.active()->findWall(id));
        }));
        // Moves a whole group without regenerating it: {x, y}.
        s.Post(R"(/api/walls/([A-Za-z0-9_\-]+)/move)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            bool ok = store.mutateActive([&](Layout& l) {
                const WallConfig* w = l.findWall(id);
                return w && moveWall(l, id, body.value("x", w->x), body.value("y", w->y));
            });
            if (!ok) return sendError(res, 404, "no such wall group");
            sendJson(res, *store.active());
        }));
        // Forgets a group. ?outputs=delete also removes its outputs; by default they stay as standalone outputs.
        s.Delete(R"(/api/walls/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            bool drop = req.get_param_value("outputs") == "delete";
            bool ok = store.mutateActive([&](Layout& l) { return removeWall(l, id, drop); });
            if (!ok) return sendError(res, 404, "no such wall group");
            sendJson(res, {{"deleted", id}});
        }));
        // ---- group content: presets per wall group, or one NDI stream on a group ----
        s.Get(R"(/api/walls/([A-Za-z0-9_\-]+)/presets)", guard([this](const httplib::Request& req, httplib::Response& res) {
            auto l = store.active();
            const WallConfig* w = l->findWall(req.matches[1]);
            if (!w) return sendError(res, 404, "no such wall group");
            sendJson(res, {{"group", w->id}, {"activePreset", w->activePreset}, {"presets", w->presets}});
        }));
        // Saves what the group shows now as a preset {name}; the same name overwrites it.
        s.Post(R"(/api/walls/([A-Za-z0-9_\-]+)/presets)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            std::string pid;
            store.mutateActive([&](Layout& l) {
                pid = saveGroupPreset(l, id, body.value("name", ""));
                return !pid.empty();
            });
            if (pid.empty()) return sendError(res, 404, "no such wall group");
            for (auto& p : store.active()->findWall(id)->presets)
                if (p.id == pid) return sendJson(res, p, 201);
            sendError(res, 500, "preset not saved");
        }));
        s.Post(R"(/api/walls/([A-Za-z0-9_\-]+)/presets/([A-Za-z0-9_\-]+)/recall)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1], pid = req.matches[2];
            json body = parseBody(req);
            auto l = store.active();
            const WallConfig* w = l->findWall(id);
            if (!w) return sendError(res, 404, "no such wall group");
            if (std::none_of(w->presets.begin(), w->presets.end(), [&](auto& p) { return p.id == pid; }))
                return sendError(res, 404, "no such preset in group '" + id + "'");
            int ms = body.value("transitionMs", 0);
            if (ms > 0) eng().captureForTransition(ms, wallBounds(*w));
            std::vector<std::string> problems;
            store.mutateActive([&](Layout& lay) { return recallGroupPreset(lay, id, pid); }, &problems);
            sendJson(res, withProblems({{"group", id}, {"recalled", pid}, {"transitionMs", ms}}, problems));
        }));
        s.Delete(R"(/api/walls/([A-Za-z0-9_\-]+)/presets/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1], pid = req.matches[2];
            bool ok = store.mutateActive([&](Layout& l) { return deleteGroupPreset(l, id, pid); });
            if (!ok) return sendError(res, 404, "no such wall group or preset");
            sendJson(res, {{"deleted", pid}});
        }));
        // Fills the group with one NDI stream: {"ndiName": "PC (Camera 1)", "transitionMs": 500}.
        s.Post(R"(/api/walls/([A-Za-z0-9_\-]+)/show)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            std::string ndiName = body.value("ndiName", "");
            if (ndiName.empty()) return sendError(res, 400, "ndiName is required (use /clear to blank a group)");
            const WallConfig* w = store.active()->findWall(id);
            if (!w) return sendError(res, 404, "no such wall group");
            int ms = body.value("transitionMs", 0);
            if (ms > 0) eng().captureForTransition(ms, wallBounds(*w));
            store.mutateActive([&](Layout& l) { return showOnGroup(l, id, ndiName); });
            sendJson(res, {{"group", id}, {"showing", ndiName}, {"transitionMs", ms}});
        }));
        // Restarts every video the group shows, together, from the first frame.
        s.Post(R"(/api/walls/([A-Za-z0-9_\-]+)/restart)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            auto l = store.active();
            if (!l->findWall(id)) return sendError(res, 404, "no such wall group");
            json restarted = json::array();
            for (auto& src : l->sources)
                if (src.group == id && src.type == SourceType::Video) eng().restartVideo(src.id), restarted.push_back(src.id);
            sendJson(res, {{"group", id}, {"restarted", restarted}});
        }));
        // Removes the group's content (it then shows whatever lies under it, usually the background).
        s.Post(R"(/api/walls/([A-Za-z0-9_\-]+)/clear)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            const WallConfig* w = store.active()->findWall(id);
            if (!w) return sendError(res, 404, "no such wall group");
            int ms = body.value("transitionMs", 0);
            if (ms > 0) eng().captureForTransition(ms, wallBounds(*w));
            store.mutateActive([&](Layout& l) { return showOnGroup(l, id, ""); });
            sendJson(res, {{"group", id}, {"cleared", true}, {"transitionMs", ms}});
        }));

        // ---- Mosaic Receiver distribution ----
        s.Get("/api/receiver/package", guard([this](const httplib::Request&, httplib::Response& res) { sendJson(res, receiverPackage()); }));
        s.Get(R"(/api/receiver/files/([A-Za-z0-9_.\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string name = req.matches[1];
            auto& names = receiverFileNames();
            if (std::find(names.begin(), names.end(), name) == names.end()) return sendError(res, 404, "not a receiver file");
            auto path = binDir / name;
            std::error_code ec;
            auto size = std::filesystem::file_size(path, ec);
            if (ec) return sendError(res, 404, "the receiver isn't built on this machine");
            auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
            res.set_content_provider(static_cast<size_t>(size), "application/octet-stream", [in](size_t offset, size_t length, httplib::DataSink& sink) {
                std::vector<char> buf(std::min<size_t>(length, 1 << 20));
                in->seekg(static_cast<std::streamoff>(offset));
                in->read(buf.data(), static_cast<std::streamsize>(buf.size()));
                return sink.write(buf.data(), static_cast<size_t>(in->gcount()));
            });
        }));

        // ---- media: image files for image sources (data/media) ----
        s.Get("/api/media", guard([this](const httplib::Request&, httplib::Response& res) {
            json files = json::array();
            std::error_code ec;
            for (auto& e : std::filesystem::directory_iterator(canvases.mediaDir(), ec)) {
                std::string name = utf8(e.path().filename());
                if (e.is_regular_file(ec) && !mediaKind(name).empty())
                    files.push_back({{"name", name}, {"bytes", e.file_size(ec)}, {"kind", mediaKind(name)}});
            }
            std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a["name"] < b["name"]; });
            sendJson(res, {{"files", files}, {"folder", utf8(std::filesystem::absolute(canvases.mediaDir()))}});
        }));
        // Upload: the raw file as the body, ?name=clip.mp4. Written to disk as it arrives, so
        // multi-gigabyte (8K) videos don't need the memory. The same name replaces the file.
        s.Post("/api/media", [this](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& content) {
            std::string name = mediaName(req.get_param_value("name"));
            if (name.empty()) return sendError(res, 400, "name must be an image (.png .jpg .bmp .gif) or video (.mp4 .mov .m4v) file name");
            auto path = canvases.mediaDir() / name;
            auto tmp = path;
            tmp += ".part";
            uint64_t bytes = 0;
            const uint64_t limit = isVideoName(name) ? UINT64_MAX : 64ull * 1024 * 1024;
            bool ok = true;
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out) return sendError(res, 500, "cannot write to the media folder");
                ok = content([&](const char* data, size_t len) {
                    bytes += len;
                    if (bytes > limit) return false;
                    out.write(data, static_cast<std::streamsize>(len));
                    return static_cast<bool>(out);
                });
                ok = ok && static_cast<bool>(out);
            }
            std::error_code ec;
            if (!ok || bytes == 0) {
                std::filesystem::remove(tmp, ec);
                return sendError(res, bytes > limit ? 413 : 400, bytes > limit ? "images can be at most 64 MB" : "the upload was cut off or empty");
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec) return sendError(res, 500, ec.message());
            sendJson(res, {{"name", name}, {"bytes", bytes}, {"kind", isVideoName(name) ? "video" : "image"}}, 201);
        });
        // The file itself, streamed from disk (videos can be gigabytes).
        s.Get(R"(/api/media/([^/]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string name = req.matches[1];
            if (!safeMediaName(name)) return sendError(res, 404, "no such file");
            auto path = canvases.mediaDir() / pathFromUtf8(name);
            std::error_code ec;
            auto size = std::filesystem::file_size(path, ec);
            if (name.empty() || ec) return sendError(res, 404, "no such file");
            auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
            res.set_content_provider(static_cast<size_t>(size), mediaType(name), [in](size_t offset, size_t length, httplib::DataSink& sink) {
                std::vector<char> buf(std::min<size_t>(length, 1 << 20));
                in->seekg(static_cast<std::streamoff>(offset));
                in->read(buf.data(), static_cast<std::streamsize>(buf.size()));
                return sink.write(buf.data(), static_cast<size_t>(in->gcount()));
            });
        }));
        s.Delete(R"(/api/media/([^/]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string name = req.matches[1];
            std::error_code ec;
            if (!safeMediaName(name) || !std::filesystem::remove(canvases.mediaDir() / pathFromUtf8(name), ec)) return sendError(res, 404, "no such file");
            sendJson(res, {{"deleted", name}});
        }));

        // ---- endpoints: receiving PCs and which output goes on each of their screens ----
        // How far apart this canvas's receiving PCs show the same frames: each Mosaic Receiver
        // reports when recent frames appeared on its first display, on its estimate of the
        // engine's clock (so the PCs' own clocks don't matter, within the network's fastest
        // transit). Median and worst spread over the frames every PC reported, and how far each
        // PC is from the group's average (positive = later).
        auto groupSync = [this](const Layout& l) {
            std::vector<std::pair<std::string, std::map<uint64_t, int64_t>>> pcs;
            {
                std::lock_guard lock(checkinMutex);
                double now = nowSeconds();
                for (auto& e : l.endpoints) {
                    auto it = checkins.find(hostKey(e.host));
                    if (it == checkins.end() || now - it->second.value("time", 0.0) > kEndpointOnlineSeconds) continue;
                    std::map<uint64_t, int64_t> shown;
                    for (auto& p : it->second.value("receiver", json::object()).value("shownAtEngine", json::array()))
                        if (p.is_array() && p.size() == 2) shown[p[0].get<uint64_t>()] = p[1].get<int64_t>();
                    if (!shown.empty()) pcs.emplace_back(e.name.empty() ? e.host : e.name, std::move(shown));
                }
            }
            json out = {{"pcs", pcs.size()}};
            if (pcs.size() < 2) return out;
            std::vector<double> spreads;
            std::vector<double> offsetSum(pcs.size(), 0);
            for (auto& [fid, t0] : pcs[0].second) {
                std::vector<int64_t> ts{t0};
                for (size_t k = 1; k < pcs.size(); ++k) {
                    auto f = pcs[k].second.find(fid);
                    if (f == pcs[k].second.end()) break;
                    ts.push_back(f->second);
                }
                if (ts.size() != pcs.size()) continue;
                auto [lo, hi] = std::minmax_element(ts.begin(), ts.end());
                spreads.push_back((*hi - *lo) / 1000.0);
                double mean = 0;
                for (auto t : ts) mean += double(t);
                mean /= double(ts.size());
                for (size_t k = 0; k < ts.size(); ++k) offsetSum[k] += (double(ts[k]) - mean) / 1000.0;
            }
            out["frames"] = spreads.size();
            if (spreads.empty()) return out;
            json per = json::array();
            for (size_t k = 0; k < pcs.size(); ++k) per.push_back({{"name", pcs[k].first}, {"offsetMs", std::round(offsetSum[k] / double(spreads.size()) * 10) / 10}});
            std::vector<double> sorted = spreads;
            std::sort(sorted.begin(), sorted.end());
            out["spreadMs"] = std::round(sorted[sorted.size() / 2] * 10) / 10;
            out["maxMs"] = std::round(sorted.back() * 10) / 10;
            out["perPc"] = per;
            return out;
        };
        s.Get("/api/endpoints", guard([this, groupSync](const httplib::Request&, httplib::Response& res) {
            auto l = store.active();
            auto discovered = eng().discoveredNdiSources();
            json list = json::array(), unassigned = json::array();
            std::set<std::string> known;
            for (auto& e : l->endpoints) {
                list.push_back(endpointView(*l, e, discovered));
                known.insert(hostKey(e.host));
            }
            // PCs set up on other canvases: theirs, so not offered here as new.
            json elsewhere = json::array();
            for (auto& other : store.running()) {
                if (other->id == l->id) continue;
                for (auto& e : other->endpoints)
                    if (known.insert(hostKey(e.host)).second) elsewhere.push_back({{"host", e.host}, {"name", e.name}, {"layout", other->name.empty() ? other->id : other->name}});
            }
            {
                // PCs that checked in recently but aren't set up as endpoints yet.
                std::lock_guard lock(checkinMutex);
                double now = nowSeconds();
                for (auto& [key, c] : checkins)
                    if (!known.count(key) && now - c.value("time", 0.0) < 600)
                        unassigned.push_back({{"host", c.value("host", key)},
                                              {"address", c.value("address", "")},
                                              {"screens", c.value("screens", json::array())},
                                              {"online", now - c.value("time", 0.0) < kEndpointOnlineSeconds}});
            }
            sendJson(res, {{"endpoints", list}, {"unassigned", unassigned}, {"elsewhere", elsewhere}, {"groupSync", groupSync(*l)}, {"engineHost", machine},
                           {"receiverVersion", receiverPackage()["version"]}});
        }));
        s.Post("/api/endpoints", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            std::string newId;
            std::vector<std::string> problems;
            store.mutateActive(
                [&](Layout& l) {
                    Endpoint e;
                    from_json(body, e);
                    std::string base = idFromName(e.name.empty() ? e.host : e.name);
                    if (base.empty()) base = "endpoint";
                    newId = base;
                    for (int k = 2; std::any_of(l.endpoints.begin(), l.endpoints.end(), [&](auto& x) { return x.id == newId; }); ++k)
                        newId = base + "-" + std::to_string(k);
                    e.id = newId;
                    l.endpoints.push_back(e);
                    return true;
                },
                &problems);
            auto l = store.active();
            for (auto& e : l->endpoints)
                if (e.id == newId) return sendJson(res, withProblems(endpointView(*l, e, eng().discoveredNdiSources()), problems), 201);
            sendError(res, 500, "endpoint not saved");
        }));
        // Partial update: {name, host, displays}.
        s.Put(R"(/api/endpoints/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            bool ok = store.mutateActive([&](Layout& l) {
                for (auto& e : l.endpoints)
                    if (e.id == id) {
                        from_json(body, e);
                        e.id = id;
                        return true;
                    }
                return false;
            });
            if (!ok) return sendError(res, 404, "no such endpoint");
            auto l = store.active();
            for (auto& e : l->endpoints)
                if (e.id == id) return sendJson(res, endpointView(*l, e, eng().discoveredNdiSources()));
            sendError(res, 404, "no such endpoint");
        }));
        s.Delete(R"(/api/endpoints/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            bool ok = store.mutateActive([&](Layout& l) {
                auto n = l.endpoints.size();
                std::erase_if(l.endpoints, [&](auto& e) { return e.id == id; });
                return l.endpoints.size() != n;
            });
            if (!ok) return sendError(res, 404, "no such endpoint");
            sendJson(res, {{"deleted", id}});
        }));
        // A receiving PC's dashboard calls this every few seconds with its screens and what each
        // panel plays: {host, version, screens: [...], panels: [{index, screen, playing, reachable}]}.
        // The reply is its assignment (endpoint null = not set up; it then leaves its panels alone).
        s.Post("/api/endpoints/checkin", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            std::string host = body.value("host", "");
            if (host.empty()) return sendError(res, 400, "host is required");
            body["time"] = nowSeconds();
            body["address"] = req.remote_addr;
            {
                std::lock_guard lock(checkinMutex);
                checkins[hostKey(host)] = body;
            }
            // The PC belongs to the first canvas whose layout lists it (receiver groups per canvas).
            auto running = store.running();
            for (auto& l : running)
                for (auto& e : l->endpoints)
                    if (hostKey(e.host) == hostKey(host))
                        return sendJson(res, {{"endpoint", {{"id", e.id}, {"name", e.name}, {"player", e.player}, {"flashTest", e.flashTest}, {"bufferMs", l->receiverBufferMs}}},
                                              {"layout", l->id},
                                              {"engineHost", machine},
                                              {"receiverPackage", receiverPackage()},
                                              {"update", updateForEndpoints()},
                                              {"displays", endpointDisplays(*l, e, eng().discoveredNdiSources())}});
            sendJson(res, {{"endpoint", nullptr}, {"layout", running.front()->id}, {"engineHost", machine}, {"displays", json::array()},
                           {"receiverPackage", receiverPackage()}, {"update", updateForEndpoints()}});
        }));

        // ---- updates: the media server (git pull + rebuild, update.ps1) and every receiving PC ----
        // GET: versions and the last update's outcome. POST {target: all | receivers | server}:
        // receiving PCs get the request at their next check-in (git pull, restart; the receiver
        // follows from the engine), and ~15 s later, once they have it, the media server updates
        // itself: the engine and editor stop for a minute or two while they rebuild.
        s.Get("/api/system/update", guard([this](const httplib::Request&, httplib::Response& res) {
            json request;
            {
                std::lock_guard lock(updateMutex);
                request = updateRequest;
            }
            json pcs = json::array();
            std::map<std::string, std::string> names;
            for (auto& l : store.running())
                for (auto& e : l->endpoints) names.emplace(hostKey(e.host), e.name);
            {
                std::lock_guard lock(checkinMutex);
                double now = nowSeconds();
                for (auto& [key, c] : checkins) {
                    if (now - c.value("time", 0.0) > 3600) continue;
                    pcs.push_back({{"host", c.value("host", key)},
                                   {"name", names.count(key) ? names[key] : c.value("host", key)},
                                   {"online", now - c.value("time", 0.0) < kEndpointOnlineSeconds},
                                   {"version", c.value("appVersion", "")},
                                   {"receiverVersion", c.value("receiverVersion", "")},
                                   {"update", c.value("update", json::object())}});
                }
            }
            sendJson(res, {{"server", {{"host", machine}, {"version", gitVersion(appDir)}, {"lastUpdate", lastUpdateResult(appDir)}, {"canUpdate", !appDir.empty()}}},
                           {"receiverVersion", receiverPackage()["version"]},
                           {"request", request},
                           {"endpoints", pcs},
                           {"now", nowSeconds()}});
        }));
        s.Post("/api/system/update", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            std::string target = body.value("target", "all");
            if (target != "all" && target != "receivers" && target != "server") return sendError(res, 400, "target is all, receivers or server");
            bool server = target != "receivers";
            if (server && appDir.empty()) return sendError(res, 409, "can't update the media server: the engine isn't running from the video-wall-control folder");
            json reply;
            {
                std::lock_guard lock(updateMutex);
                updateRequest = {{"id", updateRequest.value("id", 0) + 1}, {"requestedAt", nowSeconds()}, {"target", target}};
                saveUpdateRequest();
                reply = updateRequest;
            }
            if (server) {
                int delay = target == "all" ? 15 : 1;
                reply["serverStartsIn"] = delay;
                std::thread([this, delay] {
                    std::this_thread::sleep_for(std::chrono::seconds(delay));
                    std::string error;
                    if (!startServerUpdate(appDir, error)) {
                        std::cerr << "[update] " << error << "\n";
                        std::lock_guard lock(updateMutex);
                        updateRequest["serverError"] = error;
                        saveUpdateRequest();
                    } else {
                        std::cerr << "[update] update.ps1 started; the engine stops for the rebuild\n";
                    }
                }).detach();
            }
            sendJson(res, reply, 202);
        }));

        // ---- one-call summary for control systems ----
        s.Get("/api/control", guard([this](const httplib::Request&, httplib::Response& res) {
            auto l = store.active();
            std::set<std::string> lost;  // source ids whose NDI stream has no signal
            for (auto& st : eng().sourceStatus())
                if (st.value("signal", "") == "lost") lost.insert(st.value("id", ""));
            json groups = json::array();
            for (auto& w : l->walls) {
                json presets = json::array(), showing = json::array(), noSignal = json::array();
                for (auto& p : w.presets) presets.push_back({{"id", p.id}, {"name", p.name}});
                for (auto& src : l->sources)
                    if (src.group == w.id) {
                        showing.push_back({{"name", src.name}, {"type", toString(src.type)}, {"ndiName", src.ndiName}, {"signal", lost.count(src.id) ? "lost" : "ok"}});
                        if (lost.count(src.id)) noSignal.push_back(src.name);
                    }
                groups.push_back({{"id", w.id},
                                  {"name", w.name},
                                  {"displays", w.cols * w.rows},
                                  {"activePreset", w.activePreset},
                                  {"presets", presets},
                                  {"showing", showing},
                                  {"noSignal", noSignal}});
            }
            json scenes = json::array(), layouts = json::array();
            for (auto& sc : l->scenes) scenes.push_back({{"id", sc.id}, {"name", sc.name}});
            for (auto& x : store.list()) layouts.push_back({{"id", x.id}, {"name", x.name}, {"active", x.active}});
            json endpoints = json::array();
            auto discovered = eng().discoveredNdiSources();
            for (auto& e : l->endpoints) {
                json v = endpointView(*l, e, discovered);
                int ok = 0;
                for (auto& d : v["displays"]) ok += d.value("ok", false) ? 1 : 0;
                json ep = {{"id", e.id}, {"name", e.name}, {"player", e.player}, {"online", v["online"]}, {"displays", e.displays.size()}, {"displaysOk", ok}};
                if (v.contains("receiver")) {
                    ep["syncSpreadFrames"] = v["receiver"]["spreadMax10s"];
                    ep["screenSpreadMaxMs"] = v["receiver"]["screenSpreadMaxMs"];
                }
                endpoints.push_back(ep);
            }
            sendJson(res, {{"layout", {{"id", l->id}, {"name", l->name}}},
                           {"groups", groups},
                           {"endpoints", endpoints},
                           {"scenes", scenes},
                           {"layouts", layouts},
                           {"ndi", {{"available", eng().ndiAvailable()}, {"streams", eng().discoveredNdiSources()}}}});
        }));

        // The single-wall API from before groups: the group "wall".
        s.Get("/api/wall", guard([this](const httplib::Request&, httplib::Response& res) {
            auto l = store.active();
            const WallConfig* w = l->findWall("wall");
            sendJson(res, w ? json(*w) : json(nullptr));
        }));
        s.Put("/api/wall", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            std::vector<std::string> problems;
            store.mutateActive(
                [&](Layout& l) {
                    const WallConfig* cur = l.findWall("wall");
                    WallConfig w = cur ? *cur : WallConfig{};
                    from_json(body, w);
                    w.id = "wall";
                    applyWall(l, w);
                    return true;
                },
                &problems);
            sendJson(res, withProblems(*store.active(), problems));
        }));
        s.Delete("/api/wall", guard([this](const httplib::Request&, httplib::Response& res) {
            store.mutateActive([&](Layout& l) { return removeWall(l, "wall", false); });
            sendJson(res, {{"deleted", "wall"}});
        }));

        // ---- scenes: saved source arrangements, recalled with an optional crossfade ----
        s.Get("/api/scenes", guard([this](const httplib::Request&, httplib::Response& res) {
            sendJson(res, {{"scenes", store.active()->scenes}});
        }));
        s.Post("/api/scenes", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            std::string id;
            store.mutateActive([&](Layout& l) {
                id = saveScene(l, body.value("name", ""));
                return true;
            });
            for (auto& sc : store.active()->scenes)
                if (sc.id == id) return sendJson(res, sc, 201);
            sendError(res, 500, "scene not saved");
        }));
        s.Post(R"(/api/scenes/([A-Za-z0-9_\-]+)/recall)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            auto l = store.active();
            if (std::none_of(l->scenes.begin(), l->scenes.end(), [&](auto& sc) { return sc.id == id; }))
                return sendError(res, 404, "no such scene");
            int ms = body.value("transitionMs", 0);
            if (ms > 0) eng().captureForTransition(ms);
            std::vector<std::string> problems;
            store.mutateActive([&](Layout& lay) { return recallScene(lay, id); }, &problems);
            sendJson(res, withProblems({{"recalled", id}, {"transitionMs", ms}}, problems));
        }));
        s.Delete(R"(/api/scenes/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            bool ok = store.mutateActive([&](Layout& l) {
                auto n = l.scenes.size();
                std::erase_if(l.scenes, [&](auto& sc) { return sc.id == id; });
                return l.scenes.size() != n;
            });
            if (!ok) return sendError(res, 404, "no such scene");
            sendJson(res, {{"deleted", id}});
        }));

        // ---- active layout as a whole ----
        s.Get("/api/layout", guard([this](const httplib::Request&, httplib::Response& res) { sendJson(res, *store.active()); }));
        s.Put("/api/layout", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            std::vector<std::string> problems;
            store.mutateActive(
                [&](Layout& l) {
                    std::string id = l.id;
                    from_json(body, l);
                    l.id = id;
                    return true;
                },
                &problems);
            sendJson(res, withProblems(*store.active(), problems));
        }));

        // ---- canvases: layouts running at once, each on its own render thread ----
        // GET: every canvas (primary first) with its layout, GPU and frame rate, and which one this
        // request is about. POST {name}: a new canvas with a new, empty layout (its own NDI names),
        // or {layout}: run a saved layout as another canvas. DELETE: stop a canvas (its layout stays).
        s.Get("/api/canvases", guard([this](const httplib::Request&, httplib::Response& res) {
            json list = json::array();
            std::string current = store.currentCanvas();
            bool first = true;
            for (auto& [id, run] : canvases.all()) {
                auto l = store.snapshot(id);
                if (!l) continue;
                json c = {{"id", id}, {"layout", l->id}, {"name", l->name}, {"primary", first}, {"current", id == current},
                          {"outputs", l->outputs.size()}, {"endpoints", l->endpoints.size()}, {"frameRate", l->frameRate},
                          {"canvas", {{"width", l->canvas.width}, {"height", l->canvas.height}}}};
                c["running"] = run && run->engine;
                c["gpu"] = run && run->renderer ? run->renderer->deviceName() : "";
                c["gpuIndex"] = run ? run->gpu : -1;
                c["gpuWanted"] = l->gpu;
                c["error"] = !run ? "starting" : run->engine && run->engine->failed() ? run->engine->failure() + " (starting again)" : run->error;
                c["restarts"] = run ? run->restarts : 0;
                if (run && run->engine) c["renderFps"] = run->engine->metrics()["engine"].value("renderFps", 0.0);
                list.push_back(c);
                first = false;
            }
            sendJson(res, {{"canvases", list}, {"current", current}, {"problems", canvases.problems()}});
        }));
        // Video decoders Windows has, per codec, and which are hardware (GPU). Finds out why a
        // video decodes in software (e.g. the AV1 / HEVC Video Extension missing or GPU-less).
        s.Get("/api/system/decoders", guard([](const httplib::Request&, httplib::Response& res) {
            json list = json::array();
            for (auto& d : listVideoDecoders()) list.push_back({{"codec", d.codec}, {"name", d.name}, {"hardware", d.hardware}});
            sendJson(res, {{"decoders", list}});
        }));
        // Capture devices (capture cards) on this PC, and the ones open now (shared by every canvas
        // showing them). A source {"type": "capture", "device": name} shows one.
        s.Get("/api/capture", guard([](const httplib::Request&, httplib::Response& res) {
            std::string error;
            json devices = json::array(), open = json::array();
            for (auto& d : listCaptureDevices(error)) devices.push_back({{"name", d.name}, {"kind", d.kind}});
            for (auto& c : captureStatus())
                open.push_back({{"name", c.name}, {"width", c.width}, {"height", c.height}, {"fps", c.fps}, {"viewers", c.viewers}, {"error", c.error}});
            sendJson(res, {{"devices", devices}, {"open", open}, {"error", error}});
        }));
        // This PC's GPUs and the monitors on each (for a canvas's GPU and its display outputs).
        s.Get("/api/gpus", guard([this](const httplib::Request&, httplib::Response& res) {
            json list = json::array();
            for (auto& g : canvases.gpus()) {
                json outs = json::array();
                for (auto& o : g.outputs) {
                    json modes = json::array();
                    for (auto& m : o.modes) modes.push_back({{"width", m.w}, {"height", m.h}, {"refreshHz", m.hz}});
                    outs.push_back({{"name", o.name}, {"x", o.x}, {"y", o.y}, {"width", o.w}, {"height", o.h}, {"refreshHz", o.refreshHz}, {"primary", o.primary},
                                    {"modes", modes}});
                }
                json canvasesOn = json::array();
                for (auto& [id, run] : canvases.all())
                    if (run && run->gpu == g.index) canvasesOn.push_back(id);
                list.push_back({{"index", g.index}, {"name", g.name}, {"memoryMB", g.memoryMB}, {"outputs", outs}, {"canvases", canvasesOn}});
            }
            sendJson(res, {{"gpus", list}});
        }));
        s.Post("/api/canvases", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            std::string layoutId = body.value("layout", "");
            if (layoutId.empty()) {
                std::string name = body.value("name", "");
                if (name.empty()) return sendError(res, 400, "a new canvas needs a name (or the layout to run)");
                Layout l;
                l.name = name;
                std::string base = idFromName(name);
                if (base.empty()) base = "canvas";
                l.id = base;
                for (int k = 2; store.get(l.id); ++k) l.id = base + "-" + std::to_string(k);
                // Its own NDI names: "Mosaic 2" sends Mosaic_2_01, Mosaic_2_02, ...
                l.ndiPrefix = name;
                auto cur = store.active();
                l.frameRate = cur->frameRate;
                l.canvas = cur->canvas;
                SourceConfig pattern;
                pattern.id = "pattern";
                pattern.name = "Test pattern";
                pattern.rect = {0, 0, l.canvas.width, l.canvas.height};
                l.sources.push_back(pattern);
                store.save(l);
                layoutId = l.id;
            } else if (!store.get(layoutId)) {
                return sendError(res, 404, "no such layout");
            }
            std::string id = store.startCanvas(layoutId);
            if (id.empty()) return sendError(res, 409, "that layout is already on a canvas");
            canvases.sync();
            sendJson(res, {{"id", id}, {"layout", layoutId}, {"problems", canvases.problems()}}, 201);
        }));
        // Tests: the canvas's render thread fails as if its GPU device was lost; it starts again.
        s.Post(R"(/api/canvases/([A-Za-z0-9_\-]+)/simulate-gpu-loss)", guard([this](const httplib::Request& req, httplib::Response& res) {
            auto run = canvases.get(req.matches[1]);
            if (!run || !run->engine) return sendError(res, 404, "no such running canvas");
            run->engine->simulateFailure();
            sendJson(res, {{"simulated", std::string(req.matches[1])}});
        }));
        s.Delete(R"(/api/canvases/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            if (!store.snapshot(id)) return sendError(res, 404, "no such canvas");
            if (!store.stopCanvas(id)) return sendError(res, 409, "the last canvas can't be stopped");
            tlRun.reset();
            canvases.sync();
            sendJson(res, {{"stopped", id}});
        }));

        // ---- saved layouts ----
        s.Get("/api/layouts", guard([this](const httplib::Request&, httplib::Response& res) {
            json list = json::array();
            for (auto& l : store.list())
                list.push_back({{"id", l.id}, {"name", l.name}, {"outputs", l.outputs}, {"active", l.active}, {"running", l.running}});
            sendJson(res, {{"layouts", list}});
        }));
        // ?download=1 sends it as a file (export); POST /api/layouts with the file imports it.
        s.Get(R"(/api/layouts/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            auto l = store.get(req.matches[1]);
            if (!l) return sendError(res, 404, "no such layout");
            sendJson(res, *l);
            if (req.has_param("download"))
                res.set_header("Content-Disposition", "attachment; filename=\"" + l->id + ".json\"");
        }));
        s.Get(R"(/api/layouts/([A-Za-z0-9_\-]+)/history)", guard([this](const httplib::Request& req, httplib::Response& res) {
            json list = json::array();
            for (auto& v : store.history(req.matches[1]))
                list.push_back({{"version", v.version}, {"time", v.time}, {"name", v.name}, {"outputs", v.outputs}});
            sendJson(res, {{"layout", std::string(req.matches[1])}, {"history", list}});
        }));
        s.Post(R"(/api/layouts/([A-Za-z0-9_\-]+)/history/([A-Za-z0-9_\-]+)/restore)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::vector<std::string> problems;
            if (!store.restore(req.matches[1], req.matches[2], &problems)) return sendError(res, 404, "no such layout version");
            sendJson(res, withProblems({{"restored", std::string(req.matches[2])}, {"layout", std::string(req.matches[1])}}, problems));
        }));
        // Body: a full layout, or {"id","name","fromActive":true} to save a copy of the active one.
        s.Post("/api/layouts", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            Layout l;
            if (body.value("fromActive", false)) {
                l = *store.active();
                l.id = body.value("id", "");
                l.name = body.value("name", "");
                if (l.id.empty()) l.id = l.name;
            } else {
                l = body.get<Layout>();
            }
            if (sanitizeId(l.id.empty() ? l.name : l.id).empty()) return sendError(res, 400, "layout needs an id or name");
            auto problems = store.save(l);
            normalize(l);
            sendJson(res, withProblems(*store.get(l.id), problems), 201);
        }));
        // Optional body {"transitionMs": N} crossfades (only when the canvas size stays the same).
        s.Post(R"(/api/layouts/([A-Za-z0-9_\-]+)/activate)", guard([this](const httplib::Request& req, httplib::Response& res) {
            if (!store.get(req.matches[1])) return sendError(res, 404, "no such layout");
            int ms = parseBody(req).value("transitionMs", 0);
            bool elsewhere = false;
            for (auto& x : store.list())
                if (x.id == std::string(req.matches[1]) && x.running && !x.active) elsewhere = true;
            if (elsewhere) return sendError(res, 409, "that layout is on another canvas: switch to that canvas instead");
            if (ms > 0) eng().captureForTransition(ms);
            if (!store.activate(req.matches[1], &elsewhere)) return sendError(res, elsewhere ? 409 : 404, elsewhere ? "that layout is on another canvas" : "no such layout");
            sendJson(res, {{"active", std::string(req.matches[1])}});
        }));
        s.Delete(R"(/api/layouts/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            if (!store.remove(req.matches[1])) return sendError(res, 409, "layout not found or currently active");
            sendJson(res, {{"deleted", std::string(req.matches[1])}});
        }));

        // ---- metrics ----
        s.Get("/api/metrics", guard([this](const httplib::Request&, httplib::Response& res) { sendJson(res, eng().metrics()); }));
        // Sound outputs: the PC's sound devices, and each output's level, delay and problems.
        // The outputs themselves are the layout's `audioOutputs` (PUT /api/layout).
        s.Get("/api/audio", guard([this](const httplib::Request&, httplib::Response& res) {
            std::string error;
            json devices = json::array();
            for (auto& d : listAudioDevices(error))
                devices.push_back({{"id", d.id}, {"name", d.name}, {"channels", d.channels}, {"rate", d.rate}, {"default", d.isDefault}});
            json out = eng().audioStatus();
            out["devices"] = devices;
            out["devicesError"] = error;
            // Sound inputs (a capture card's audio, a line in): an output plays one as source "input:<id>".
            std::string inError;
            json inputs = json::array();
            for (auto& d : listAudioInputs(inError)) inputs.push_back({{"id", d.id}, {"name", d.name}, {"channels", d.channels}, {"rate", d.rate}});
            out["inputs"] = inputs;
            out["inputsError"] = inError;
            out["ndiSources"] = eng().discoveredNdiSources();
            sendJson(res, out);
        }));

        // Server-sent events: metrics + layout version every 500 ms.
        s.Get("/api/events", [this](const httplib::Request&, httplib::Response& res) {
            res.set_header("Cache-Control", "no-store");
            std::string canvas = store.currentCanvas();
            res.set_chunked_content_provider("text/event-stream", [this, canvas](size_t, httplib::DataSink& sink) {
                while (running && sink.is_writable()) {
                    auto run = canvases.get(canvas);
                    json ev = {{"metrics", run && run->engine ? run->engine->metrics() : json::object()}, {"layoutVersion", store.version()}, {"canvas", canvas}};
                    std::string msg = "data: " + ev.dump() + "\n\n";
                    if (!sink.write(msg.data(), msg.size())) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                sink.done();
                return true;
            });
        });
    }
};

HttpApi::HttpApi(Canvases& canvases, LayoutStore& store, std::filesystem::path uiDir, std::filesystem::path binDir)
    : impl_(std::make_unique<Impl>(canvases, store)) {
    impl_->binDir = std::move(binDir);
    impl_->appDir = findAppDir(impl_->binDir.empty() ? std::filesystem::current_path() : impl_->binDir);
    impl_->loadUpdateRequest();
    impl_->server.new_task_queue = [] { return new httplib::ThreadPool(24); };
    impl_->routes();
    if (std::filesystem::is_directory(uiDir)) impl_->server.set_mount_point("/", uiDir.string());
}

HttpApi::~HttpApi() = default;

bool HttpApi::listen(const std::string& host, int port) { return impl_->server.listen(host, port); }

void HttpApi::stop() {
    impl_->running = false;
    impl_->server.stop();
}

}  // namespace mosaic
