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
    if (times_.size() < kWindow) {
        times_.push_back(ms);
        replacedBefore_.push_back(replacedSince_);
    } else {
        times_[head_] = ms;
        replacedBefore_[head_] = replacedSince_;
    }
    replacedSince_ = false;
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
    const auto replaced = std::count(replacedBefore_.begin(), replacedBefore_.end(), uint8_t(1));
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
    // `mixed`: the fitting intervals that are not the commonest number of
    // periods long (needsEvidence).
    size_t mixed = 0, fitted = 0;
    int commonest = 0;
    auto fitPeriod = [&d, &mixed, &fitted, &commonest, this](double p, double loose = 1.0, double share = 0.9, bool shortNeeded = false) {
        // The tolerance also shrinks with the period: with a loose one, a
        // short period "explains" any set of intervals (a variable-refresh
        // display at 120 fps fitted 360 Hz).
        const double tol = loose * std::min(jitterMs_, jitterShare_ * p);
        double sumD = 0.0, sumN = 0.0;
        size_t fit = 0, shortFit = 0, byCount[9] = {};
        for (double x : d) {
            const double n = std::round(x / p);
            if (n >= 1.0 && n <= 8.0 && std::fabs(x - n * p) < tol) {
                ++fit;
                ++byCount[int(n)];
                shortFit += n <= 3.0;
                sumD += x;
                sumN += n;
            }
        }
        fitted = fit;
        commonest = int(std::max_element(byCount, byCount + 9) - byCount);
        mixed = fit - byCount[commonest];
        const size_t need = share >= 0.9 ? d.size() * 9 / 10 : size_t(std::ceil(double(d.size()) * share));
        if (shortNeeded && shortFit * 10 < d.size()) return 0.0;
        return fit >= need ? sumD / sumN : 0.0;
    };
    // (Hyprland's callbacks under load, a minute and a half replayed: valid
    // 45 % of the estimates and 34 switches; sticky, 100 % and one, the
    // grid's phase moving over 1 ms at 0.5 % of the callbacks instead of
    // 12.5 %. Random intervals — a variable-refresh display — never keep
    // it: 0 in 40 000.)
    // needsEvidence, a period tried (the learned one, the hint's) that the
    // intervals fit without showing a grid — all the same number of periods:
    // two or more only say that the presents' cadence is near a multiple of
    // it, the period stays the one tried, not refined to that cadence (a
    // game at 67.6 fps on a 143.9 Hz screen read 135.2 Hz); one each, the
    // fit is that cadence itself, taken when within 5 % of the period tried
    // (128 fps read 128 Hz on the 143.9 Hz screen, inside the tolerance).
    auto settle = [&](double tried, double p) {
        const bool plain = !needsEvidence_ || mixed * 10 >= fitted;
        if (!plain && commonest == 1 && std::fabs(p - tried) > 0.05 * tried) return false;
        const bool keep = !plain && commonest >= 2;
        periodMs_ = keep ? tried : p;
        setPhase(t, !keep);
        return true;
    };
    if (sticky_ && learnedMs_ > 0.0) {  // (also to take it back after a burst of late ones)
        if (const double p = fitPeriod(learnedMs_, 1.5, 0.65, true); p > 0.0 && settle(learnedMs_, p)) return;
    }
    if (hintMs_ >= kMinPeriodMs && hintMs_ <= kMaxPeriodMs) {
        if (const double p = fitPeriod(hintMs_); p > 0.0 && settle(hintMs_, p)) return;
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
        if (const double refined = fitPeriod(p, 1.0, 0.9, k >= 2); refined > 0.0) {
            // needsEvidence: the display's grid, or only the cadence of
            // presents that each completed at its own time? Intervals of
            // different lengths in periods (a tenth of them), or presents
            // replaced within one period (three in the window), tell; so do
            // the system's refresh rate and a period that showed them before
            // (one frame a refresh, paced on the grid, shows neither).
            if (needsEvidence_) {
                auto within5 = [](double a, double b) { return b > 0.0 && std::fabs(a - b) < 0.05 * b; };
                const bool shown = mixed * 10 >= fitted || (k == 1 && replaced >= 3);
                if (shown) evidencedMs_ = refined;
                else if (!within5(refined, hintMs_) && !within5(refined, evidencedMs_)) continue;
                if (!shown && commonest >= 2) {  // (as settle: a cadence near a multiple of that period)
                    periodMs_ = within5(refined, hintMs_) ? hintMs_ : evidencedMs_;
                    setPhase(t, false);
                    return;
                }
            }
            periodMs_ = refined;  // refined over every fitting interval
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
void DisplayClock::setPhase(const std::vector<double>& t, bool refine) {
    if (!sticky_ || t.size() < 16) return;
    // (Times 8 apart first: the refreshes between them are counted right
    // from a period several percent off; then half the window.)
    for (const size_t h : {size_t(8), t.size() / 2}) {
        if (!refine) break;
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
