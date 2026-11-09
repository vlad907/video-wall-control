// `mosaic_engine --audio-self-test`: the audio ring and playhead against a simulated source
// and network, in simulated time (runs in a moment, no sound hardware needed). Exits non-zero
// on failure; runs in CI.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

#include "audio/audio_ring.hpp"

namespace mosaic {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Sim {
    double sourceRate = 48000;     // nominal
    double sourceDrift = 0;        // the source's clock runs this much fast (e.g. 50e-6)
    double deviceRate = 48000;
    double delay = 0.3;            // wanted, seconds
    double minTransit = 0.002;     // network, seconds
    double jitter = 0.030;         // plus 0..this, random (arrivals only ever late)
    double stallAt = -1, stallFor = 0;  // no audio arrives meanwhile; it all comes at the end
    double seconds = 20;
    double toneHz = 50;
};

struct Result {
    double worstAfterSettle = 0;  // largest |output - expected| once settled
    uint64_t jumps = 0, missing = 0;
    double offByMs = 0;
};

// The source plays a tone; sample i is made at i / (rate * (1 + drift)) and sent in 1024-sample
// blocks. The device asks for 480 frames every 10 ms, heard 20 ms later. Expected output: the
// tone `delay` after the fastest possible arrival.
Result run(const Sim& s) {
    AudioRing ring;
    AudioPlayhead head;
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jit(0, s.jitter);
    const double trueRate = s.sourceRate * (1 + s.sourceDrift);
    const int block = 1024, period = 480;
    std::vector<float> blockL(block), outL(period), outR(period);
    uint64_t made = 0;
    struct Pending {
        double arrive;
        uint64_t first;
    };
    std::vector<Pending> inFlight;
    Result r;
    double nextPeriod = 0, lastArrive = 0;
    uint64_t missingAtSettle = 0;
    bool settled = false;
    for (double t = 0; t < s.seconds; t += 0.0005) {
        // Blocks finished by now leave the source.
        while ((made + block) / trueRate <= t) {
            double arrive = (made + block) / trueRate + s.minTransit + jit(rng);
            if (s.stallAt >= 0 && arrive >= s.stallAt && arrive < s.stallAt + s.stallFor) arrive = s.stallAt + s.stallFor;
            arrive = std::max(arrive, lastArrive);  // NDI delivers in order: a late block holds up the next ones
            lastArrive = arrive;
            inFlight.push_back({arrive, made});
            made += block;
        }
        for (auto it = inFlight.begin(); it != inFlight.end();) {
            if (it->arrive > t) {
                ++it;
                continue;
            }
            for (int i = 0; i < block; ++i) blockL[i] = static_cast<float>(std::sin(2 * kPi * s.toneHz * (it->first + i) / trueRate));
            ring.push(blockL.data(), blockL.data(), block, static_cast<int>(s.sourceRate), t);
            it = inFlight.erase(it);
        }
        if (t < nextPeriod) continue;
        double heardAt = nextPeriod + 0.020;  // a device period starts exactly every 480 frames
        nextPeriod += period / s.deviceRate;
        head.fill(ring, s.deviceRate, heardAt, s.delay, period, outL.data(), outR.data());
        if (t < 12) continue;  // settle: the clock estimate looks back 10 s
        if (!settled) settled = true, missingAtSettle = head.missing();
        for (int k = 0; k < period; ++k) {
            // When the sample heard now was made: heard - delay - the fastest transit.
            double made_at = heardAt + k / s.deviceRate - s.delay - s.minTransit;
            double expected = std::sin(2 * kPi * s.toneHz * made_at);
            r.worstAfterSettle = std::max(r.worstAfterSettle, std::fabs(outL[k] - expected));
        }
    }
    r.jumps = head.jumps();
    r.missing = head.missing() - missingAtSettle;
    r.offByMs = head.errorMs();
    return r;
}

}  // namespace

int audioSelfTest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what, const Result& r) {
        std::printf("%s %-62s (worst %.3f, jumps %llu, silent %llu samples, off %.2f ms)\n", cond ? "  ok  " : "  FAIL", what, r.worstAfterSettle,
                    static_cast<unsigned long long>(r.jumps), static_cast<unsigned long long>(r.missing), r.offByMs);
        ok = ok && cond;
    };
    // Tolerance: 0.1 of full scale at 50 Hz is about 0.3 ms of timing error (sound travels 10 cm in that).
    Sim base;
    Result r = run(base);
    check(r.worstAfterSettle < 0.1 && r.jumps == 0 && r.missing == 0, "plays the source exactly the delay after it arrived", r);

    Sim stall = base;
    stall.stallAt = 14;
    stall.stallFor = 0.15;
    r = run(stall);
    check(r.worstAfterSettle < 0.1 && r.jumps == 0 && r.missing == 0, "a 150 ms network stall with a 300 ms delay isn't heard", r);

    Sim longStall = stall;
    longStall.stallFor = 0.4;
    r = run(longStall);
    check(r.missing > 0, "a 400 ms stall, longer than the delay, is heard as a gap", r);

    Sim rates = base;
    rates.deviceRate = 44100;
    r = run(rates);
    check(r.worstAfterSettle < 0.1 && r.jumps == 0 && r.missing == 0, "48 kHz source on a 44.1 kHz device", r);

    Sim drift = base;
    drift.sourceDrift = 50e-6;  // the source's clock 50 ppm fast: 3 ms a minute
    drift.seconds = 60;
    r = run(drift);
    check(r.jumps == 0 && r.missing == 0 && std::fabs(r.offByMs) < 2, "follows a source clock 50 ppm fast without a glitch", r);

    Sim tooShort = base;
    tooShort.delay = 0.010;  // less than the jitter: some blocks arrive after they're due
    r = run(tooShort);
    check(r.missing > 0, "a delay shorter than the network's jitter is reported (silence)", r);

    std::printf(ok ? "AUDIO SELF-TEST PASSED\n" : "AUDIO SELF-TEST FAILED\n");
    return ok ? 0 : 1;
}

}  // namespace mosaic
