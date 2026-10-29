#pragma once

#include <atomic>
#include <chrono>
#include <mutex>

namespace mosaic {

using Clock = std::chrono::steady_clock;

inline double msBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Exponentially weighted moving average; thread-safe.
class Ewma {
public:
    explicit Ewma(double alpha = 0.05) : alpha_(alpha) {}
    void add(double v) {
        std::lock_guard l(m_);
        value_ = has_ ? value_ + alpha_ * (v - value_) : v;
        has_ = true;
        if (v > max_) max_ = v;
    }
    double value() const {
        std::lock_guard l(m_);
        return value_;
    }
    // Largest sample since the last call.
    double takeMax() {
        std::lock_guard l(m_);
        double v = max_;
        max_ = 0;
        return v;
    }

private:
    mutable std::mutex m_;
    double alpha_, value_ = 0, max_ = 0;
    bool has_ = false;
};

// Events per second over a sliding ~1 s window.
class RateMeter {
public:
    void tick() {
        std::lock_guard l(m_);
        auto now = Clock::now();
        ++count_;
        if (now - windowStart_ >= std::chrono::seconds(1)) {
            rate_ = count_ / std::chrono::duration<double>(now - windowStart_).count();
            count_ = 0;
            windowStart_ = now;
        }
        last_ = now;
    }
    double rate() const {
        std::lock_guard l(m_);
        return Clock::now() - last_ > std::chrono::seconds(2) ? 0.0 : rate_;
    }

private:
    mutable std::mutex m_;
    Clock::time_point windowStart_ = Clock::now(), last_{};
    int count_ = 0;
    double rate_ = 0;
};

}  // namespace mosaic
