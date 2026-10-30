#include "x11_present_clock.h"
#include "test.h"

#include <cmath>
#include <cstdint>

using namespace bdex;

namespace {
double jitter(uint32_t& s, double a) {
    s = s * 1664525u + 1013904223u;
    return (double((s >> 8) & 0xffff) / 65535.0 * 2.0 - 1.0) * a;
}
constexpr double kP = 1000.0 / 143.912;
} // namespace

// One present a refresh, as x4 on 30 fps fills a 144 Hz screen: each runs
// after the frame callback of its refresh (the UST, a compositor's, up to
// ~0.5 ms off). The grid is learned and every present counts as shown.
TEST(present_timeline_learns_the_compositor_grid) {
    PresentTimeline t;
    t.setHint(143.912);
    uint32_t s = 7;
    for (uint64_t msc = 1; msc <= 300; ++msc) t.add(msc, 1000.0 + msc * kP + jitter(s, 0.5));
    CHECK(t.valid());
    CHECK(std::fabs(t.periodMs() - kP) < 0.02);
    CHECK(t.shown == 299 && t.replaced == 0 && t.held == 0);
}

// Two presents run before the same callback: the first was overwritten.
// A callback a refresh late (nothing new handed over for one): one held.
TEST(present_timeline_counts_replaced_and_held) {
    PresentTimeline t;
    t.setHint(143.912);
    uint64_t msc = 1;
    double ust = 1000.0;
    for (int i = 0; i < 60; ++i) t.add(msc++, ust += kP);
    t.add(msc, ust += kP);
    t.add(msc, ust);  // same MSC: the one before it was replaced
    ++msc;
    t.add(msc++, ust += 2 * kP);  // the callback came two refreshes later
    CHECK(t.replaced == 1);
    CHECK(t.held == 1);
    CHECK(t.presents == 63);
}

// Xwayland ticks only after a commit: a game at 30 fps without generated
// frames gives a callback every 4 or 5 refreshes. With the refresh rate the
// panel reads, the grid is still the screen's, not 30 Hz.
TEST(present_timeline_sparse_callbacks_keep_the_refresh) {
    PresentTimeline t;
    t.setHint(143.912);
    double ust = 1000.0;
    uint32_t s = 3;
    for (uint64_t msc = 1; msc <= 120; ++msc) {
        ust += (msc % 5 == 0 ? 4 : 5) * kP;
        t.add(msc, ust + jitter(s, 0.3));
    }
    CHECK(t.valid());
    CHECK(std::fabs(t.periodMs() - kP) < 0.05);
}

// A window counter that goes back (a new window, a reset): starts over
// without counting nonsense.
TEST(present_timeline_restarts_on_a_counter_reset) {
    PresentTimeline t;
    t.add(100, 1000.0);
    t.add(101, 1000.0 + kP);
    t.add(5, 2000.0);
    t.add(6, 2000.0 + kP);
    CHECK(t.shown == 2 && t.replaced == 0 && t.held == 0);
}

// A compositor's frame callbacks under GPU load (Hyprland, PRAGMATA at
// 1440p, measured: 84 % within 0.4 ms of the grid, the rest spread up to
// half a period). The 90 % test alone lost the grid two estimates in three
// (the layer then paced on other grids, at other phases); once learned (a
// calmer moment) it now stays, and its phase is the median of the recent
// callbacks, not the last one — late or not.
TEST(present_timeline_keeps_the_grid_under_late_callbacks) {
    PresentTimeline t;
    t.setHint(143.912);
    uint32_t s = 17;
    int checks = 0, valid = 0;
    double worstPhase = 0.0;
    for (uint64_t msc = 1; msc <= 3000; ++msc) {
        const double grid = 1000.0 + double(msc) * kP;
        const double u = (jitter(s, 1.0) + 1.0) * 0.5;  // [0, 1]
        const double off = msc <= 200 || u < 0.84 ? jitter(s, 0.4) : jitter(s, 3.4);
        t.add(msc, grid + off);
        if (msc > 200 && msc % 16 == 0) {
            ++checks;
            if (t.valid()) {
                ++valid;
                worstPhase = std::max(worstPhase, std::fabs(std::remainder(t.lastMs() - 1000.0, kP)));
            }
        }
    }
    CHECK(valid == checks);
    CHECK(worstPhase < 0.5);
}

// ...while a variable-refresh display (any interval) still has no grid.
TEST(present_timeline_sticky_grid_rejects_variable_refresh) {
    PresentTimeline t;
    t.setHint(143.912);
    uint32_t s = 5;
    double ust = 1000.0;
    for (uint64_t msc = 1; msc <= 400; ++msc) t.add(msc, ust += 7.0 + std::fabs(jitter(s, 6.0)));
    CHECK(!t.valid());
}
