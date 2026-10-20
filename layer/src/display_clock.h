#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

namespace bdex {

// Learns the display's refresh grid from the moments frames were really
// displayed (present_wait completions, in ms on any steady clock). Pure logic,
// unit-tested (tests/test_display_clock.cpp).
//
// The refresh period is the largest period of which (nearly) every interval
// between two displayed frames is a whole multiple, within the wake-up jitter
// of the thread that observes them. A variable-refresh display (VRR/FreeSync/
// G-Sync) or a compositor that does not present on a grid has no such period:
// the clock then stays invalid and nothing relies on it.
// target_fps: the multiplier after `current` (0 = none chosen yet) for a
// game frame interval that allows `ratio` output frames at the target rate
// (target x interval): the largest one at or under the target, within 2 %,
// up to `maxMult`; stepping up waits for a 3 % margin (no flip-flopping at a
// boundary). Above the refresh rate a mailbox display drops frames unevenly.
inline int dynamicMultiplier(int current, double ratio, int maxMult) {
    auto clampInt = [maxMult](double v) {
        const int i = static_cast<int>(v);
        return i < 1 ? 1 : i > maxMult ? maxMult : i;
    };
    const int fits = clampInt(ratio * 1.02);
    if (current == 0 || fits < current) return fits;
    if (current < maxMult && ratio * 0.97 >= current + 1) return clampInt(ratio * 0.97);
    return current;
}

class DisplayClock {
public:
    void add(double displayedMs);      // one displayed frame (not a replaced one)
    void reset() { *this = DisplayClock{}; }
    // The refresh rate the system reports for the display (the control
    // panel reads it), 0 = unknown. The displayed frames alone cannot tell a
    // refresh grid from a regular cadence that skips refreshes: when every
    // other frame is replaced before display, each displayed interval spans
    // two refreshes, and the grid they fit is half the refresh rate (seen:
    // "115 Hz" on a compositor that showed 200 fps just before, which drove
    // the « Auto » multiplier to x1). A period the intervals fit in whole
    // multiples of the hint's is the hint's.
    void setHint(double hz) { hintMs_ = hz > 0.0 ? 1000.0 / hz : 0.0; }
    // The jitter tolerated on an interval (ms, and as a share of the period
    // tried): the observer's wake-up by default; x11_clock's times are the
    // compositor's frame callbacks, up to ~1 ms off (KWin) — at 0.5 ms / 8 %
    // the grid came and went.
    void setJitter(double ms, double share) { jitterMs_ = ms; jitterShare_ = share; }
    // x11_clock: a compositor's frame callbacks under GPU load are mostly on
    // time, with a tail of late ones (Hyprland, PRAGMATA at 1440p: 82 %
    // within 0.5 ms of the grid, 88 % within 1 ms) — enough to fail the
    // 90 % test two estimates in three, the layer falling back to grids at
    // other phases each time. Sticky: a learned period stays while 65 % of
    // the intervals fit within 1.5 times the tolerance (and comes back so
    // after a burst of late ones), and the grid's phase is the median of the
    // recent times, not the last one (perhaps late).
    void setSticky(bool on) { sticky_ = on; }
    bool valid() const { return periodMs_ > 0.0; }
    double periodMs() const { return periodMs_; }
    double refreshHz() const { return valid() ? 1000.0 / periodMs_ : 0.0; }
    // The first refresh at or after `ms` (valid() only): when a frame
    // presented in time for it shows.
    double nextRefresh(double ms) const;
    // The last display time — sticky: the refresh of the grid nearest it.
    double lastMs() const {
        if (!sticky_ || !valid() || phaseMs_ == 0.0) return last_;
        return phaseMs_ + std::round((last_ - phaseMs_) / periodMs_) * periodMs_;
    }

private:
    void estimate();
    void setPhase(const std::vector<double>& sorted);
    std::vector<double> times_;   // ring of the latest display times
    size_t head_ = 0;
    double last_ = 0.0;
    double periodMs_ = 0.0;
    double hintMs_ = 0.0;
    double jitterMs_ = 0.5;
    double jitterShare_ = 0.08;
    bool sticky_ = false;
    double phaseMs_ = 0.0;    // sticky: a refresh of the grid (the median phase of times_)
    double learnedMs_ = 0.0;  // sticky: the last period learned
    int sinceEstimate_ = 0;
};

} // namespace bdex
