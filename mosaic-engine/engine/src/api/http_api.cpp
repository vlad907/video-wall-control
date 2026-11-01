#include "api/http_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

#include "core/engine.hpp"
#include "core/layout_store.hpp"

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

constexpr double kEndpointOnlineSeconds = 20;  // endpoints check in every 5 s

}  // namespace

struct HttpApi::Impl {
    Engine& engine;
    LayoutStore& store;
    httplib::Server server;
    std::atomic<bool> running{true};
    const std::string machine = machineName();

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
        json last;
        {
            std::lock_guard lock(checkinMutex);
            auto it = checkins.find(hostKey(e.host));
            if (it != checkins.end()) last = it->second;
        }
        double ago = last.is_null() ? -1 : nowSeconds() - last.value("time", 0.0);
        j["online"] = ago >= 0 && ago < kEndpointOnlineSeconds;
        j["lastSeenSecondsAgo"] = ago < 0 ? json(nullptr) : json(std::round(ago));
        j["address"] = last.value("address", "");
        j["version"] = last.value("version", "");
        j["screens"] = last.value("screens", json::array());
        json panels = last.value("panels", json::array());
        for (auto& d : displays) {
            // The dashboard numbers its panels like the display slots.
            for (auto& p : panels)
                if (p.value("index", 0) == d["index"]) {
                    d["reachable"] = p.value("reachable", false);
                    d["playing"] = p.value("playing", "");
                    d["onScreen"] = p.value("screen", "");
                }
            d["ok"] = j["online"].get<bool>() && d.value("reachable", false) && d.contains("stream") &&
                      d.value("playing", "") == d.value("stream", "");
        }
        j["displays"] = displays;
        return j;
    }

    Impl(Engine& e, LayoutStore& s) : engine(e), store(s) {}

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

        // ---- sources ----
        s.Get("/api/sources", guard([this](const httplib::Request&, httplib::Response& res) {
            auto layout = store.active();
            json status = engine.sourceStatus();
            json configured = json::array();
            for (auto& src : layout->sources) {
                json j = src;
                for (auto& st : status)
                    if (st["id"] == src.id) j["status"] = st;
                configured.push_back(j);
            }
            sendJson(res, {{"sources", configured},
                           {"ndi", {{"available", engine.ndiAvailable()}, {"discovered", engine.discoveredNdiSources()}}}});
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
            if (!engine.snapshotJpeg(req.matches[1], jpeg, frameId))
                return sendError(res, 503, "no frame (output missing, disabled or not sending)");
            res.set_header("X-Frame-Id", std::to_string(frameId));
            res.set_header("Cache-Control", "no-store");
            res.set_content(jpeg, "image/jpeg");
        }));

        s.Get(R"(/api/outputs/([A-Za-z0-9_\-]+)/frame\.bgra)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::vector<uint8_t> bgra;
            int w = 0, h = 0;
            uint64_t frameId = 0;
            if (!engine.snapshotBgra(req.matches[1], bgra, w, h, frameId))
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
            if (!engine.previewJpeg(req.matches[1], jpeg)) {
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
            if (ms > 0) engine.captureForTransition(ms, wallBounds(*w));
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
            if (ms > 0) engine.captureForTransition(ms, wallBounds(*w));
            store.mutateActive([&](Layout& l) { return showOnGroup(l, id, ndiName); });
            sendJson(res, {{"group", id}, {"showing", ndiName}, {"transitionMs", ms}});
        }));
        // Removes the group's content (it then shows whatever lies under it, usually the background).
        s.Post(R"(/api/walls/([A-Za-z0-9_\-]+)/clear)", guard([this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            json body = parseBody(req);
            const WallConfig* w = store.active()->findWall(id);
            if (!w) return sendError(res, 404, "no such wall group");
            int ms = body.value("transitionMs", 0);
            if (ms > 0) engine.captureForTransition(ms, wallBounds(*w));
            store.mutateActive([&](Layout& l) { return showOnGroup(l, id, ""); });
            sendJson(res, {{"group", id}, {"cleared", true}, {"transitionMs", ms}});
        }));

        // ---- endpoints: receiving PCs and which output goes on each of their screens ----
        s.Get("/api/endpoints", guard([this](const httplib::Request&, httplib::Response& res) {
            auto l = store.active();
            auto discovered = engine.discoveredNdiSources();
            json list = json::array(), unassigned = json::array();
            std::set<std::string> known;
            for (auto& e : l->endpoints) {
                list.push_back(endpointView(*l, e, discovered));
                known.insert(hostKey(e.host));
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
            sendJson(res, {{"endpoints", list}, {"unassigned", unassigned}, {"engineHost", machine}});
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
                if (e.id == newId) return sendJson(res, withProblems(endpointView(*l, e, engine.discoveredNdiSources()), problems), 201);
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
                if (e.id == id) return sendJson(res, endpointView(*l, e, engine.discoveredNdiSources()));
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
            auto l = store.active();
            for (auto& e : l->endpoints)
                if (hostKey(e.host) == hostKey(host))
                    return sendJson(res, {{"endpoint", {{"id", e.id}, {"name", e.name}}},
                                          {"layout", l->id},
                                          {"engineHost", machine},
                                          {"displays", endpointDisplays(*l, e, engine.discoveredNdiSources())}});
            sendJson(res, {{"endpoint", nullptr}, {"layout", l->id}, {"engineHost", machine}, {"displays", json::array()}});
        }));

        // ---- one-call summary for control systems ----
        s.Get("/api/control", guard([this](const httplib::Request&, httplib::Response& res) {
            auto l = store.active();
            json groups = json::array();
            for (auto& w : l->walls) {
                json presets = json::array(), showing = json::array();
                for (auto& p : w.presets) presets.push_back({{"id", p.id}, {"name", p.name}});
                for (auto& src : l->sources)
                    if (src.group == w.id)
                        showing.push_back({{"name", src.name}, {"type", toString(src.type)}, {"ndiName", src.ndiName}});
                groups.push_back({{"id", w.id},
                                  {"name", w.name},
                                  {"displays", w.cols * w.rows},
                                  {"activePreset", w.activePreset},
                                  {"presets", presets},
                                  {"showing", showing}});
            }
            json scenes = json::array(), layouts = json::array();
            for (auto& sc : l->scenes) scenes.push_back({{"id", sc.id}, {"name", sc.name}});
            for (auto& x : store.list()) layouts.push_back({{"id", x.id}, {"name", x.name}, {"active", x.active}});
            json endpoints = json::array();
            auto discovered = engine.discoveredNdiSources();
            for (auto& e : l->endpoints) {
                json v = endpointView(*l, e, discovered);
                int ok = 0;
                for (auto& d : v["displays"]) ok += d.value("ok", false) ? 1 : 0;
                endpoints.push_back({{"id", e.id}, {"name", e.name}, {"online", v["online"]}, {"displays", e.displays.size()}, {"displaysOk", ok}});
            }
            sendJson(res, {{"layout", {{"id", l->id}, {"name", l->name}}},
                           {"groups", groups},
                           {"endpoints", endpoints},
                           {"scenes", scenes},
                           {"layouts", layouts},
                           {"ndi", {{"available", engine.ndiAvailable()}, {"streams", engine.discoveredNdiSources()}}}});
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
            if (ms > 0) engine.captureForTransition(ms);
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

        // ---- saved layouts ----
        s.Get("/api/layouts", guard([this](const httplib::Request&, httplib::Response& res) {
            json list = json::array();
            for (auto& l : store.list())
                list.push_back({{"id", l.id}, {"name", l.name}, {"outputs", l.outputs}, {"active", l.active}});
            sendJson(res, {{"layouts", list}});
        }));
        s.Get(R"(/api/layouts/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            auto l = store.get(req.matches[1]);
            if (!l) return sendError(res, 404, "no such layout");
            sendJson(res, *l);
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
            if (ms > 0) engine.captureForTransition(ms);
            if (!store.activate(req.matches[1])) return sendError(res, 404, "no such layout");
            sendJson(res, {{"active", std::string(req.matches[1])}});
        }));
        s.Delete(R"(/api/layouts/([A-Za-z0-9_\-]+))", guard([this](const httplib::Request& req, httplib::Response& res) {
            if (!store.remove(req.matches[1])) return sendError(res, 409, "layout not found or currently active");
            sendJson(res, {{"deleted", std::string(req.matches[1])}});
        }));

        // ---- metrics ----
        s.Get("/api/metrics", guard([this](const httplib::Request&, httplib::Response& res) { sendJson(res, engine.metrics()); }));

        // Server-sent events: metrics + layout version every 500 ms.
        s.Get("/api/events", [this](const httplib::Request&, httplib::Response& res) {
            res.set_header("Cache-Control", "no-store");
            res.set_chunked_content_provider("text/event-stream", [this](size_t, httplib::DataSink& sink) {
                while (running && sink.is_writable()) {
                    json ev = {{"metrics", engine.metrics()}, {"layoutVersion", store.version()}};
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

HttpApi::HttpApi(Engine& engine, LayoutStore& store, std::filesystem::path uiDir) : impl_(std::make_unique<Impl>(engine, store)) {
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
