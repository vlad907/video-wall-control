// Thread priority and precise frame pacing.
//
// Windows' default sleep granularity is ~15.6 ms, close to a whole 60 fps
// frame, so std::this_thread::sleep_until alone gives uneven frame times.
// PreciseClock uses a high-resolution waitable timer (Windows 10 1803+, with
// a timeBeginPeriod(1) fallback) for the coarse wait and spins with yields
// for the last fraction of a millisecond.
#pragma once

#include <chrono>

namespace mosaic {

enum class ThreadRole { Render, Sender };

// Best effort; failures are ignored.
void raiseThreadPriority(ThreadRole role);

class PreciseSleeper {
public:
    PreciseSleeper();
    ~PreciseSleeper();
    PreciseSleeper(const PreciseSleeper&) = delete;
    PreciseSleeper& operator=(const PreciseSleeper&) = delete;

    void sleepUntil(std::chrono::steady_clock::time_point t);

private:
    [[maybe_unused]] void* timer_ = nullptr;  // Windows HANDLE
    [[maybe_unused]] bool timePeriodSet_ = false;
};

}  // namespace mosaic
