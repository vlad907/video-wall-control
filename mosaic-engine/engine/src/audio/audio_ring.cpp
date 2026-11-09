#include "audio/audio_ring.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic {

void AudioRing::push(const float* left, const float* right, int frames, int rate, double arrivedSeconds) {
    if (frames <= 0 || rate <= 0) return;
    std::lock_guard l(m_);
    if (rate != rate_) {  // first audio, or the source changed rate: start over
        rate_ = rate;
        l_.assign(static_cast<size_t>(kSeconds) * rate, 0.f);
        r_.assign(l_.size(), 0.f);
        written_ = 0;
        offsets_.clear();
    }
    const size_t cap = l_.size();
    for (int i = 0; i < frames; ++i) {
        size_t at = (written_ + static_cast<uint64_t>(i)) % cap;
        l_[at] = left[i];
        r_[at] = right[i];
    }
    written_ += static_cast<uint64_t>(frames);
    lastArrival_ = arrivedSeconds;

    // The block's last sample arrived now. The smallest offset of the last ~10 s is a block
    // that came straight through. A jump of over a second (the source stopped and came back,
    // or restarted) starts the estimate over.
    double offset = arrivedSeconds - static_cast<double>(written_) / rate;
    if (!offsets_.empty() && std::fabs(offset - base_) > 1.0) offsets_.clear();
    offsets_.push_back({arrivedSeconds, offset});
    while (offsets_.front().first < arrivedSeconds - kSeconds) offsets_.pop_front();
    base_ = offsets_.front().second;
    for (auto& o : offsets_) base_ = std::min(base_, o.second);
}

AudioRing::Timeline AudioRing::timeline() const {
    std::lock_guard l(m_);
    Timeline t;
    t.known = written_ > 0;
    t.rate = rate_ ? rate_ : 48000;
    t.base = base_;
    t.written = written_;
    return t;
}

int AudioRing::read(double index, double step, int n, float* left, float* right) const {
    std::lock_guard l(m_);
    const double cap = static_cast<double>(l_.size());
    const double newest = static_cast<double>(written_) - 1;  // last sample held
    const double oldest = std::max(0.0, static_cast<double>(written_) - cap);
    int missing = 0;
    for (int k = 0; k < n; ++k) {
        double x = index + k * step;
        if (l_.empty() || x < oldest || x > newest) {
            left[k] = right[k] = 0.f;
            ++missing;
            continue;
        }
        auto i0 = static_cast<uint64_t>(x);
        double frac = x - static_cast<double>(i0);
        size_t a = static_cast<size_t>(i0 % l_.size());
        size_t b = static_cast<size_t>(std::min<uint64_t>(i0 + 1, written_ - 1) % l_.size());
        left[k] = static_cast<float>(l_[a] + (l_[b] - l_[a]) * frac);
        right[k] = static_cast<float>(r_[a] + (r_[b] - r_[a]) * frac);
    }
    return missing;
}

double AudioRing::lastArrival() const {
    std::lock_guard l(m_);
    return lastArrival_;
}

void AudioPlayhead::fill(const AudioRing& ring, double deviceRate, double heardAt, double delaySeconds, int n, float* left, float* right) {
    auto t = ring.timeline();
    if (!t.known || deviceRate <= 0) {
        std::fill(left, left + n, 0.f);
        std::fill(right, right + n, 0.f);
        return;
    }
    // The source sample that should be heard at `heardAt`.
    double target = (heardAt - delaySeconds - t.base) * t.rate;
    if (pos_ < 0 || std::fabs(target - pos_) > 0.1 * t.rate) {
        if (pos_ >= 0) ++jumps_;
        pos_ = target;
    }
    double error = target - pos_;  // samples: positive = we're behind, play a touch faster
    errorMs_ = error * 1000.0 / t.rate;
    double nudge = std::clamp(error / t.rate / 2.0, -0.002, 0.002);  // close the gap over ~2 s
    double step = t.rate / deviceRate * (1.0 + nudge);
    missing_ += static_cast<uint64_t>(ring.read(pos_, step, n, left, right));
    pos_ += step * n;
}

}  // namespace mosaic
