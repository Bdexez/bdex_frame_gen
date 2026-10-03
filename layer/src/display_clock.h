#pragma once
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
    bool valid() const { return periodMs_ > 0.0; }
    double periodMs() const { return periodMs_; }
    double refreshHz() const { return valid() ? 1000.0 / periodMs_ : 0.0; }
    // The first refresh at or after `ms` (valid() only): when a frame
    // presented in time for it shows.
    double nextRefresh(double ms) const;
    double lastMs() const { return last_; }

private:
    void estimate();
    std::vector<double> times_;   // ring of the latest display times
    size_t head_ = 0;
    double last_ = 0.0;
    double periodMs_ = 0.0;
    int sinceEstimate_ = 0;
};

} // namespace bdex
