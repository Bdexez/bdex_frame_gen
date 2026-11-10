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
    // Whole multiples of `p` within the observer's jitter (times `loose`):
    // the refined period over the fitting intervals, 0 if fewer than
    // `share` of them fit (90 %).
    // `shortNeeded`: the period must also be seen — some intervals one to
    // three periods long (10 % of them). Without, a short period explains
    // intervals that are all several of its periods within the tolerance:
    // under Xwayland, a game presenting at 60 fps on a 143.9 Hz screen has
    // completions at its presents' own times, 16.3-17.0 ms apart, and a
    // 294 Hz period learned before took each for 5 of its refreshes, sticky
    // for good (Civilization VI, 10/11: refresh_hz 293.7 in the status).
    auto fitPeriod = [&d, this](double p, double loose = 1.0, double share = 0.9, bool shortNeeded = false) {
        // The tolerance also shrinks with the period: with a loose one, a
        // short period "explains" any set of intervals (a variable-refresh
        // display at 120 fps fitted 360 Hz).
        const double tol = loose * std::min(jitterMs_, jitterShare_ * p);
        double sumD = 0.0, sumN = 0.0;
        size_t fit = 0, shortFit = 0;
        for (double x : d) {
            const double n = std::round(x / p);
            if (n >= 1.0 && n <= 8.0 && std::fabs(x - n * p) < tol) {
                ++fit;
                shortFit += n <= 3.0;
                sumD += x;
                sumN += n;
            }
        }
        const size_t need = share >= 0.9 ? d.size() * 9 / 10 : size_t(std::ceil(double(d.size()) * share));
        if (shortNeeded && shortFit * 10 < d.size()) return 0.0;
        return fit >= need ? sumD / sumN : 0.0;
    };
    // (Hyprland's callbacks under load, a minute and a half replayed: valid
    // 45 % of the estimates and 34 switches; sticky, 100 % and one, the
    // grid's phase moving over 1 ms at 0.5 % of the callbacks instead of
    // 12.5 %. Random intervals — a variable-refresh display — never keep
    // it: 0 in 40 000.)
    if (sticky_ && learnedMs_ > 0.0) {  // (also to take it back after a burst of late ones)
        if (const double p = fitPeriod(learnedMs_, 1.5, 0.65, true); p > 0.0) {
            periodMs_ = p;
            setPhase(t);
            return;
        }
    }
    if (hintMs_ >= kMinPeriodMs && hintMs_ <= kMaxPeriodMs) {
        const double p = fitPeriod(hintMs_);
        if (p > 0.0) {
            periodMs_ = p;
            setPhase(t);
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
        // (a divisor of the smallest interval: no interval is shorter than k
        // periods, the evidence must be there — see fitPeriod)
        if (const double fitted = fitPeriod(p, 1.0, 0.9, k >= 2); fitted > 0.0) {
            periodMs_ = fitted;  // refined over every fitting interval
            setPhase(t);
            return;
        }
    }
}

// Sticky: the period again, from the median slope between times half the
// window apart (the mean of the fitting intervals, with late callbacks among
// them, wandered from 6.86 to 7.01 ms for 6.95 — the phase over the window
// then off by milliseconds), and the grid's phase from the median offset of
// the recent times to the last one — late outliers, the last one included,
// do not move it.
void DisplayClock::setPhase(const std::vector<double>& t) {
    if (!sticky_ || t.size() < 16) return;
    // (Times 8 apart first: the refreshes between them are counted right
    // from a period several percent off; then half the window.)
    for (const size_t h : {size_t(8), t.size() / 2}) {
        std::vector<double> slopes;
        for (size_t i = 0; i + h < t.size(); ++i) {
            const double n = std::round((t[i + h] - t[i]) / periodMs_);
            if (n >= 1.0) slopes.push_back((t[i + h] - t[i]) / n);
        }
        if (slopes.empty()) break;
        std::nth_element(slopes.begin(), slopes.begin() + long(slopes.size() / 2), slopes.end());
        const double p = slopes[slopes.size() / 2];
        if (std::fabs(p - periodMs_) < 0.05 * periodMs_) periodMs_ = p;
    }
    learnedMs_ = periodMs_;
    std::vector<double> r;
    r.reserve(t.size());
    for (double x : t) r.push_back(std::remainder(x - last_, periodMs_));
    std::nth_element(r.begin(), r.begin() + long(r.size() / 2), r.end());
    phaseMs_ = last_ + r[r.size() / 2];
}

double DisplayClock::nextRefresh(double ms) const {
    if (!valid()) return ms;
    const double last = lastMs();
    const double k = std::ceil((ms - last) / periodMs_);
    return last + std::max(k, 0.0) * periodMs_;
}

} // namespace bdex
