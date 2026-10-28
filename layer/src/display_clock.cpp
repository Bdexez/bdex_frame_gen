#include "display_clock.h"

#include <algorithm>
#include <cmath>

namespace bdex {

namespace {
constexpr size_t kWindow = 96;         // display times kept
constexpr double kMinPeriodMs = 2.5;   // 400 Hz
constexpr double kMaxPeriodMs = 50.0;  // 20 Hz
} // namespace

void DisplayClock::add(double ms) {
    if (times_.size() < kWindow) times_.push_back(ms);
    else times_[head_] = ms;
    head_ = (head_ + 1) % kWindow;
    last_ = ms;
    if (++sinceEstimate_ >= 16) estimate();
}

void DisplayClock::estimate() {
    sinceEstimate_ = 0;
    std::vector<double> t = times_;
    std::sort(t.begin(), t.end());
    std::vector<double> d;
    for (size_t i = 1; i < t.size(); ++i)
        if (t[i] - t[i - 1] > jitterMs_) d.push_back(t[i] - t[i - 1]);
    periodMs_ = 0.0;
    if (d.size() < 16) return;
    // Whole multiples of `p` within the observer's jitter: the refined period
    // over the fitting intervals, 0 if fewer than 90 % fit.
    auto fitPeriod = [&d, this](double p) {
        // The tolerance also shrinks with the period: with a loose one, a
        // short period "explains" any set of intervals (a variable-refresh
        // display at 120 fps fitted 360 Hz).
        const double tol = std::min(jitterMs_, jitterShare_ * p);
        double sumD = 0.0, sumN = 0.0;
        size_t fit = 0;
        for (double x : d) {
            const double n = std::round(x / p);
            if (n >= 1.0 && n <= 8.0 && std::fabs(x - n * p) < tol) {
                ++fit;
                sumD += x;
                sumN += n;
            }
        }
        return fit >= d.size() * 9 / 10 ? sumD / sumN : 0.0;
    };
    if (hintMs_ >= kMinPeriodMs && hintMs_ <= kMaxPeriodMs) {
        const double p = fitPeriod(hintMs_);
        if (p > 0.0) {
            periodMs_ = p;
            return;
        }
    }
    std::vector<double> sorted = d;
    std::sort(sorted.begin(), sorted.end());
    const double dmin = sorted[sorted.size() / 10];  // robust smallest interval
    // The smallest interval is one or a few refreshes: try its divisors,
    // largest period first, and keep the first that explains the intervals.
    for (int k = 1; k <= 4; ++k) {
        const double p = dmin / k;
        if (p < kMinPeriodMs || p > kMaxPeriodMs) continue;
        if (const double fitted = fitPeriod(p); fitted > 0.0) {
            periodMs_ = fitted;  // refined over every fitting interval
            return;
        }
    }
}

double DisplayClock::nextRefresh(double ms) const {
    if (!valid()) return ms;
    const double k = std::ceil((ms - last_) / periodMs_);
    return last_ + std::max(k, 0.0) * periodMs_;
}

} // namespace bdex
