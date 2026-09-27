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
