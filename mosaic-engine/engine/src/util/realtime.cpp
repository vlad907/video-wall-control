#include "util/realtime.hpp"

#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace mosaic {

using Clock = std::chrono::steady_clock;

// Spin (yielding) for at most this long before the deadline; sleep before that.
constexpr auto kSpinWindow = std::chrono::microseconds(700);

void raiseThreadPriority(ThreadRole role) {
#if defined(_WIN32)
    if (role == ThreadRole::Render) {
        // MMCSS "Games" class: the scheduler boosts it like a game's render thread.
        DWORD taskIndex = 0;
        if (HANDLE h = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex)) AvSetMmThreadPriority(h, AVRT_PRIORITY_HIGH);
        else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
#elif defined(__APPLE__)
    if (role == ThreadRole::Render) {
        // Time-constraint (real-time) policy, as used by audio/video threads.
        // Without it macOS coalesces timers and wakes the frame loop ~3-7 ms
        // late; with it, measured lateness drops to microseconds.
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        double perMs = double(tb.denom) / double(tb.numer) * 1e6;
        thread_time_constraint_policy_data_t p;
        p.period = static_cast<uint32_t>(16.667 * perMs);
        p.computation = static_cast<uint32_t>(2.0 * perMs);  // our CPU work is ~0.5 ms/frame
        p.constraint = static_cast<uint32_t>(10.0 * perMs);
        p.preemptible = 1;
        if (thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                              reinterpret_cast<thread_policy_t>(&p), THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS)
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    } else {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    }
#else
    (void)role;
#endif
}

PreciseSleeper::PreciseSleeper() {
#if defined(_WIN32)
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) {
        // Pre-1803 Windows: raise the system timer resolution instead.
        timePeriodSet_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    }
#endif
}

PreciseSleeper::~PreciseSleeper() {
#if defined(_WIN32)
    if (timer_) CloseHandle(static_cast<HANDLE>(timer_));
    if (timePeriodSet_) timeEndPeriod(1);
#endif
}

void PreciseSleeper::sleepUntil(Clock::time_point t) {
    auto coarse = t - kSpinWindow;
    auto now = Clock::now();
    if (now < coarse) {
#if defined(_WIN32)
        if (timer_) {
            // Relative due time in 100 ns units (negative = relative).
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(coarse - now).count() / 100);
            if (SetWaitableTimerEx(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, nullptr, 0))
                WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE);
            else
                std::this_thread::sleep_until(coarse);
        } else {
            std::this_thread::sleep_until(coarse);
        }
#else
        std::this_thread::sleep_until(coarse);
#endif
    }
    while (Clock::now() < t) std::this_thread::yield();
}

}  // namespace mosaic
