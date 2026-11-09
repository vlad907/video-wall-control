// Audio of one NDI source, and playing it a set time after it arrived. Portable (no OS audio
// here): the NDI receive thread fills an AudioRing, and each audio output's device thread
// reads it through its own AudioPlayhead. Tested by `mosaic_engine --audio-self-test`.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace mosaic {

// The last ~10 s of one source as stereo float samples at the source's rate, and when they
// arrived. Sample i plays "as live" at base + i / rate (seconds on the steady clock), where
// base comes from the fastest-arriving blocks of the last ~10 s: like the receiver's video
// buffer, network jitter only ever delays a block, never speeds it up.
class AudioRing {
public:
    static constexpr int kSeconds = 10;

    // Receive thread. Mono sources pass the same pointer for both channels.
    void push(const float* left, const float* right, int frames, int rate, double arrivedSeconds);

    struct Timeline {
        bool known = false;
        int rate = 48000;
        double base = 0;       // seconds (steady clock) when sample 0 arrived, as if live
        uint64_t written = 0;  // samples received so far
    };
    Timeline timeline() const;

    // Copies n samples from `index` on, `step` source samples per output sample (linear
    // interpolation); silence where nothing is held. Returns how many were missing.
    int read(double index, double step, int n, float* left, float* right) const;

    double lastArrival() const;  // seconds, 0 = nothing yet

private:
    mutable std::mutex m_;
    std::vector<float> l_, r_;  // ring, capacity kSeconds * rate
    int rate_ = 0;
    uint64_t written_ = 0;
    double lastArrival_ = 0;
    std::deque<std::pair<double, double>> offsets_;  // (arrival, arrival - written / rate), last ~10 s
    double base_ = 0;
};

// One output's position in a source: which sample is heard when. Each device period it is
// told when its first frame will be heard and the delay wanted, and keeps the source's samples
// that far behind their arrival: by nudging the playback rate (at most 0.2 %, inaudible) for
// clock drift and jitter, or by jumping when it's far off (a new delay, a restarted source).
class AudioPlayhead {
public:
    // Fills n device frames at `deviceRate` that will be heard at `heardAt` (seconds, steady
    // clock), `delaySeconds` after the source's audio arrived.
    void fill(const AudioRing& ring, double deviceRate, double heardAt, double delaySeconds, int n, float* left, float* right);

    double errorMs() const { return errorMs_; }      // how far from the wanted delay (last fill)
    uint64_t jumps() const { return jumps_; }        // times it had to jump (an audible glitch)
    uint64_t missing() const { return missing_; }    // samples it had to play as silence
    bool playing() const { return pos_ >= 0; }

private:
    double pos_ = -1;  // source sample index of the next frame; -1 = not started
    double errorMs_ = 0;
    uint64_t jumps_ = 0, missing_ = 0;
};

inline double steadySeconds(std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now()) {
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

}  // namespace mosaic
