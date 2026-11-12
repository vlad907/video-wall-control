// Mosaic Receiver: plays engine NDI outputs full screen on a receiving PC's displays, showing
// the same engine frame on all of them each refresh. Normally started and configured by the
// video-wall dashboard (endpoint player "mosaic"); see docs/RECEIVER.md.
//
//   mosaic_receiver [--port 8095] [--host 127.0.0.1] [--config FILE] [--headless] [--software-gpu]
//   mosaic_receiver --self-test [SECONDS]     # synthetic streams, headless; exits non-zero on failure
#include <httplib.h>

#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include "receiver.hpp"
#include "synthetic_input.hpp"

using nlohmann::json;
using namespace mosaic::rx;

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }

// Four synthetic streams arriving differently: on time, late with jitter, dropping frames,
// and one that stalls for half a second. The in-step displays must show the same frame.
int selfTest(double seconds) {
    json cfg = {{"lagFrames", 2}, {"lostAfterMs", 300}, {"displays", json::array()}};
    auto add = [&](int i, json synth) {
        synth["width"] = 64;
        synth["height"] = 36;
        synth["colour"] = 0xFF000000u | (0x404040u * static_cast<unsigned>(i));
        cfg["displays"].push_back({{"index", i}, {"stream", "synthetic"}, {"synthetic", synth},
                                   {"window", {{"x", (i - 1) * 1920}, {"y", 0}, {"w", 1920}, {"h", 1080}}}});
    };
    add(1, json::object());
    add(2, {{"delayMs", 6}, {"jitterMs", 5}});
    add(3, {{"delayMs", 3}, {"dropEvery", 37}});
    add(4, {{"delayMs", 2}, {"stallAtS", seconds * 0.4}, {"stallForS", 0.5}});

    ReceiverConfig c;
    from_json(cfg, c);
    Receiver rx([](std::string&) { return createHeadlessPresenter(60.0); });
    rx.setConfig(c);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    json s = rx.status();
    std::cout << s.dump(2) << "\n";

    bool ok = true;
    auto check = [&](bool cond, const std::string& what) {
        std::cout << (cond ? "  ok   " : "  FAIL ") << what << "\n";
        ok = ok && cond;
    };
    double aligned = s.value("alignedRatio", 0.0);
    check(s.value("presents", 0) > seconds * 50, "presented at ~60 Hz (" + std::to_string(s.value("presents", 0)) + ")");
    // Frames dropped at the sender (display 3) and the stall (display 4) are the only mismatches.
    check(aligned > 0.95, "in-step displays showed the same frame in " + std::to_string(aligned * 100) + "% of refreshes");
    auto& d = s["displays"];
    check(d[0].value("framesShown", 0) > seconds * 50, "the on-time stream kept moving");
    check(d[3].value("late", 0) > 0, "the stalled stream was counted late, not waited for");
    check(d[0].value("shownFrameId", 0) > 0 && d[0].value("shownFrameId", 0) == d[1].value("shownFrameId", 0), "displays 1 and 2 end on the same frame");
    check(s.value("timing", "") == "measured", "on-screen timing is measured");
    check(s.value("spreadMax10s", 99) <= 1, "in-step displays were never more than 1 frame apart (" + std::to_string(s.value("spreadMax10s", 99)) + ")");
    bool stallLogged = false;
    for (auto& f : s.value("freezes", json::array()))
        if (f.value("cause", "") == "network" && f.value("ms", 0.0) > 400 && f.value("ms", 0.0) < 700) stallLogged = true;
    check(stallLogged, "the half-second stall was logged as a network freeze");

    // Sync compensation: like the venue's wall, Windows shows the top two displays one refresh
    // late. The bottom two must be held back one refresh, and then all four show frames together.
    {
        json cfg2 = {{"displays", json::array()}};
        for (int i = 1; i <= 4; ++i)
            cfg2["displays"].push_back({{"index", i}, {"stream", "synthetic"}, {"synthetic", {{"width", 64}, {"height", 36}, {"fps", 30}}},
                                        {"window", {{"x", (i - 1) * 1920}, {"y", 0}, {"w", 1920}, {"h", 1080}}}});
        ReceiverConfig c2;
        from_json(cfg2, c2);
        Receiver rx2([](std::string&) { return createHeadlessPresenter(60.0, {1, 1, 0, 0}); });
        rx2.setConfig(c2);
        std::this_thread::sleep_for(std::chrono::duration<double>(std::max(seconds, 7.0)));
        json s2 = rx2.status();
        auto& d2 = s2["displays"];
        std::cout << "compensation: held " << d2[0].value("heldRefreshes", -1) << " " << d2[1].value("heldRefreshes", -1) << " "
                  << d2[2].value("heldRefreshes", -1) << " " << d2[3].value("heldRefreshes", -1) << ", apart now "
                  << s2.value("screenSpreadNowMs", -1.0) << " ms\n";
        check(d2[0].value("windowsLateMs", 0.0) > 12 && d2[2].value("windowsLateMs", 99.0) < 4, "measured which displays Windows shows a refresh late");
        check(d2[0].value("heldRefreshes", -1) == 0 && d2[1].value("heldRefreshes", -1) == 0 && d2[2].value("heldRefreshes", -1) == 1 &&
                  d2[3].value("heldRefreshes", -1) == 1,
              "held the other displays back one refresh");
        check(s2.value("screenSpreadNowMs", 99.0) < 8, "with compensation all displays show each frame together (under half a refresh apart)");
    }
    // A slow GPU, like the venue's receiving PC: each window's present takes 4.5 ms, so presenting
    // all four every refresh (18 ms) can't keep up. Only displays whose picture changes are
    // presented (the top pair one refresh, the held-back bottom pair the next), so the loop keeps
    // every refresh and the hold-back stays put instead of flipping on and off.
    {
        json cfg4 = {{"displays", json::array()}};
        for (int i = 1; i <= 4; ++i)
            cfg4["displays"].push_back({{"index", i}, {"stream", "synthetic"}, {"synthetic", {{"width", 64}, {"height", 36}, {"fps", 30}}},
                                        {"window", {{"x", (i - 1) * 1920}, {"y", 0}, {"w", 1920}, {"h", 1080}}}});
        ReceiverConfig c4;
        from_json(cfg4, c4);
        Receiver rx4([](std::string&) { return createHeadlessPresenter(60.0, {1, 1, 0, 0}, 4.5); });
        rx4.setConfig(c4);
        // Until the hold-back is measured (~3 s) all four change together and the loop can't keep up: judge after that.
        std::this_thread::sleep_for(std::chrono::seconds(6));
        json before4 = rx4.status();
        std::this_thread::sleep_for(std::chrono::seconds(4));
        json s4 = rx4.status();
        double passes = (s4.value("presents", 0) - before4.value("presents", 0)) / 4.0;
        int freezes4 = s4.value("freezesTotal", 0) - before4.value("freezesTotal", 0);
        auto& d4 = s4["displays"];
        std::cout << "slow GPU: " << passes << " passes/s, " << s4.value("windowPresentsPerS", 0.0) << " window presents/s, held "
                  << d4[0].value("heldRefreshes", -1) << " " << d4[1].value("heldRefreshes", -1) << " " << d4[2].value("heldRefreshes", -1) << " "
                  << d4[3].value("heldRefreshes", -1) << ", changes " << s4.value("compensationChanges", -1) << ", apart now "
                  << s4.value("screenSpreadNowMs", -1.0) << " ms, freezes " << freezes4 << ", deferred " << s4.value("heldDeferred", -1) << "\n";
        check(passes > 55, "the loop kept every refresh though presenting every window would take 18 ms");
        check(s4.value("windowPresentsPerS", 999.0) < 140, "only the displays that changed were presented (about 2 per refresh)");
        check(d4[0].value("heldRefreshes", -1) == 0 && d4[1].value("heldRefreshes", -1) == 0 && d4[2].value("heldRefreshes", -1) == 1 &&
                  d4[3].value("heldRefreshes", -1) == 1,
              "held the bottom displays back one refresh");
        check(s4.value("compensationChanges", 99) <= 2, "the hold-back was set once and stayed (" + std::to_string(s4.value("compensationChanges", 99)) + " changes)");
        check(s4.value("screenSpreadNowMs", 99.0) < 8, "all displays show each frame together on the slow GPU");
        check(freezes4 == 0, "no freezes on the slow GPU once the hold-back is set (" + std::to_string(freezes4) + ")");
    }
    // One stream across a 2 x 2 of screens: each display shows its quarter of the same picture
    // (red, green / blue, white), and all four show the same frame.
    {
        json cfgQ = {{"probe", true}, {"displays", json::array()}};
        json synth = {{"width", 64}, {"height", 36}, {"fps", 30}, {"quadrants", true}};
        for (int i = 1; i <= 4; ++i)
            cfgQ["displays"].push_back({{"index", i}, {"stream", "synthetic"}, {"synthetic", synth},
                                        {"window", {{"x", (i - 1) * 1920}, {"y", 0}, {"w", 1920}, {"h", 1080}}},
                                        {"crop", {{"x", ((i - 1) % 2) * 0.5}, {"y", ((i - 1) / 2) * 0.5}, {"w", 0.5}, {"h", 0.5}}}});
        ReceiverConfig cq;
        from_json(cfgQ, cq);
        Receiver rxq([](std::string&) { return createHeadlessPresenter(60.0); });
        rxq.setConfig(cq);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        json sq = rxq.status();
        std::vector<uint32_t> got = sq.value("probe", std::vector<uint32_t>{});
        const std::vector<uint32_t> want = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
        char buf[96] = "";
        if (got.size() == 4) std::snprintf(buf, sizeof buf, "%06X %06X %06X %06X", got[0], got[1], got[2], got[3]);
        check(got == want, std::string("four displays show the four quarters of one stream (") + buf + ")");
        check(sq.value("spreadMax10s", 99) == 0, "and always the same frame");
    }
    // Buffer: like the venue's congested network, one stream stalls for 150 ms and then its
    // frames arrive all at once. With a 250 ms buffer nobody sees it: every display stays on
    // the same frame and nothing counts as a freeze.
    {
        json cfg3 = {{"bufferMs", 250}, {"displays", json::array()}};
        // Synthetic streams keep one clock from the first one made: the stall is timed on it.
        double soon = std::chrono::duration<double>(Clock::now() - SyntheticInput::epoch()).count();
        for (int i = 1; i <= 4; ++i) {
            json synth = {{"width", 64}, {"height", 36}, {"fps", 30}, {"delayMs", 2}, {"jitterMs", 4}};
            if (i == 4) synth.update({{"stallAtS", soon + 3.0}, {"stallForS", 0.15}, {"stallLate", true}});
            cfg3["displays"].push_back({{"index", i}, {"stream", "synthetic"}, {"synthetic", synth},
                                        {"window", {{"x", (i - 1) * 1920}, {"y", 0}, {"w", 1920}, {"h", 1080}}}});
        }
        ReceiverConfig c3;
        from_json(cfg3, c3);
        Receiver rx3([](std::string&) { return createHeadlessPresenter(60.0); });
        rx3.setConfig(c3);
        // Settle first (the clock estimate and the buffer fill), then count from before the stall.
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        json before = rx3.status();
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        json s3 = rx3.status();
        auto late = [](const json& s, int d) { return s["displays"][d].value("late", 0); };
        int lateDuring = late(s3, 3) - late(before, 3);
        std::cout << "buffer: held " << s3.value("heldMs", 0.0) << " ms, gaps absorbed " << s3.value("gapsAbsorbed", 0) << ", display 4 late "
                  << lateDuring << ", freezes " << s3.value("freezesTotal", 0) << "\n";
        check(s3.value("bufferReady", false), "the buffer found the engine's clock");
        check(s3.value("heldMs", 0.0) > 180 && s3.value("heldMs", 0.0) < 300, "frames were held about the buffer's length");
        check(s3.value("gapsAbsorbed", 0) >= 1, "the 150 ms stall was absorbed by the buffer");
        check(lateDuring == 0, "the stalled display never fell behind (" + std::to_string(lateDuring) + " late)");
        check(s3.value("freezesTotal", 99) == 0, "no freezes");
        // Cross-PC sync check: each frame's show time on the engine's clock. With a 250 ms buffer
        // it's the frame's timecode + 250 ms, plus up to two refreshes (picked on the refresh after
        // it's due, on screen at the one after that) and the test network's jitter.
        std::vector<double> after;
        for (auto& p : s3.value("shownAtEngine", json::array()))
            after.push_back((p[1].get<int64_t>() - p[0].get<int64_t>() * 1000000 / 30) / 1000.0 - 250.0);
        std::sort(after.begin(), after.end());
        double med = after.empty() ? -99 : after[after.size() / 2];
        std::cout << "engine-clock show times: " << after.size() << " frames, median " << med << " ms after due\n";
        check(after.size() > 20 && med > -2 && med < 42, "frames' show times on the engine's clock are their due time (within two refreshes)");
    }
    std::cout << (ok ? "SELF-TEST PASSED" : "SELF-TEST FAILED") << "\n";
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Windows wakes sleeping threads every 15.6 ms by default: too coarse for 60 Hz timing.
    timeBeginPeriod(1);
#endif
    std::string host = "127.0.0.1", configFile;
    int port = 8095;
    bool headless = false, software = std::getenv("MOSAIC_SOFTWARE_GPU") != nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") port = std::stoi(next());
        else if (a == "--host") host = next();
        else if (a == "--config") configFile = next();
        else if (a == "--headless") headless = true;
        else if (a == "--software-gpu") software = true;
        else if (a == "--self-test") return selfTest(i + 1 < argc && argv[i + 1][0] != '-' ? std::stod(next()) : 5.0);
        else {
            std::cout << "usage: mosaic_receiver [--port N] [--host ADDR] [--config FILE] [--headless] [--software-gpu] | --self-test [SECONDS]\n";
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Receiver rx([headless, software](std::string& error) -> std::unique_ptr<Presenter> {
        if (headless) return createHeadlessPresenter();
        auto p = createD3D11Presenter(software, error);
        if (!p) {
            std::cerr << "[receiver] " << error << "; running headless\n";
            return createHeadlessPresenter();
        }
        return p;
    });
    if (!configFile.empty()) {
        try {
            std::ifstream in(configFile);
            ReceiverConfig c;
            from_json(json::parse(in), c);
            rx.setConfig(c);
        } catch (const std::exception& e) {
            std::cerr << "[receiver] cannot read " << configFile << ": " << e.what() << "\n";
        }
    }

    // Local control: the dashboard puts the displays here and reads the status back.
    httplib::Server http;
    auto sendJson = [](httplib::Response& res, const json& j, int code = 200) {
        res.status = code;
        res.set_content(j.dump(), "application/json");
    };
    http.Get("/health", [&](const httplib::Request&, httplib::Response& res) { sendJson(res, {{"ok", true}}); });
    http.Get("/status", [&](const httplib::Request&, httplib::Response& res) { sendJson(res, rx.status()); });
    http.Get("/config", [&](const httplib::Request&, httplib::Response& res) { sendJson(res, to_json(rx.config())); });
    http.Put("/config", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            ReceiverConfig c;
            from_json(json::parse(req.body), c);
            rx.setConfig(c);
            sendJson(res, to_json(c));
        } catch (const std::exception& e) {
            sendJson(res, {{"error", std::string("bad config: ") + e.what()}}, 400);
        }
    });
    std::thread server([&] {
        if (!http.listen(host, port)) {
            std::cerr << "[receiver] cannot listen on " << host << ":" << port << "\n";
            g_quit = true;
        }
    });
    std::cerr << "[receiver] control on http://" << host << ":" << port << "\n";
    while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    http.stop();
    server.join();
    return 0;
}
