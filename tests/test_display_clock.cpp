#include "display_clock.h"
#include "test.h"

#include <cmath>
#include <cstdint>

using namespace bdex;

namespace {
// Deterministic jitter in [-a, a].
double jitter(uint32_t& s, double a) {
    s = s * 1664525u + 1013904223u;
    return (double((s >> 8) & 0xffff) / 65535.0 * 2.0 - 1.0) * a;
}
}

TEST(display_clock_learns_a_fixed_refresh) {
    DisplayClock c;
    uint32_t s = 1;
    const double p = 1000.0 / 144.0;
    double t = 100.0;
    // Frames every one or two refreshes (60-ish fps output on 144 Hz), with
    // observer jitter.
    for (int i = 0; i < 96; ++i) {
        t += p * ((i % 5 == 0) ? 3 : 2);
        c.add(t + jitter(s, 0.2));
    }
    CHECK(c.valid());
    CHECK(std::fabs(c.refreshHz() - 144.0) < 0.5);
    const double next = c.nextRefresh(c.lastMs() + 0.3 * p);
    CHECK(std::fabs(next - (c.lastMs() + p)) < 0.3);
}

TEST(display_clock_learns_one_frame_per_refresh) {
    DisplayClock c;
    uint32_t s = 7;
    for (int i = 0; i < 64; ++i) c.add(i * (1000.0 / 60.0) + jitter(s, 0.15));
    CHECK(c.valid() && std::fabs(c.refreshHz() - 60.0) < 0.3);
}

TEST(display_clock_rejects_variable_refresh) {
    DisplayClock c;
    uint32_t s = 3;
    double t = 0.0;
    for (int i = 0; i < 96; ++i) {
        t += 7.0 + std::fabs(jitter(s, 6.0));  // VRR: any interval
        c.add(t);
    }
    CHECK(!c.valid());
    CHECK(c.nextRefresh(123.0) == 123.0);
}

// Every other frame replaced before display: the displayed intervals all
// span two refreshes. Alone, the clock takes that for a refresh grid at half
// the rate; with the system's refresh rate as a hint it keeps the real one.
TEST(display_clock_hint_resolves_skipped_refreshes) {
    const double p = 1000.0 / 240.0;
    uint32_t s = 5;
    DisplayClock alone, hinted;
    hinted.setHint(240.0);
    for (int i = 0; i < 96; ++i) {
        const double t = 50.0 + i * 2.0 * p + jitter(s, 0.15);
        alone.add(t);
        hinted.add(t);
    }
    CHECK(alone.valid() && std::fabs(alone.refreshHz() - 120.0) < 0.5);
    CHECK(hinted.valid() && std::fabs(hinted.refreshHz() - 240.0) < 0.5);
}

// A hint the displayed frames contradict (another screen, a stale reading)
// is ignored: the measured grid wins.
TEST(display_clock_ignores_a_wrong_hint) {
    DisplayClock c;
    c.setHint(60.0);
    uint32_t s = 11;
    for (int i = 0; i < 96; ++i) c.add(i * (1000.0 / 144.0) + jitter(s, 0.15));
    CHECK(c.valid() && std::fabs(c.refreshHz() - 144.0) < 0.5);
    // ...and a variable-refresh display stays without a grid.
    DisplayClock vrr;
    vrr.setHint(144.0);
    double t = 0.0;
    for (int i = 0; i < 96; ++i) {
        t += 7.0 + std::fabs(jitter(s, 6.0));
        vrr.add(t);
    }
    CHECK(!vrr.valid());
}

// Without sticky (the present feedback's clock), the same late tail loses
// the grid — the reason x11_clock's is sticky.
TEST(display_clock_late_tail_fails_the_plain_test) {
    DisplayClock plain;
    plain.setJitter(1.0, 0.15);
    plain.setHint(143.912);
    const double p = 1000.0 / 143.912;
    uint32_t s = 17;
    int invalid = 0;
    for (int i = 1; i <= 3000; ++i) {
        const double u = (jitter(s, 1.0) + 1.0) * 0.5;
        plain.add(1000.0 + i * p + (i <= 200 || u < 0.84 ? jitter(s, 0.4) : jitter(s, 3.4)));
        if (i > 200 && i % 16 == 0) invalid += !plain.valid();
    }
    CHECK(invalid > 20);
}

// target_fps: the output never goes above the target (the refresh rate), where
// a mailbox display drops frames unevenly; hysteresis at the boundaries.
TEST(dynamic_multiplier_stays_under_the_target) {
    using bdex::dynamicMultiplier;
    auto ratio = [](double gameFps) { return 144.0 / gameFps; };  // 144 Hz target
    CHECK(dynamicMultiplier(0, ratio(60), 4) == 2);   // 120
    CHECK(dynamicMultiplier(0, ratio(72), 4) == 2);   // 144
    CHECK(dynamicMultiplier(0, ratio(50), 4) == 2);   // 100, not 150
    CHECK(dynamicMultiplier(0, ratio(48), 4) == 3);   // 144
    CHECK(dynamicMultiplier(0, ratio(36), 4) == 4);   // 144
    CHECK(dynamicMultiplier(0, ratio(20), 4) == 4);   // capped at the multiplier
    CHECK(dynamicMultiplier(0, ratio(90), 4) == 1);   // x2 would be 180
    CHECK(dynamicMultiplier(0, ratio(36), 2) == 2);   // capped at x2
    // Down at once when the output would exceed the target...
    CHECK(dynamicMultiplier(3, ratio(52), 4) == 2);
    // ... up only with a margin: 47.5 fps x3 = 142.5 fits, but not by 3 %.
    CHECK(dynamicMultiplier(2, ratio(47.5), 4) == 2);
    CHECK(dynamicMultiplier(2, ratio(46), 4) == 3);
}

// needsEvidence (an X11 window's present feedback, which under Xwayland
// times our own presents): a steady cadence away from the system's refresh
// rate is not a grid; without the flag (a Wayland surface's feedback: real
// display times) one frame a refresh still is, whatever the hint — the
// panel reads the fastest monitor, the game may be on another.
TEST(display_clock_needs_evidence_only_where_asked) {
    uint32_t s = 13;
    DisplayClock plain, x11;
    plain.setHint(144.0);
    x11.setHint(144.0);
    x11.setNeedsEvidence(true);
    for (int i = 0; i < 96; ++i) {
        const double t = i * (1000.0 / 80.0) + jitter(s, 0.15);
        plain.add(t);
        x11.add(t);
    }
    CHECK(plain.valid() && std::fabs(plain.refreshHz() - 80.0) < 0.5);
    CHECK(!x11.valid());
    // Presents replaced within a refresh: they outnumber the refreshes, the
    // times are the display's.
    DisplayClock fast;
    fast.setHint(144.0);
    fast.setNeedsEvidence(true);
    for (int i = 0; i < 96; ++i) {
        if (i % 8 == 0) fast.noteReplaced();
        fast.add(i * (1000.0 / 80.0) + jitter(s, 0.15));
    }
    CHECK(fast.valid() && std::fabs(fast.refreshHz() - 80.0) < 0.5);
}
