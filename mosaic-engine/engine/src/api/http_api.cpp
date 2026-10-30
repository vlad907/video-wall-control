#include "api/http_api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

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

}  // namespace

struct HttpApi::Impl {
    Engine& engine;
    LayoutStore& store;
    httplib::Server server;
    std::atomic<bool> running{true};

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

        // ---- video wall: a grid of panels with bezel gaps, generating the outputs ----
        s.Get("/api/wall", guard([this](const httplib::Request&, httplib::Response& res) {
            auto l = store.active();
            sendJson(res, l->wall ? json(*l->wall) : json(nullptr));
        }));
        s.Put("/api/wall", guard([this](const httplib::Request& req, httplib::Response& res) {
            json body = parseBody(req);
            std::vector<std::string> problems;
            store.mutateActive(
                [&](Layout& l) {
                    WallConfig w = l.wall.value_or(WallConfig{});
                    from_json(body, w);
                    applyWall(l, w);
                    return true;
                },
                &problems);
            sendJson(res, withProblems(*store.active(), problems));
        }));
        // Forgets the wall definition; its outputs stay as ordinary outputs.
        s.Delete("/api/wall", guard([this](const httplib::Request&, httplib::Response& res) {
            store.mutateActive([&](Layout& l) {
                l.wall.reset();
                for (auto& o : l.outputs) o.group.clear();
                return true;
            });
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
