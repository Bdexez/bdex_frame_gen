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
