// The pacing's decisions (layer/src/pacing.h) against a simulated game, GPU
// and screen — no GPU, no window, a few milliseconds: the scenarios that
// broke on real hardware before (frames replaced before display, the
// latency limiter, the adaptive multiplier, skip_fast flapping) as
// properties of the code the layer runs.
#include "config.h"
#include "frame_order.h"
#include "pacing.h"
#include "test.h"

#include <cmath>
#include <cstdint>
#include <vector>

using bdex::Config;
using bdex::FrameOrder;
namespace pacing = bdex::pacing;

namespace {

// A mailbox screen on a fixed refresh grid: a present shows on the first
// refresh at or after it, and replaces a frame presented for the same
// refresh. The worker presents each frame of a job at its target (or as soon
// as it can after the previous present).
struct Screen {
    explicit Screen(double period, bool gridKnown = true) : periodMs(period), known(gridKnown) {}
    double periodMs;
    bool known;  // the layer has the refresh grid (present feedback); else only the rate (the panel's)
    struct Shown { double presentMs, displayMs, contentMs; bool real; };
    std::vector<Shown> frames;  // presented, in order (replaced ones included)
    int replaced = 0;
    double lastPresentMs = 0.0, lastLandMs = 0.0;
    pacing::Grid screenGrid() const { return {true, 0.0, periodMs}; }
    pacing::Grid grid() const { return {known, 0.0, periodMs}; }  // what the layer sees

    void present(double atMs, double contentMs, bool real) {
        const double t = std::max(atMs, lastPresentMs + 0.01);
        const double land = screenGrid().land(t);
        if (!frames.empty() && frames.back().displayMs == land) {
            frames.back().displayMs = -1.0;  // replaced before being displayed
            ++replaced;
        }
        frames.push_back({t, land, contentMs, real});
        lastPresentMs = t;
        lastLandMs = known ? land : 0.0;
    }
    int displayed() const {
        int n = 0;
        for (const auto& f : frames) n += f.displayMs >= 0.0;
        return n;
    }
};

// One present of the game at `presentMs`, its GPU work done `gpuMs` later:
// frames as FrameOrder places them, each showing the content of its t
// (content time = previous frame + t * interval).
void runJob(Screen& s, const FrameOrder& order, double presentMs, double gpuMs, double intervalMs, int frames) {
    const double origin = presentMs + gpuMs;
    for (int k = 0; k < frames; ++k) {
        const double at = pacing::presentTargetMs(origin, intervalMs, frames, order, k, s.lastPresentMs, s.lastLandMs, s.grid());
        const bool real = k == order.real;
        double t = 1.0;
        if (!real) t = order.t(k < order.real ? k : k - 1);
        s.present(at, presentMs - intervalMs + t * intervalMs, real);
    }
}

FrameOrder orderFor(int m, float hold, float cfgHold, double hz, double intervalMs) {
    Config c;
    c.interpHold = cfgHold;
    FrameOrder o = FrameOrder::of(c, m, true);
    o.hold = pacing::effectiveHold(o.hold == 1.f ? hold : o.hold, cfgHold, hz, m, intervalMs, o.real);
    return o;
}

} // namespace

// x2 at 60 fps on 144 Hz, evenly spaced: every frame displayed, generated and
// real alternating — the base the other cases are measured against.
TEST(pacing_even_x2_every_frame_displayed) {
    Screen s(1000.0 / 144);
    const double I = 1000.0 / 60;
    const FrameOrder o = orderFor(2, 1.f, 1.f, 144, I);
    for (int f = 1; f <= 120; ++f) runJob(s, o, f * I, 1.5, I, 2);
    CHECK(s.replaced == 0);
    CHECK(s.displayed() == 240);
}

// interp_hold: asked for 0.25 at 60 fps x2 on 144 Hz, the frames would come
// a quarter of the usual spacing apart — inside one refresh — and replace
// each other where the layer knows the refresh rate but not its grid (the
// panel's rate, as measured in headless gamescope: 0.25 showed 95 of 120
// frames); the bound (one refresh apart) keeps them all.
TEST(pacing_interp_hold_bounded_by_the_refresh) {
    const double I = 1000.0 / 60, hz = 144;
    Config c;
    c.interpHold = 0.25f;
    FrameOrder raw = FrameOrder::of(c, 2, true);  // without the bound
    Screen a(1000.0 / hz, false);
    for (int f = 1; f <= 120; ++f) runJob(a, raw, f * I, 1.5, I, 2);
    CHECK(a.replaced > 0);
    const FrameOrder bounded = orderFor(2, 1.f, 0.25f, hz, I);
    CHECK(bounded.hold > 0.8f && bounded.hold < 1.f);
    Screen b(1000.0 / hz, false);
    for (int f = 1; f <= 120; ++f) runJob(b, bounded, f * I, 1.5, I, 2);
    CHECK(b.replaced == 0);
}

// interp_hold = auto at 30 fps x2 on 144 Hz: every frame still displayed,
// the real frame on screen at least 2 ms sooner than evenly spaced, and each
// frame's content the same distance behind its display (motion stays right).
TEST(pacing_interp_hold_auto_sooner_real_frame) {
    const double I = 1000.0 / 30, hz = 144;
    const FrameOrder even = orderFor(2, 1.f, 1.f, hz, I);
    const FrameOrder held = orderFor(2, 1.f, -1.f, hz, I);
    CHECK(held.hold < 1.f);
    Screen a(1000.0 / hz), b(1000.0 / hz);
    for (int f = 1; f <= 60; ++f) {
        runJob(a, even, f * I, 2.0, I, 2);
        runJob(b, held, f * I, 2.0, I, 2);
    }
    CHECK(a.replaced == 0 && b.replaced == 0);
    double holdA = 0, holdB = 0;
    int n = 0;
    for (size_t i = 0; i < a.frames.size(); ++i)
        if (a.frames[i].real) {
            holdA += a.frames[i].displayMs - a.frames[i].contentMs;
            holdB += b.frames[i].displayMs - b.frames[i].contentMs;
            ++n;
        }
    CHECK(holdA / n - holdB / n >= 2.0);
    // (content-to-display lag of the generated frames within a refresh of the real ones')
    for (size_t i = 1; i < b.frames.size(); ++i) {
        const auto& f = b.frames[i];
        if (f.real || i + 1 >= b.frames.size()) continue;
        const auto& r = b.frames[i + 1];
        CHECK(std::abs((f.displayMs - f.contentMs) - (r.displayMs - r.contentMs)) <= 1000.0 / hz + 0.5);
    }
    // auto at 60 fps x2 on 144 Hz: under 2 ms to gain — evenly spaced.
    CHECK(orderFor(2, 1.f, -1.f, hz, 1000.0 / 60).hold == 1.f);
    // and nothing without a known refresh rate
    CHECK(orderFor(2, 1.f, -1.f, 0.0, I).hold == 1.f);
}

// Adaptive, 50 fps into 144 Hz: 3, 3, 2... frames a game frame, one refresh
// apart — 144 frames a second on screen, none replaced.
TEST(pacing_adaptive_fills_the_refresh_rate) {
    const double hz = 144, P = 1000.0 / hz, I = 1000.0 / 50;
    Screen s(P);
    pacing::AdaptivePacer ap;
    Config c;
    int total = 0;
    for (int f = 1; f <= 100; ++f) {  // two seconds
        double period = P;
        const int n = ap.next(I, period, 8);
        total += n;
        const FrameOrder o = FrameOrder::of(c, n);  // adaptive: no interp_hold
        runJob(s, o, f * I, 1.5, period > 0 ? period * n : I, n);
    }
    CHECK(total >= 286 && total <= 289);  // 2 s x 144
    CHECK(s.replaced == 0);
}

// latency_limiter, in closed loop: a GPU-bound game 8 ms ahead of the GPU
// (16.7 ms frames) waits 8 ms minus what it was held. The hold settles just
// under its lead — the slack left — and the wait at about the slack; never
// past 3/4 of a frame; once the GPU is idle (no wait at all), it halves away.
TEST(pacing_latency_limiter_converges_and_backs_off) {
    pacing::StartLimiter l;
    const double I = 1000.0 / 60, lead = 8.0;
    double d = 0, wait = lead;
    for (int i = 0; i < 200; ++i) {
        wait = std::max(0.0, lead - d);
        d = l.step(wait, I);
    }
    CHECK(d > 4.0 && d < lead);
    CHECK(wait > 0.05 && wait < 3.0);
    for (int i = 0; i < 400; ++i) d = l.step(30.0, I);  // a lead longer than a frame
    CHECK(d <= 0.75 * I + 1e-9);
    for (int i = 0; i < 20; ++i) d = l.step(0.0, I);
    CHECK(d == 0.0);
}

// skip_fast: 130 fps on 144 Hz goes idle after half a second, once; between
// the thresholds (80 %) nothing changes either way; back under 75 % for half
// a second, generation resumes.
TEST(pacing_skip_fast_hysteresis) {
    pacing::FastGate g;
    bool toggled = false;
    int flips = 0;
    double now = 1000.0;
    for (int i = 0; i < 200; ++i, now += 1000.0 / 130) {
        g.update(true, 1000.0 / 130, 144, now, toggled);
        flips += toggled;
    }
    CHECK(g.idle && flips == 1);
    for (int i = 0; i < 200; ++i, now += 1000.0 / 115) {  // 80 %: inside the band
        g.update(true, 1000.0 / 115, 144, now, toggled);
        flips += toggled;
    }
    CHECK(g.idle && flips == 1);
    for (int i = 0; i < 100; ++i, now += 1000.0 / 100) {  // 69 %
        g.update(true, 1000.0 / 100, 144, now, toggled);
        flips += toggled;
    }
    CHECK(!g.idle && flips == 2);
    // The half second: not idle 0.3 s into a fast stretch, idle by 0.6 s.
    pacing::FastGate h;
    bool idleEarly = false;
    for (double t = 0.0; t < 600.0; t += 1000.0 / 130) {
        h.update(true, 1000.0 / 130, 144, 5000.0 + t, toggled);
        if (t < 300.0 && h.idle) idleEarly = true;
    }
    CHECK(!idleEarly && h.idle);
    CHECK(!g.update(true, 1000.0 / 130, 0.0, now, toggled));  // refresh rate unknown: never idle
    CHECK(!g.update(false, 1000.0 / 130, 144, now, toggled)); // off
}

// A job whose first frame cannot make the first refresh it could once ready
// counts as late; one on time does not.
TEST(pacing_late_job) {
    const pacing::Grid g{true, 0.0, 10.0};
    // origin 21: ready at 24 -> first land 30; the previous present landed on 30: on time
    CHECK(!pacing::jobLate(21.0, 30.0, 0.0, g));
    // the previous present still holds 40 (a refresh past it): late
    CHECK(pacing::jobLate(21.0, 40.0, 0.0, g));
    CHECK(pacing::jobLate(21.0, 30.0, 9.5, g));  // the job began a refresh late
}

// Whatever the jitter of the game and of the GPU: presents strictly ordered,
// never two on one refresh while the spacing allows it, each real frame
// after the generated ones before it.
TEST(pacing_jittered_game_properties) {
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / double(1u << 24); };
    const double hz = 144;
    for (int m : {2, 3, 4}) {
        Screen s(1000.0 / hz);
        const double I = m * 1000.0 / hz * 1.15;  // frames a bit more than a refresh apart
        const FrameOrder o = orderFor(m, 1.f, 1.f, hz, I);
        double t = 0;
        for (int f = 0; f < 300; ++f) {
            t += I * (0.9 + 0.2 * rnd());
            runJob(s, o, t, 0.5 + 2.0 * rnd(), I, m);
        }
        for (size_t i = 1; i < s.frames.size(); ++i) CHECK(s.frames[i].presentMs > s.frames[i - 1].presentMs);
        // (jitter may put two on one refresh now and then, rarely)
        CHECK(s.replaced * 50 < int(s.frames.size()));
    }
}

// vrr_hold: on a variable-refresh screen a frame shows when it is presented
// (no sooner than the top rate after the one before; a frame still waiting
// is replaced by a newer one). Auto hold there at 40 fps x2, 144 Hz top:
// every frame shown, the real frame ~3.8 ms sooner than evenly spaced (the
// bound: 1.25 refresh between the two frames, a hold of 0.69).
TEST(pacing_vrr_auto_hold) {
    struct Vrr {
        explicit Vrr(double m) : minMs(m) {}
        double minMs;
        std::vector<double> shown;  // display times (-1 replaced)
        std::vector<bool> real;
        double lastPresentMs = 0.0;
        int replaced = 0;
        void present(double t, bool isReal) {
            t = std::max(t, lastPresentMs + 0.01);
            double show = shown.empty() ? t : std::max(t, lastShown() + minMs);
            if (!shown.empty() && shown.back() > t) {  // the previous one still waiting
                shown.back() = -1.0;
                ++replaced;
                show = std::max(t, lastShown() + minMs);
            }
            shown.push_back(show);
            real.push_back(isReal);
            lastPresentMs = t;
        }
        double lastShown() const {
            for (auto it = shown.rbegin(); it != shown.rend(); ++it)
                if (*it >= 0.0) return *it;
            return -1e9;
        }
    };
    const double hz = 144, I = 1000.0 / 40;
    const FrameOrder even = orderFor(2, 1.f, 1.f, hz, I);
    const FrameOrder held = orderFor(2, 1.f, -1.f, hz, I);  // what vrr_hold asks for
    CHECK(held.hold < 1.f);
    Vrr a(1000.0 / hz), b(1000.0 / hz);
    const pacing::Grid none{};  // no grid on a VRR screen
    double realA = 0, realB = 0;
    int n = 0;
    for (int f = 1; f <= 80; ++f) {
        const double present = f * I, origin = present + 2.0;
        for (int k = 0; k < 2; ++k) {
            a.present(pacing::presentTargetMs(origin, I, 2, even, k, a.lastPresentMs, 0.0, none), k == 1);
            b.present(pacing::presentTargetMs(origin, I, 2, held, k, b.lastPresentMs, 0.0, none), k == 1);
        }
        realA += a.shown.back() - present;
        realB += b.shown.back() - present;
        ++n;
    }
    CHECK(a.replaced == 0 && b.replaced == 0);
    CHECK(realA / n - realB / n >= 3.0);
}

namespace {
// A 30 fps game on a 144 Hz mailbox screen, the frames of each present given
// their content for the refresh the layer predicts (frame_timing) and
// presented as the worker does, the GPU work done 3 +- 1 ms after the present
// call. Returns the rms of (content step - display step) between displayed
// frames — the motion's judder — and counts the frames shown and repeated.
struct Cadence { double judderMs; double shownPerS; double repeatsPerS; };
Cadence runCadence(bool adaptive, bool plan, double gridPhase, bool screenGridKnown) {
    const double P = 1000.0 / 143.912, I = 1000.0 / 30;
    const pacing::Grid screen{true, 0.0, P};
    const pacing::Grid seen{true, screenGridKnown ? 0.0 : gridPhase * P, P};  // measured, or virtual_grid's
    pacing::AdaptivePacer ap;
    Config c;
    struct F { double display, content; };
    std::vector<F> fr;
    double lastPresent = 0, lastLand = 0, lastPlanned = 0, lag = 1.0;
    uint32_t rng = 12345;
    auto uniform = [&] { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / double(1 << 24); };
    for (int f = 1; f <= 600; ++f) {
        const double content = f * I, now = content + 2.0 + (uniform() - 0.5), gpu = 2.0 + 2.0 * uniform();
        double period = P;
        const int n = adaptive ? ap.next(I, period, 5) : 4;
        const FrameOrder o = FrameOrder::of(c, n, !adaptive);
        const double interval = adaptive ? period * n : I, slot = interval / n;
        double base[8] = {}, planned[8] = {}, at[8] = {};
        for (int k = 1; k < n; ++k) base[k] = slot * k;
        if (plan) {
            pacing::planLands(now, lag, base, n, seen, lastPlanned, planned);
            for (int k = 0; k < n; ++k) at[k] = planned[k];
        } else {  // (the layer before pace_plan)
            for (int k = 0; k < n; ++k) at[k] = seen.land(now + (k == 0 ? 1.0 : base[k] + (adaptive ? 1.0 : 0.0)));
        }
        for (int k = 0; k < n; ++k) {
            double t = pacing::presentTargetMs(now + gpu, interval, n, o, k, lastPresent, lastLand, seen, planned[k]);
            t = std::max({t, now + gpu, lastPresent + 0.01});
            const double d = screen.land(t);
            if (!fr.empty() && fr.back().display == d) fr.back().display = -1.0;  // replaced
            fr.push_back({d, content + at[k] - at[o.real]});
            lastPresent = t;
            lastLand = seen.land(t);
        }
        lag += (gpu > lag ? 0.5 : 0.05) * (gpu - lag);
    }
    std::vector<F> v;
    for (const F& x : fr)
        if (x.display > 2000.0) v.push_back(x);
    double e2 = 0;
    int shown = 0, repeats = 0;
    const F* prev = nullptr;
    for (const F& x : v) {
        if (x.display < 0.0) continue;
        if (prev) {
            const double e = (x.content - prev->content) - (x.display - prev->display);
            e2 += e * e;
            repeats += std::round((x.display - prev->display) / P) > 1;
        }
        prev = &x;
        ++shown;
    }
    const double secs = (prev->display - 2000.0) / 1000.0;
    return {std::sqrt(e2 / std::max(1, shown - 1)), shown / secs, repeats / secs};
}
} // namespace

// pace_plan, 30 fps x4 on 144 Hz with the grid known: the content of each
// frame is that of the refresh it shows on. Predicted from the present call
// (the GPU still working then), a frame showed a refresh later than its
// content's in about half the presents.
TEST(pacing_plan_matches_content_to_display) {
    const Cadence before = runCadence(false, false, 0.0, true), after = runCadence(false, true, 0.0, true);
    CHECK(before.judderMs > 3.0);
    CHECK(after.judderMs < 2.0);
    CHECK(after.shownPerS > 119.0);
}

// virtual_grid: adaptive x4-x5 at 30 fps on 144 Hz, refresh grid unknown —
// a grid of our own at most phases of the screen's gives every refresh a
// frame (the phase where our presents meet the screen's latch gets frames
// replaced, which the layer moves away from).
TEST(pacing_virtual_grid_fills_every_refresh) {
    int good = 0;
    for (int i = 0; i < 10; ++i) {
        const Cadence c = runCadence(true, true, i / 10.0, false);
        good += c.shownPerS > 143.0 && c.repeatsPerS < 0.5 && c.judderMs < 1.6;
    }
    CHECK(good >= 8);
}
