#include "x11_present_clock.h"
#include "test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

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

// A game presenting slower than the display under Xwayland (Civilization VI
// at 60 fps on 143.9 Hz, Hyprland, 10/11): the completions are the presents'
// own times, 16.3 to 17.0 ms apart, on no refresh grid. A short period learned
// before (here a 294 Hz one) "explains" them all — 16.7 ms is about 5 of its
// periods, within the sticky tolerance — and stayed for good: the status said
// 293.7 Hz, held_per_s 233, and so would the pacing once generating. A period
// that no interval spans three times or less is not kept.
TEST(present_timeline_drops_a_short_period_no_interval_shows) {
    PresentTimeline t;
    uint32_t s = 9;
    double ust = 1000.0;
    uint64_t msc = 1;
    const double p294 = 1000.0 / 294.0;
    for (; msc <= 96; ++msc) t.add(msc, ust += ((msc % 3) ? 1 : 2) * p294 + jitter(s, 0.1));  // a 294 Hz grid
    CHECK(t.valid() && std::fabs(t.refreshHz() - 294.0) < 1.0);
    for (int i = 0; i < 600; ++i) t.add(msc++, ust += 16.67 + jitter(s, 0.35));  // 60 fps, off any grid
    CHECK(!t.valid() || std::fabs(t.refreshHz() - 294.0) > 20.0);
    // ...while the same 60 fps on the 143.9 Hz grid (2 and 3 refreshes apart) keeps it.
    PresentTimeline g;
    g.setHint(143.912);
    double u = 1000.0, content = 1000.0;
    for (uint64_t m = 1; m <= 600; ++m) {
        content += 16.67;
        u = std::max(u + kP, std::ceil(content / kP) * kP);
        g.add(m, u + jitter(s, 0.3));
    }
    CHECK(g.valid() && std::fabs(g.periodMs() - kP) < 0.05);
}

// A game that presents no faster than the screen refreshes, under Xwayland:
// each present completes at its own time, one MSC each. A steady cadence
// fits a "grid" — the game's: State of Decay 2 armed at 79.6 fps on a
// 143.9 Hz screen read 82.0 Hz (11/11), and at 72.8 fps 71.3 Hz. « Auto »
// aims at the refresh rate: a game at its own "refresh rate" got no generated
// frame, and so never showed the screen's grid. Not a grid: every interval
// is one period, no present met another in a refresh, and the system reports
// another rate (or none).
TEST(present_timeline_does_not_take_the_games_cadence_for_the_screen) {
    for (const double hint : {143.912, 0.0}) {
        PresentTimeline t;
        t.setHint(hint);
        uint32_t s = 21;
        double ust = 1000.0;
        uint64_t msc = 1;
        for (int i = 0; i < 600; ++i) t.add(msc++, ust += 1000.0 / 79.6 + jitter(s, 0.3));
        CHECK(!t.valid());
        // Generation starts: three frames a game frame, more than the screen
        // takes — ticks on its grid, presents replaced. The grid is learned...
        double present = ust;
        uint64_t tick = uint64_t(std::ceil(ust / kP));
        for (int i = 0; i < 1200; ++i) {
            present += 1000.0 / (3 * 79.6);
            const uint64_t at = uint64_t(std::ceil(present / kP));  // the callback after this present
            if (at != tick) {
                ++msc;
                ust = double(at) * kP + jitter(s, 0.3);  // (one callback, one time: the same for the presents it takes)
            }
            tick = at;
            t.add(msc, ust);
        }
        CHECK(t.valid() && std::fabs(t.periodMs() - kP) < 0.05);
        CHECK(t.replaced > 300);
        // ...and stays when the frames then go out one a refresh, paced on it
        // (no interval of two periods, no replaced present any more).
        for (int i = 0; i < 600; ++i) t.add(++msc, double(++tick) * kP + jitter(s, 0.3));
        CHECK(t.valid() && std::fabs(t.periodMs() - kP) < 0.05);
    }
}

// A real grid away from the hint (the panel reads the fastest monitor, the
// game is on a 60 Hz one): 45 fps there completes one or two refreshes
// apart — the intervals show the grid, and it is taken.
TEST(present_timeline_takes_a_grid_the_intervals_show) {
    PresentTimeline t;
    t.setHint(143.912);
    uint32_t s = 4;
    const double p60 = 1000.0 / 60.0;
    double content = 1000.0, ust = 1000.0;
    for (uint64_t msc = 1; msc <= 400; ++msc) {
        content += 1000.0 / 45.0;
        ust = std::max(ust + p60, std::ceil(content / p60) * p60);
        t.add(msc, ust + jitter(s, 0.3));
    }
    CHECK(t.valid() && std::fabs(t.refreshHz() - 60.0) < 0.5);
}

// A real game, as the X server reported it (tools/x11_trace: bdex-x11-trace
// watch, read-only, 11/11): Civilization VI (VKD3D-Proton) at 60 fps on the
// 143.912 Hz screen, Hyprland's Xwayland, the layer not generating — 20 s,
// one MSC a present. Part of the completions are on the screen's grid (two,
// then three refreshes apart), part at the presents' own times (2.4
// refreshes: the game's 16.67 ms). The estimator of 10/11 held "60 Hz" in 14
// estimates of 75 — the game's cadence; a grid, when one is held, must be
// the screen's.
TEST(present_timeline_real_trace_of_a_60_fps_game_is_not_a_60_hz_screen) {
    for (const double hint : {143.912, 0.0}) {
        std::ifstream f(BDEX_SOURCE_DIR "/tests/data/x11_present_civ6_60fps.txt");
        CHECK(f.good());
        PresentTimeline t;
        t.setHint(hint);
        unsigned window = 0, mode = 0;
        unsigned long long msc = 0, ust = 0;
        int events = 0, wrong = 0;
        while (f >> window >> msc >> ust >> mode) {
            t.add(msc, double(ust) / 1000.0);
            if (++events % 16 == 0 && t.valid() && std::fabs(t.refreshHz() - 143.912) > 0.05 * 143.912) ++wrong;
        }
        CHECK(events == 1201);
        CHECK(wrong == 0);
        CHECK(t.shown == 1200 && t.replaced == 0);
    }
}
