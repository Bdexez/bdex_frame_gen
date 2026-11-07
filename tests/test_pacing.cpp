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

TEST(pacing_interval_filter_keeps_the_ema_under_jitter) {
    for (int pattern = 0; pattern < 3; ++pattern) {
        pacing::IntervalFilter filter;
        double expected = 1000.0 / 30, estimate = expected;
        for (int i = 0; i < 600; ++i) {
            const double dt = pattern == 0 ? 1000.0 / 30 + 2.0 * std::sin(double(i))
                            : pattern == 1 ? (i % 2 ? 20.0 : 140.0 / 3)
                            : i % 45 ? 1000.0 / 30 : 80.0;
            expected = expected * 0.8 + dt * 0.2;
            estimate = filter.update(estimate, dt);
            CHECK(estimate == expected);
        }
    }
}

TEST(pacing_interval_filter_confirms_rate_changes_and_resets_after_a_pause) {
    pacing::IntervalFilter filter;
    double estimate = 1000.0 / 30;
    estimate = filter.update(estimate, estimate);
    estimate = filter.update(estimate, 1000.0 / 60);
    CHECK(estimate > 29.0);  // one short frame is insufficient
    estimate = filter.update(estimate, 1000.0 / 60);
    CHECK(std::abs(estimate - 1000.0 / 60) < 1e-9);
    estimate = filter.update(estimate, 1000.0 / 30);
    CHECK(estimate < 21.0);
    estimate = filter.update(estimate, 1000.0 / 30);
    CHECK(std::abs(estimate - 1000.0 / 30) < 1e-9);
    estimate = filter.update(estimate, 1000.0 / 60);
    CHECK(filter.update(estimate, 500.0) == estimate);
    estimate = filter.update(estimate, 1000.0 / 60);
    CHECK(estimate > 25.0);  // the sample before the pause cannot confirm this one
    CHECK(filter.update(estimate, 0.0) == estimate);
    CHECK(filter.update(estimate, -1.0) == estimate);
}

TEST(pacing_interval_filter_counts_short_intervals_in_a_bursty_source) {
    pacing::IntervalFilter filter;
    double estimate = 16.6, expected = estimate;
    for (int i = 0; i < 600; ++i) {
        const double dt = i % 2 ? 0.1 : 16.5;
        expected = 0.8 * expected + 0.2 * dt;
        estimate = filter.update(estimate, dt);
        CHECK(estimate == expected);
    }
    CHECK(estimate < 10.0);  // both halves count, not only the 16.5 ms half
}

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

// A multiplier past the refresh rate: 60 fps x4 on 144 Hz, 240 frames for
// 144 refreshes. One refresh a frame, each present lasted four refreshes
// (28 ms of a 17 ms game frame), a backlog without end — in the layer, a
// real-only catch-up every few presents (headless gamescope: 24 game frames
// a second without generated frames). The frames before the real one now
// share refreshes: no backlog, no real frame replaced, every refresh a new
// frame; and the uncapped game frame rates around it the same.
TEST(pacing_multiplier_past_the_refresh_rate_no_backlog) {
    const double P = 1000.0 / 143.912;
    for (double fps : {40.0, 48.0, 60.0, 90.0}) {
        const double I = 1000.0 / fps;
        Screen s(P);
        const FrameOrder o = orderFor(4, 1.f, 1.f, 143.912, I);
        const int jobs = int(10.0 * fps);
        double worstLag = 0.0;
        for (int f = 1; f <= jobs; ++f) {
            runJob(s, o, f * I, 3.0, I, 4);
            if (f > 20) worstLag = std::max(worstLag, s.frames.back().displayMs - f * I);  // (the real frame, last)
        }
        int realShown = 0;
        for (const auto& fr : s.frames) realShown += fr.real && fr.displayMs >= 0.0;
        const double shownPerS = s.displayed() / (jobs * I / 1000.0);
        std::printf("x4 at %.0f fps on 144 Hz: real frame on screen at most %.1f ms after the present, %.1f frames a second shown, %d of %d real\n",
                    fps, worstLag, shownPerS, realShown, jobs);
        CHECK(worstLag < I + 2.0 * P);
        CHECK(realShown == jobs);
        CHECK(shownPerS > 0.97 * std::min(143.912, 4.0 * fps));
    }
}

// The worker learns a fence's completion time when it waits it. If it was
// still presenting the previous job, the origin is at least that previous
// present, not an ideal GPU timestamp in the past. Exercise that feedback
// loop at arbitrary rates, including a source faster than the screen.
TEST(pacing_fixed_multiplier_arbitrary_source_rate_no_worker_backlog) {
    for (double hz : {60.0, 90.0, 120.0, 143.912, 165.0, 240.0}) {
        const double P = 1000.0 / hz;
        for (int m = 2; m <= 8; ++m) {
            for (double fps : {23.976, 30.0, 36.2, 47.0, 59.94, 72.0, 90.0, 119.88, 144.0, 200.0, 300.0}) {
                const double I = 1000.0 / fps;
                Screen s(P);
                const FrameOrder o = orderFor(m, 1.f, 1.f, hz, I);
                pacing::Grid outputGrid = s.grid();
                outputGrid.valid = pacing::usesDisplayGrid(I, m, P, 0.0);
                double lastPlanned = 0.0, base[8] = {}, planned[8] = {};
                for (int k = 0; k < m; ++k) base[k] = I * k / m;
                double worstLag = 0.0;
                for (int f = 1; f <= 300; ++f) {
                    const double source = f * I;
                    // No GPU pressure: only the scheduling itself may hold
                    // the worker back. All chosen outputs must be submitted.
                    const double origin = std::max(source + 0.2, s.lastPresentMs);
                    if (outputGrid.valid)
                        pacing::planLands(source, 0.2, base, m, s.grid(), lastPlanned, planned, I, o.real);
                    for (int k = 0; k < m; ++k) {
                        const double target = pacing::presentTargetMs(origin, I, m, o, k, s.lastPresentMs,
                                                                      s.lastLandMs, outputGrid, planned[k], !outputGrid.valid);
                        s.present(std::max(origin, target), source - I + I * (k + 1) / m, k == o.real);
                    }
                    if (f > 50) worstLag = std::max(worstLag, s.lastPresentMs - source);
                }
                if (worstLag >= I + 3.0 * P)
                    std::printf("backlog: %.3f fps x%d on %.3f Hz: %.3f ms\n", fps, m, hz, worstLag);
                CHECK(worstLag < I + 3.0 * P);
                CHECK(s.frames.size() == size_t(300 * m));
                // When the source fits the screen, the next job must not
                // overwrite the real frame, even if its generated frames
                // outnumber the available refreshes.
                if (I >= P) {
                    int realShown = 0;
                    for (const auto& fr : s.frames) realShown += fr.real && fr.displayMs >= 0.0;
                    CHECK(realShown == 300);
                }
            }
        }
    }
}

TEST(pacing_prediction_modes_do_not_reserve_more_refreshes_than_available) {
    for (double hz : {60.0, 143.912, 240.0}) {
        const double P = 1000.0 / hz;
        for (int m : {3, 4, 8}) {
            for (double sourceRatio : {0.4, 0.5, 0.6, 0.95, 1.0, 1.1, 2.0}) {
                const double I = P / sourceRatio;
                for (int mode = 0; mode < 3; ++mode) {
                    Config c;
                    c.extrapolate = mode == 0;
                    c.hybrid = mode != 0;
                    c.hybridPredict = mode == 2 ? 1 : 0;
                    const FrameOrder order = FrameOrder::of(c, m);
                    Screen s(P);
                    pacing::Grid outputGrid = s.grid();
                    outputGrid.valid = pacing::sourceFitsGrid(I, P);
                    double base[8] = {}, planned[8] = {}, lastPlanned = 0.0;
                    for (int k = 0; k < m; ++k) base[k] = I * order.offset(k);
                    double worstLag = 0.0;
                    for (int f = 1; f <= 300; ++f) {
                        const double source = f * I;
                        const double origin = std::max(source + 0.2, s.lastPresentMs);
                        // Extrapolation has no plan; hybrid uses the same
                        // absolute planning as interpolation in the layer.
                        if (!c.extrapolate && outputGrid.valid)
                            pacing::planLands(source, 0.2, base, m, s.grid(), lastPlanned, planned, I, order.real);
                        for (int k = 0; k < m; ++k) {
                            // Pure extrapolation presents the real image
                            // immediately, and cancels a prediction once
                            // the next real image is ready (runJob).
                            const double target = c.extrapolate && k == 0 ? origin
                                : pacing::presentTargetMs(origin, I, m, order, k, s.lastPresentMs,
                                                          s.lastLandMs, outputGrid, planned[k], !outputGrid.valid);
                            if (k > order.real && target >= source + I + 0.2) break;
                            s.present(std::max(origin, target), source, k == order.real);
                        }
                        if (f > 50) worstLag = std::max(worstLag, s.lastPresentMs - source);
                    }
                    if (worstLag >= I + 3.0 * P)
                        std::printf("prediction backlog: %.3f Hz, x%d, source/Hz %.2f, mode %d: %.3f ms\n",
                                    hz, m, sourceRatio, mode, worstLag);
                    CHECK(worstLag < I + 3.0 * P);
                }
            }
        }
    }
}

// Never assume a stable or capped source: ramps, alternating frame times,
// sustained rate changes and a single slow frame cross the xN/Hz boundary
// repeatedly. Keep the actual content clock separate from the pacing EMA.
TEST(pacing_fixed_multiplier_variable_source_keeps_count_and_bounded_latency) {
    for (double hz : {60.0, 143.912, 240.0}) {
        const double P = 1000.0 / hz;
        for (int m = 2; m <= 8; ++m) {
            Screen s(P);
            pacing::IntervalFilter filter;
            double now = 100.0, estimate = 1000.0 / 30, lastPlanned = 0.0;
            bool tight = false;
            double worstLag = 0.0;
            for (int f = 0; f < 1500; ++f) {
                const double fps = f < 300 ? 24.0 + f * 0.6
                                 : f < 600 ? (f % 2 ? 45.0 : 115.0)
                                 : f < 900 ? 23.976
                                 : f < 1200 ? 144.0 : 300.0;
                const double dt = f == 650 ? 120.0 : 1000.0 / fps;
                now += dt;
                estimate = filter.update(estimate, dt);
                FrameOrder order = orderFor(m, 1.f, 1.f, hz, estimate);
                order.hold = pacing::tightHold(P, m, estimate, order.real, tight);
                tight = order.hold < 1.f;
                double base[8] = {}, planned[8] = {};
                for (int k = 0; k < m; ++k) base[k] = estimate * order.offset(k);
                pacing::Grid outputGrid = s.grid();
                outputGrid.valid = pacing::usesDisplayGrid(estimate, m, P, 0.0);
                if (outputGrid.valid)
                    pacing::planLands(now, 0.2, base, m, s.grid(), lastPlanned, planned, estimate, order.real);
                else
                    lastPlanned = 0.0;
                const double origin = std::max(now + 0.2, s.lastPresentMs);
                for (int k = 0; k < m; ++k) {
                    const double target = pacing::presentTargetMs(origin, estimate, m, order, k, s.lastPresentMs,
                                                                  s.lastLandMs, outputGrid, planned[k], !outputGrid.valid);
                    s.present(std::max(origin, target), now, k == order.real);
                }
                worstLag = std::max(worstLag, s.lastPresentMs - now);
            }
            CHECK(s.frames.size() == size_t(1500 * m));
            CHECK(worstLag < 120.0 + 4.0 * P);
            CHECK(s.lastPresentMs - now < 1000.0 / 300 + 2.0 * P);
        }
    }
}

TEST(pacing_latency_limiter_at_high_source_rates) {
    for (double fps : {240.0, 360.0, 1000.0}) {
        const double I = 1000.0 / fps;
        pacing::StartLimiter limiter;
        for (int f = 0; f < 100; ++f) {
            const double delay = limiter.step(f % 5 ? 0.5 * I : 0.0, I);
            CHECK(std::isfinite(delay) && delay >= 0.0 && delay <= 0.75 * I);
            CHECK(limiter.slackMs >= 0.0 && limiter.slackMs <= 0.3 * I);
        }
    }
}

TEST(pacing_sub_refresh_output_does_not_add_a_slot_after_observed_readiness) {
    const FrameOrder order = orderFor(8, 1.f, 1.f, 144.0, 5.0);
    const pacing::Grid noGrid;
    CHECK(pacing::presentTargetMs(10.0, 5.0, 8, order, 0, 9.9, 0.0, noGrid, 0.0, true) == 10.0);
    CHECK(pacing::presentTargetMs(10.0, 5.0, 8, order, 1, 10.0, 0.0, noGrid, 0.0, true) == 10.625);
    // Without a known oversupply, keep the historical half-slot guard.
    CHECK(pacing::presentTargetMs(10.0, 5.0, 8, order, 0, 9.9, 0.0, noGrid) > 10.0);
}

TEST(pacing_continuous_source_backpressure_keeps_the_fixed_multiplier) {
    const double I = 1000.0 / 300, P = 1000.0 / 144;
    CHECK(!pacing::sourceFitsGrid(I, P));
    // The uncapped producer reaches the worker's throughput; every new job
    // waits for submissions. Dropping synthesis on those waits oscillated
    // between x4 and x1 (about x2 overall) despite continuous deadlines.
    CHECK(!pacing::needsCatchUp(0.8 * I, I, false, false, true));
    CHECK(!pacing::needsCatchUp(0.8 * I, I, true, false, true));
    // Automatic modes preserve recovery, including under a Steam display gate.
    CHECK(pacing::needsCatchUp(0.8 * I, I, false, false, false));
    CHECK(!pacing::needsCatchUp(0.8 * I, I, false, true, false));
    CHECK(pacing::needsCatchUp(0.0, I, true, true, false));
    // Fixed multiplication also keeps synthesis when source < Hz; even a
    // persistently late display must not silently change the chosen count.
    CHECK(!pacing::needsCatchUp(0.8 * I, I, false, false, true));
    CHECK(!pacing::needsCatchUp(0.8 * I, I, true, false, true));
}

// Model an uncapped producer as well as its worker: the source interval
// includes the previous present's submission wait. With a refresh plan on
// every job this could settle at 72/96/144 fps on 144 Hz with an idle GPU.
// An externally fixed source in the other simulations cannot expose that
// feedback loop.
TEST(pacing_uncapped_producer_does_not_lock_to_its_own_refresh_plan) {
    for (double hz : {60.0, 143.912, 240.0}) {
        const double P = 1000.0 / hz;
        for (int m : {3, 4, 8}) {
            Screen s(P);
            pacing::IntervalFilter filter;
            double interval = 16.6, source = 100.0, previousSource = 0.0;
            pacing::GridPacer gridPacer;
            double submitted = 0.0, workerEnd = 0.0, readyLag = 1.0, lastPlanned = 0.0;
            double measuredStart = 0.0;
            for (int f = 0; f < 600; ++f) {
                const double prepared = std::max(source, submitted);
                const double workerWait = prepared - source;
                interval = filter.update(interval, source - previousSource);
                previousSource = source;
                const FrameOrder order = orderFor(m, 1.f, 1.f, hz, interval);
                pacing::Grid g = s.grid();
                g.valid = gridPacer.use(interval, m, P, workerWait);
                double base[8] = {}, planned[8] = {};
                for (int k = 0; k < m; ++k) base[k] = interval * k / m;
                if (g.valid)
                    pacing::planLands(source, workerWait + readyLag, base, m, g, lastPlanned, planned, interval, order.real);
                else
                    lastPlanned = 0.0;
                const double gpuReady = prepared + 0.2;
                const double origin = std::max(workerEnd, gpuReady);
                const double lag = origin - prepared;
                if (g.valid && (gpuReady > workerEnd || lag < readyLag))
                    readyLag += (lag > readyLag ? 0.5 : 0.05) * (lag - readyLag);
                for (int k = 0; k < m; ++k) {
                    const double target = pacing::presentTargetMs(origin, interval, m, order, k, s.lastPresentMs,
                                                                  s.lastLandMs, g, planned[k], !g.valid);
                    s.present(std::max(origin, target), source, k == order.real);
                    if (k == 0) submitted = s.lastPresentMs;  // split synthesis: deferRest
                }
                workerEnd = s.lastPresentMs;
                source = prepared + 0.1;  // game CPU work, without an FPS cap
                if (f == 499) measuredStart = source;
            }
            const double fps = 100000.0 / (source - measuredStart);
            if (fps <= 1.1 * hz)
                std::printf("producer locked to its plan: x%d on %.3f Hz, %.3f fps\n", m, hz, fps);
            CHECK(fps > 1.1 * hz);
        }
    }
    CHECK(pacing::usesDisplayGrid(1000.0 / 30, 4, 1000.0 / 144, 0.0));
    CHECK(pacing::usesDisplayGrid(1000.0 / 60, 4, 1000.0 / 144, 0.0));
    CHECK(!pacing::usesDisplayGrid(1000.0 / 60, 4, 1000.0 / 144, 8.0));
    CHECK(!pacing::usesDisplayGrid(1000.0 / 200, 8, 1000.0 / 144, 0.0));
    pacing::GridPacer g;
    CHECK(!g.use(16.6, 4, 6.94, 12.0));
    for (int i = 0; i < 8; ++i) CHECK(!g.use(16.6, 4, 6.94, 0.0));
    CHECK(g.use(16.6, 4, 6.94, 0.0));
    CHECK(!g.use(16.6, 4, 6.94, 12.0));
    CHECK(g.use(33.3, 4, 6.94, 0.0));  // every output fits: restore immediately
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

// A plan chained on a grid whose phase then moves (virtual_grid's half
// period, x11_clock's grid replacing the virtual one) follows the new phase
// at once, instead of chaining every job on the old one.
TEST(pacing_plan_follows_a_moved_grid) {
    const double P = 1000.0 / 144;
    const double base[4] = {0.0, P, 2 * P, 3 * P};
    double lastPlanned = 0.0, lands[4];
    pacing::Grid a{true, 0.0, P};
    pacing::planLands(100.0, 3.0, base, 4, a, lastPlanned, lands);
    for (double l : lands) CHECK(std::fabs(std::remainder(l, P)) < 1e-6);
    // The grid now 0.6 period later; the next job would collide with the last
    // planned refresh (filling every refresh): still on the new grid.
    pacing::Grid b{true, 0.6 * P, P};
    pacing::planLands(lands[3] - 3.0 - 0.5 * P, 3.0, base, 4, b, lastPlanned, lands);
    for (double l : lands) CHECK(std::fabs(std::remainder(l - 0.6 * P, P)) < 1e-6);
    for (int k = 1; k < 4; ++k) CHECK(std::fabs(lands[k] - lands[k - 1] - P) < 1e-6);
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
struct Cadence { double judderMs; double shownPerS; double repeatsPerS; double worstStepMs; };
Cadence runCadence(bool adaptive, bool plan, double gridPhase, bool screenGridKnown, bool smooth = false) {
    const double P = 1000.0 / 143.912, I = 1000.0 / 30;
    const pacing::Grid screen{true, 0.0, P};
    const pacing::Grid seen{true, screenGridKnown ? 0.0 : gridPhase * P, P};  // measured, or virtual_grid's
    pacing::AdaptivePacer ap;
    pacing::MotionClock motion;
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
        const double span = motion.span(at[o.real], I, plan && smooth);
        for (int k = 0; k < n; ++k) {
            double t = pacing::presentTargetMs(now + gpu, interval, n, o, k, lastPresent, lastLand, seen, planned[k]);
            t = std::max({t, now + gpu, lastPresent + 0.01});
            const double d = screen.land(t);
            if (!fr.empty() && fr.back().display == d) fr.back().display = -1.0;  // replaced
            fr.push_back({d, content + (at[k] - at[o.real]) * (span > 0.0 ? I / span : 1.0)});
            lastPresent = t;
            lastLand = seen.land(t);
        }
        lag += (gpu > lag ? 0.5 : 0.05) * (gpu - lag);
    }
    std::vector<F> v;
    for (const F& x : fr)
        if (x.display > 2000.0) v.push_back(x);
    double e2 = 0, worst = 0;
    int shown = 0, repeats = 0;
    const F* prev = nullptr;
    for (const F& x : v) {
        if (x.display < 0.0) continue;
        if (prev) {
            const double e = (x.content - prev->content) - (x.display - prev->display);
            e2 += e * e;
            worst = std::max(worst, std::abs(e));
            repeats += std::round((x.display - prev->display) / P) > 1;
        }
        prev = &x;
        ++shown;
    }
    const double secs = (prev->display - 2000.0) / 1000.0;
    return {std::sqrt(e2 / std::max(1, shown - 1)), shown / secs, repeats / secs, worst};
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

// The refresh cadence can be perfect while motion jumps at each change
// between x4 and x5. The previous real's planned refresh is the other end
// of the interpolation interval: spread its phase error across the pair.
TEST(pacing_phase_smooth_reduces_motion_jumps_without_delaying_frames) {
    const Cadence a = runCadence(true, true, 0.0, true);
    const Cadence b = runCadence(true, true, 0.0, true, true);
    CHECK(b.judderMs < 0.75 * a.judderMs);
    CHECK(b.worstStepMs < 0.5 * a.worstStepMs);
    CHECK(a.shownPerS == b.shownPerS);
    CHECK(a.repeatsPerS == b.repeatsPerS);
    std::printf("phase_smooth: motion step RMS %.3f -> %.3f ms, worst %.3f -> %.3f ms\n",
                a.judderMs, b.judderMs, a.worstStepMs, b.worstStepMs);
}

TEST(pacing_motion_clock_resets_on_discontinuities) {
    pacing::MotionClock c;
    CHECK(c.span(100.0, 30.0, true) == 0.0);  // first pair, no display anchor
    CHECK(c.span(130.0, 30.0, true) == 30.0);
    CHECK(c.span(165.0, 30.0, true) == 35.0);
    CHECK(c.span(200.0, 30.0, false) == 0.0);  // pause, no plan, prediction or dedup
    CHECK(c.span(230.0, 30.0, true) == 0.0);
    CHECK(c.span(1000.0, 30.0, true) == 0.0);  // history/grid change or long pause
    CHECK(c.span(1030.0, 30.0, true) == 30.0);
    CHECK(c.span(1020.0, 30.0, true) == 0.0);  // clock moved backwards
    CHECK(c.span(1040.0, 0.0, true) == 0.0);
}

TEST(pacing_plan_slip_reports_new_whole_refresh_delay_once) {
    pacing::PlanSlip s;
    CHECK(!s.update(100.0, 100.0, 10.0, true));
    CHECK(s.update(140.0, 150.0, 10.0, true));
    CHECK(!s.update(180.0, 190.0, 10.0, true));
    CHECK(!s.update(220.0, 230.0, 10.0, true));
    CHECK(s.update(260.0, 280.0, 10.0, true));
    CHECK(!s.update(300.0, 300.0, 10.0, true));
    CHECK(s.update(340.0, 350.0, 10.0, true));
    CHECK(!s.update(380.0, 395.0, 10.0, true)); // half-period grid phase change
    CHECK(!s.update(420.0, 430.0, 10.0, true)); // new baseline
    CHECK(!s.update(460.0, 480.0, 20.0, true)); // a different refresh rate
    CHECK(!s.update(500.0, 540.0, 20.0, false));
    CHECK(!s.update(540.0, 580.0, 20.0, true)); // resume, no event
    CHECK(!s.update(0.0, 620.0, 20.0, true));
    CHECK(!s.update(620.0, 640.0, 0.0, true));
    CHECK(!s.update(620.0, 640.0, INFINITY, true));
}

namespace {
struct PlanRecovery { double excessMs = 0.0, worstRecoveryStep = 0.0; int lateFrames = 0, trims = 0, outputs = 0; };

// At 36 fps on 144 Hz, one synthesis misses its first refresh. Subsequent
// jobs keep the added refresh until catch-up: eight late jobs, or a request
// from the worker as soon as the plan gains that extra refresh. Re-space
// the next source pair over three outputs; do not drop an existing output.
PlanRecovery runPlanRecovery(bool prompt) {
    const double P = 1000.0 / 144, I = 4 * P;
    Screen screen(P);
    pacing::MotionClock motion;
    pacing::PlanSlip slip;
    Config cfg;
    double lastPlanned = 0.0, readyLag = 0.3 * P, previousContent = 0.0;
    int late = 0;
    bool requested = false;
    PlanRecovery result;
    for (int f = 1; f <= 40; ++f) {
        const double now = f * I;
        const bool catchup = late >= 8 || (prompt && requested && readyLag < 0.5 * I);
        requested = false;
        if (catchup) late = 0;
        const int n = catchup ? pacing::catchUpFrames(4, true, 0.0, I, P, readyLag) : 4;
        const FrameOrder order = FrameOrder::of(cfg, n);
        double base[8] = {}, planned[8] = {};
        for (int k = 0; k < n; ++k) base[k] = k * P;
        pacing::planLands(now, readyLag, base, n, screen.grid(), lastPlanned, planned);
        double span = motion.span(planned[n - 1], I, true);
        if (!(span > 0.0)) span = I;
        const double start = std::max(now, screen.lastPresentMs);
        const double gpu = now + (f == 10 ? 1.6 : 0.3) * P;
        const double origin = std::max(start, gpu), measured = origin - now;
        if (start < gpu || measured < readyLag)
            readyLag += (measured > readyLag ? 0.5 : 0.05) * (measured - readyLag);
        for (int k = 0; k < n; ++k) {
            const double target = pacing::presentTargetMs(origin, n * P, n, order, k,
                screen.lastPresentMs, screen.lastLandMs, screen.grid(), planned[k]);
            const double content = now + (planned[k] - planned[n - 1]) * I / span;
            screen.present(std::max(target, origin), content, k == n - 1);
            if (f > 10 && f < 30)
                result.worstRecoveryStep = std::max(result.worstRecoveryStep, content - previousContent);
            previousContent = content;
            if (k == 0) {
                if (slip.update(planned[0], screen.lastLandMs, P, true) && measured < 0.5 * I) requested = true;
                if (pacing::jobLate(origin, screen.lastLandMs, start - now, screen.grid())) ++late;
                else late = 0;
            }
            ++result.outputs;
        }
        if (f >= 10 && f < 30) {
            const double extra = std::max(0.0, screen.lastLandMs - now - I);
            result.excessMs += extra;
            result.lateFrames += extra > 0.5 * P;
            result.trims += catchup;
        }
    }
    return result;
}
}

TEST(pacing_new_plan_slip_recovers_without_eight_late_jobs) {
    const PlanRecovery a = runPlanRecovery(false), b = runPlanRecovery(true);
    // The conservative detector can reset near its early-present boundary;
    // its exact delay depends on phase, but never improves on eight jobs.
    CHECK(a.lateFrames >= 8 && b.lateFrames == 1);
    CHECK(b.excessMs < 0.2 * a.excessMs);
    CHECK(b.worstRecoveryStep <= a.worstRecoveryStep + 1e-6);
    CHECK(a.trims == 1 && b.trims == 1 && a.outputs == b.outputs);
    std::printf("new plan slip: added refresh persists %d -> %d source frames, recovery step %.3f -> %.3f ms\n",
                a.lateFrames, b.lateFrames, a.worstRecoveryStep, b.worstRecoveryStep);
}

TEST(pacing_refresh_fill_covers_integer_ratios_and_caps_output) {
    for (double fps : {30.0, 36.0, 36.2, 37.0, 40.0, 48.0, 50.0, 60.0})
        CHECK(pacing::refreshFillFits(143.912 / fps, 5));
    CHECK(!pacing::refreshFillFits(143.912 / 20.0, 5));  // too few allocated outputs
    CHECK(!pacing::refreshFillFits(143.912 / 100.0, 5)); // single-frame jobs would lose the virtual grid
    Config c;
    c.multiplier = 4;
    // (off by default since 20/10: x4 is 4 frames a game frame, 120 fps at 30)
    CHECK(!c.allowsRefreshFill() && c.frameCeiling() == 4);
    c.refreshFill = 1;
    CHECK(c.allowsRefreshFill() && c.frameCeiling() == 5);
    c.multiplier = 8;
    CHECK(c.allowsRefreshFill() && c.frameCeiling() == 8);
    c.refreshFill = 0;
    CHECK(!c.allowsRefreshFill());
    c.refreshFill = 1; c.multiplier = 2;
    CHECK(!c.allowsRefreshFill());
    c.multiplier = 4; c.hybrid = true;
    CHECK(!c.allowsRefreshFill());
    c.hybrid = false; c.extrapolate = true;
    CHECK(!c.allowsRefreshFill());
}

namespace {
// The user's fixed x4, on a compositor without a measurable refresh grid.
// Before: near/above 36 fps refresh_fill was inactive; each job starts from
// its own GPU completion. After: the same virtual grid and phase smoothing
// as the existing 30 fps path, fewer outputs when x4 would exceed the screen.
Cadence runRefreshCap(double fps, bool cap) {
    const double P = 1000.0 / 143.912, I = 1000.0 / fps;
    const bool adaptive = cap ? pacing::refreshFillFits(I / P, 5) : I / P > 4 * 1.02 && I / P <= 5 * 1.02;
    Screen screen(P, adaptive);
    pacing::AdaptivePacer pacer;
    pacing::MotionClock motion;
    Config cfg;
    double plannedLast = 0.0;
    uint32_t random = 12345;
    for (int f = 1; f <= 1000; ++f) {
        random = random * 1664525u + 1013904223u;
        const double now = f * I, ready = now + 2.0 + 2.0 * ((random >> 8) / double(1u << 24));
        double period = P;
        const int n = adaptive ? pacer.next(I, period, 5) : 4;
        const FrameOrder order = FrameOrder::of(cfg, n);
        const double interval = adaptive ? n * P : I;
        double base[8] = {}, lands[8] = {};
        for (int k = 0; k < n; ++k) base[k] = k * interval / n;
        if (adaptive) pacing::planLands(now, 3.6, base, n, screen.grid(), plannedLast, lands);
        const double span = motion.span(lands[order.real], I, adaptive);
        for (int k = 0; k < n; ++k) {
            const double target = pacing::presentTargetMs(ready, interval, n, order, k,
                screen.lastPresentMs, screen.lastLandMs, screen.grid(), lands[k]);
            const double content = adaptive
                ? now + (lands[k] - lands[order.real]) * I / (span > 0.0 ? span : I)
                : now + (k - order.real) * I / n;
            screen.present(std::max(ready, target), content, k == order.real);
        }
    }
    const Screen::Shown* prev = nullptr;
    double error2 = 0.0, worst = 0.0, start = 0.0, end = 0.0;
    int shown = 0, repeats = 0;
    for (const auto& frame : screen.frames) {
        if (frame.displayMs < 2000.0) continue;
        if (prev) {
            const double e = (frame.contentMs - prev->contentMs) - (frame.displayMs - prev->displayMs);
            error2 += e * e;
            worst = std::max(worst, std::abs(e));
            repeats += frame.displayMs - prev->displayMs >= 1.5 * P;
        } else start = frame.displayMs;
        prev = &frame; end = frame.displayMs; ++shown;
    }
    const double seconds = (end - start) / 1000.0;
    return {std::sqrt(error2 / (shown - 1)), shown / seconds, repeats / seconds, worst};
}
}

TEST(pacing_refresh_fill_near_screen_rate_reduces_motion_judder) {
    for (double fps : {36.0, 36.2, 37.0, 40.0, 48.0, 50.0}) {
        const Cadence a = runRefreshCap(fps, false), b = runRefreshCap(fps, true);
        CHECK(b.judderMs < 0.75 * a.judderMs);
        CHECK(b.repeatsPerS <= a.repeatsPerS);
        CHECK(b.shownPerS > 143.0);
        std::printf("refresh cap %.1f fps: motion RMS %.3f -> %.3f ms, held %.2f -> %.2f/s\n",
                    fps, a.judderMs, b.judderMs, a.repeatsPerS, b.repeatsPerS);
    }
}

TEST(pacing_catchup_keeps_interpolation_only_without_gpu_or_worker_pressure) {
    const double I = 1000.0 / 36.2, P = 1000.0 / 144;
    CHECK(pacing::catchUpFrames(4, true, 0.0, I, P, 8.0) == 3);
    CHECK(pacing::catchUpFrames(5, true, 0.0, I, P, 8.0) == 4);
    CHECK(pacing::catchUpFrames(3, true, 0.0, I, P, 8.0) == 2);
    CHECK(pacing::catchUpFrames(2, true, 0.0, I, P, 8.0) == 1);
    CHECK(pacing::catchUpFrames(4, false, 10.0, I, P, 8.0) == 1);
    CHECK(pacing::catchUpFrames(4, true, 10.0, I, P, 8.0) == 1);
    CHECK(pacing::catchUpFrames(4, true, 0.0, I, P, 20.0) == 1);
    CHECK(pacing::catchUpFrames(4, true, 0.0, I, 0.0, 8.0) == 1);
    // Four source periods of content, after giving the display one refresh
    // back: each of the three steps is still forward and far smaller than
    // the real-only jump; the anchor clock stays valid.
    pacing::MotionClock motion;
    CHECK(motion.span(100.0, I, true) == 0.0);
    const double span = motion.span(100.0 + 3 * P, I, true);
    CHECK(std::abs(span - 3 * P) < 1e-9);
    double previous = 0.0;
    for (int k = 1; k <= 3; ++k) {
        const double content = k * P * I / span;
        CHECK(content > previous);
        CHECK(content - previous < 0.5 * I);
        previous = content;
    }
    CHECK(std::abs(previous - I) < 1e-9);
}

namespace {
// A 30 fps game whose presents jitter like PRAGMATA's (27 to 39 ms, sd ~1.6)
// on a 143.912 Hz screen with the refresh grid known (x11_clock's lead), the
// content of a frame its present time. Each refresh's content step against
// its display step: pinned real frames (pace_plan + phase_smooth, refresh
// fill x4-x5) or even_motion.
struct Motion { double stepRmsMs, worstStepMs, holdsPerS, latencyMs, outputsPerFrame; };
Motion runMotion(bool even) {
    const double P = 1000.0 / 143.912, I = 1000.0 / 30.0;
    pacing::Grid g{true, 0.0, P};
    g.lead = pacing::kX11Lead;
    pacing::AdaptivePacer ap;
    pacing::MotionClock motion;
    pacing::IntervalFilter filter;
    pacing::EvenClock clock;
    Config c;
    struct F { double display, content; };
    std::vector<F> shown;
    double lastPresent = 0.0, lastLand = 0.0, lastPlanned = 0.0, lag = 4.0, interval = I, prevNow = 0.0;
    uint32_t rng = 777;
    auto uniform = [&] { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / double(1 << 24); };
    int outputs = 0, frames = 0;
    for (int f = 1; f <= 1200; ++f) {
        // (sum of three uniforms: about normal, sd 1.6, within +-5 ms)
        const double now = f * I + 1.6 * 2.0 * (uniform() + uniform() + uniform() - 1.5);
        const double dt = prevNow > 0.0 ? now - prevNow : I;
        prevNow = now;
        interval = filter.update(interval, dt);
        const double gpu = 3.0 + 3.0 * uniform();  // first frame ready
        double planned[8] = {}, content[8] = {};
        int n;
        if (even) {
            double realMs = 0.0;
            n = clock.plan(now, dt, lag, g, 6, lastPlanned, planned, realMs);
            for (int k = 0; k < n; ++k) content[k] = planned[k] - (realMs - now);
        } else {
            double period = P;
            n = ap.next(interval, period, 5);
            double base[8] = {};
            for (int k = 1; k < n; ++k) base[k] = k * P;
            pacing::planLands(now, lag, base, n, g, lastPlanned, planned);
            double span = motion.span(planned[n - 1], dt, true);
            if (!(span > 0.0)) span = dt;
            for (int k = 0; k < n; ++k) content[k] = now + (planned[k] - planned[n - 1]) * dt / span;
        }
        const FrameOrder o = FrameOrder::of(c, even ? n + 1 : n);
        const double origin = now + gpu;
        for (int k = 0; k < n; ++k) {
            double t = pacing::presentTargetMs(origin, n * P, n, o, k, lastPresent, lastLand, g, planned[k]);
            t = std::max({t, origin, lastPresent + 0.01});
            // (an X11 tick: what was presented since the last one goes out at the next)
            const double d = g.land(t);
            if (!shown.empty() && shown.back().display == d) shown.back() = {d, content[k]};
            else shown.push_back({d, content[k]});
            lastPresent = t;
            lastLand = d;
        }
        lag += (gpu > lag ? 0.5 : 0.05) * (gpu - lag);
        if (f > 100) outputs += n, ++frames;
    }
    double e2 = 0.0, worst = 0.0, lat = 0.0;
    int steps = 0, holds = 0;
    for (size_t i = 1; i < shown.size(); ++i) {
        if (shown[i].display < 100 * I) continue;
        const double gap = shown[i].display - shown[i - 1].display;
        if (gap > 1.5 * P) { ++holds; continue; }
        const double e = (shown[i].content - shown[i - 1].content) - gap;
        e2 += e * e;
        worst = std::max(worst, std::abs(e));
        lat += shown[i].display - shown[i].content;
        ++steps;
    }
    const double secs = (shown.back().display - 100 * I) / 1000.0;
    return {std::sqrt(e2 / std::max(1, steps)), worst, holds / secs, lat / std::max(1, steps), double(outputs) / frames};
}
} // namespace

// even_motion: every refresh its own moment — the content steps by one
// refresh's worth at each, where pinned real frames give pairs of 4 and 5
// refreshes (a quarter faster through one in five) and the game's jitter on
// top. The price: a generated frame in place of each real one and a few
// milliseconds of latency, never a refresh held for it.
TEST(pacing_even_motion_steps_every_refresh_alike) {
    const Motion pinned = runMotion(false), even = runMotion(true);
    std::printf("even_motion: content step error rms %.3f -> %.3f ms, worst %.3f -> %.3f, holds %.2f -> %.2f /s, "
                "latency %.1f -> %.1f ms, outputs a frame %.2f -> %.2f\n",
                pinned.stepRmsMs, even.stepRmsMs, pinned.worstStepMs, even.worstStepMs, pinned.holdsPerS, even.holdsPerS,
                pinned.latencyMs, even.latencyMs, pinned.outputsPerFrame, even.outputsPerFrame);
    CHECK(pinned.stepRmsMs > 0.5);
    CHECK(even.stepRmsMs < 0.05);
    CHECK(even.holdsPerS <= pinned.holdsPerS + 0.1);
    CHECK(even.latencyMs < pinned.latencyMs + 10.0);
}

namespace {
// Fixed x4 at 30 fps (presents jittered like PRAGMATA's) on a 143.912 Hz
// grid, pace_plan, each frame the content of its display time: the real
// frame's lag behind its content, refreshes held, frames replaced, content
// steps against display steps.
struct Hold { double realLagMs, holdsPerS, replacedPerS, stepRmsMs; };
Hold runHold(bool tight) {
    const double P = 1000.0 / 143.912, I = 1000.0 / 30.0;
    pacing::Grid g{true, 0.0, P};
    g.lead = pacing::kX11Lead;
    pacing::IntervalFilter filter;
    pacing::MotionClock motion;
    Config c;
    struct F { double display, content; bool real; };
    std::vector<F> shown;
    double lastPresent = 0.0, lastLand = 0.0, lastPlanned = 0.0, lag = 4.0, interval = I, prevNow = 0.0;
    uint32_t rng = 4242;
    auto uniform = [&] { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / double(1 << 24); };
    int replaced = 0;
    for (int f = 1; f <= 1200; ++f) {
        const double now = f * I + 1.6 * 2.0 * (uniform() + uniform() + uniform() - 1.5);
        const double dt = prevNow > 0.0 ? now - prevNow : I;
        prevNow = now;
        interval = filter.update(interval, dt);
        FrameOrder o = FrameOrder::of(c, 4);
        if (tight) o.hold = pacing::tightHold(P, 4, interval, o.real);
        double base[8] = {}, planned[8] = {};
        for (int k = 1; k < 4; ++k) base[k] = o.hold < 1.f ? interval * o.offset(k) : interval * k / 4;
        pacing::planLands(now, lag, base, 4, g, lastPlanned, planned);
        double span = motion.span(planned[3], dt, true);
        if (!(span > 0.0)) span = dt;
        const double gpu = 3.0 + 3.0 * uniform(), origin = now + gpu;
        for (int k = 0; k < 4; ++k) {
            double t = pacing::presentTargetMs(origin, interval, 4, o, k, lastPresent, lastLand, g, planned[k]);
            t = std::max({t, origin, lastPresent + 0.01});
            const double d = g.land(t);
            const double content = now + (planned[k] - planned[3]) * dt / span;
            if (!shown.empty() && shown.back().display == d) { shown.back() = {d, content, k == 3}; ++replaced; }
            else shown.push_back({d, content, k == 3});
            lastPresent = t;
            lastLand = d;
        }
        lag += (gpu > lag ? 0.5 : 0.05) * (gpu - lag);
    }
    double realLag = 0.0, e2 = 0.0;
    int reals = 0, holds = 0, steps = 0;
    for (size_t i = 1; i < shown.size(); ++i) {
        if (shown[i].display < 100 * I) continue;
        if (shown[i].real) realLag += shown[i].display - shown[i].content, ++reals;
        const double gap = shown[i].display - shown[i - 1].display;
        holds += int(std::round(gap / P)) - 1;
        const double e = (shown[i].content - shown[i - 1].content) - gap;
        e2 += e * e, ++steps;
    }
    const double secs = (shown.back().display - 100 * I) / 1000.0;
    return {realLag / std::max(1, reals), holds / secs, replaced / secs, std::sqrt(e2 / std::max(1, steps))};
}
} // namespace

// tight_hold, 30 fps x4 on 144 Hz: the generated frames one refresh apart,
// the real one sooner by about 4 ms; as many refreshes held (the screen has
// 4.8 for 4 frames), none replaced, the motion as even.
TEST(pacing_tight_hold_real_frame_sooner_same_holds) {
    const Hold even = runHold(false), tight = runHold(true);
    std::printf("tight_hold: real frame lag %.2f -> %.2f ms, held %.1f -> %.1f /s, replaced %.2f -> %.2f /s, "
                "content step error rms %.3f -> %.3f ms\n", even.realLagMs, tight.realLagMs, even.holdsPerS,
                tight.holdsPerS, even.replacedPerS, tight.replacedPerS, even.stepRmsMs, tight.stepRmsMs);
    CHECK(even.realLagMs - tight.realLagMs > 3.0);
    CHECK(tight.holdsPerS <= even.holdsPerS + 1.0);
    CHECK(tight.replacedPerS <= even.replacedPerS + 0.1);
    CHECK(tight.stepRmsMs <= even.stepRmsMs + 0.1);
    // (nothing to give: x4 at 36 fps fills 144 Hz; x2 at 30 fps would hold the real frame 3.8 refreshes)
    CHECK(pacing::tightHold(1000.0 / 143.912, 4, 1000.0 / 36.0, 3) == 1.f);
    CHECK(pacing::tightHold(1000.0 / 143.912, 2, 1000.0 / 30.0, 1) == 1.f);
    CHECK(pacing::tightHold(1000.0 / 143.912, 3, 1000.0 / 40.0, 2) < 1.f);
}

// tight_hold's hysteresis: a game hovering at a bound (x4 on 144 Hz around
// 32.9 fps, where the saving falls under 2 ms) keeps one spacing instead of
// switching present after present.
TEST(pacing_tight_hold_hysteresis) {
    const double P = 1000.0 / 143.912;
    uint32_t rng = 99;
    auto uniform = [&] { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / double(1 << 24); };
    int flipsPlain = 0, flipsHyst = 0;
    bool plain = false, hyst = false;
    for (int f = 0; f < 600; ++f) {
        const double I = 1000.0 / (32.9 + 0.6 * (uniform() - 0.5));  // 32.6 to 33.2 fps
        const bool p = pacing::tightHold(P, 4, I, 3) < 1.f, h = pacing::tightHold(P, 4, I, 3, hyst) < 1.f;
        flipsPlain += f > 0 && p != plain;
        flipsHyst += f > 0 && h != hyst;
        plain = p;
        hyst = h;
    }
    CHECK(flipsPlain > 50);
    CHECK(flipsHyst <= 2);
    CHECK(pacing::tightHold(P, 4, 1000.0 / 30.0, 3) < 1.f);  // (30 fps: on, either way)
}

namespace {
// even_fast against the pinned real frames, closed loop: a game at `fps`
// whose frame times jitter (sum of three uniforms, sd `jitterMs`), held at
// its present until the worker submitted the job before (as present()); its
// first frame ready 2.5 +- 1 ms after the present; the worker presenting
// each frame at its target, in order; an Xwayland-like screen on 143.912 Hz
// (x11_clock's lead) taking a present at the first refresh at least 1 ms
// after it, the later of two for one refresh replacing the other. Every
// refresh's content step against the period (a held refresh is a step of
// 0), motion jumps (a step over 1.6 times the usual), content delay behind
// the screen, presents per game frame.
struct Fast { double stepRmsMs, jumpsPerS, holdsPerS, delayMs, presentsPerFrame; int fastJobs; };
Fast runFast(double fps, double jitterMs, int m, bool fast, bool wide = false) {
    const double P = 1000.0 / 143.912;
    pacing::Grid g{true, 0.37, P};
    g.lead = pacing::kX11Lead;
    pacing::IntervalFilter filter;
    pacing::MotionClock motion;
    pacing::EvenClock clock;
    clock.fallback = true;
    Config c;
    const FrameOrder order = FrameOrder::of(c, m, true);
    struct Pr { double at, content; };
    std::vector<Pr> presents;
    uint32_t rng = 4242;
    auto uniform = [&] { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / double(1 << 24); };
    double call = 100.0, prevNow = 0.0, interval = 0.0, lastPlanned = 0.0, readyLag = 1.0, submitted = 0.0;
    double workerFree = 0.0, lastPresent = 0.0, lastLand = 0.0;
    bool on = false;
    int fastJobs = 0, frames = 0;
    const int count = int(fps * 20);
    for (int f = 0; f < count; ++f) {
        const double now = call, t0 = std::max(call, submitted);
        const double dt = prevNow > 0.0 ? now - prevNow : 0.0;
        prevNow = now;
        interval = filter.update(interval, dt);
        const double cpuWait = t0 - now + 0.15;
        const double gpuReady = now + cpuWait + 2.5 + 2.0 * (uniform() - 0.5);
        double planned[8] = {}, content[8] = {}, lands[8] = {};
        int n = 0;
        bool skip = false;
        if (f > 0 && fast && (on = pacing::evenFastBand(interval, P, on, wide && m >= 4))) {
            pacing::Grid gs = g;
            gs.readyMarginMs = pacing::kX11ReadyMarginMs;
            double realMs = 0.0;
            n = clock.plan(now, dt, cpuWait + readyLag, gs, m - 1, lastPlanned, lands, realMs);
            skip = n == 0 && clock.lagMs > 0.0;
            for (int k = 0; k + 1 < m && n > 0; ++k) {
                planned[k] = lands[std::min(n - 1, k * n / (m - 1))];
                content[k] = std::clamp(planned[k] - clock.lagMs, now - dt, now);
            }
            content[m - 1] = now;
            fastJobs += n > 0 || skip;
        } else if (f > 0) {
            clock.reset();
            double base[8] = {};
            for (int k = 1; k < m; ++k) base[k] = interval * k / m;
            pacing::planLands(now, cpuWait + readyLag, base, m, g, lastPlanned, planned, interval, order.real);
            const double span = motion.span(planned[order.real], dt, dt > 0.5 && dt < 500.0);
            for (int k = 0; k < m; ++k) {
                const double t = k == order.real ? 1.0
                    : std::clamp(1.0 + (planned[k] - planned[order.real]) / (span > 0.0 ? span : dt), 0.0, 1.0);
                content[k] = now - (1.0 - t) * dt;
            }
        }
        const int jobFrames = f == 0 ? 1 : m;
        const bool fallback = n > 0 || skip;
        if (skip)
            for (int k = 0; k < m; ++k) content[k] = now;
        const double origin = std::max(gpuReady, std::max(t0 + 0.15, workerFree));
        if (f > 0 && !skip) {
            const double lag = origin - (t0 + 0.15);
            if (lag < readyLag || gpuReady > workerFree) readyLag += (lag > readyLag ? 0.5 : 0.05) * (lag - readyLag);
        }
        double first = 0.0;
        for (int k = 0; k < jobFrames; ++k) {
            const bool real = jobFrames == 1 || k == order.real;
            const bool standIn = fallback && (real || skip);  // (Job::fallbackFrom)
            double target = origin;
            if (standIn) target = lastLand > 0.0 ? lastLand + pacing::kFallbackAfter * P : origin;
            else if (jobFrames > 1) target = pacing::presentTargetMs(origin, interval, m, order, k, lastPresent, lastLand, g, planned[k]);
            const double at = std::max({target, origin, lastPresent + 0.02});
            presents.push_back({at, jobFrames == 1 ? now : content[k]});
            lastPresent = at;
            if (!standIn) lastLand = g.land(at);
            if (k == 0) first = at;
        }
        workerFree = lastPresent;
        submitted = jobFrames > 1 && gpuReady > t0 + 0.15 ? first : t0 + 0.15;
        frames += jobFrames;
        // (the game: back from its present, then its own frame time)
        call = t0 + 0.1 + std::max(1.0, 1000.0 / fps + jitterMs * 2.0 * (uniform() + uniform() + uniform() - 1.5));
    }
    // The screen: each refresh shows the last present that made it.
    std::vector<double> shown;  // content, refresh by refresh
    size_t i = 0;
    double contentNow = 0.0;
    const double begin = 3000.0, end = presents.back().at - 100.0;
    double delay = 0.0;
    int ticks = 0;
    for (double tick = g.land(begin); tick < end; tick += P) {
        while (i < presents.size() && presents[i].at + 1.0 <= tick) contentNow = presents[i++].content;
        shown.push_back(contentNow);
        delay += tick - contentNow, ++ticks;
    }
    std::vector<double> steps;
    double e2 = 0.0;
    int holds = 0;
    for (size_t k = 1; k < shown.size(); ++k) {
        const double s = shown[k] - shown[k - 1];
        steps.push_back(s);
        e2 += (s - P) * (s - P);
        holds += s <= 1e-9;
    }
    std::vector<double> sorted;
    for (double s : steps) if (s > 0.0) sorted.push_back(s);
    std::sort(sorted.begin(), sorted.end());
    const double usual = sorted[sorted.size() / 2];
    int jumps = 0;
    for (size_t k = 1; k < steps.size(); ++k) jumps += steps[k - 1] > 0.0 && steps[k] > 1.6 * usual;
    const double secs = (end - begin) / 1000.0;
    return {std::sqrt(e2 / steps.size()), jumps / secs, holds / secs, delay / ticks, double(frames) / count, fastJobs};
}
} // namespace

// even_fast, a game frame of one to two refreshes: pinned real frames make
// pairs of one and two refreshes (100 fps on 144 Hz: steps of 10, then 5 and
// 5 ms of content a 6.9 ms refresh). Every refresh with the content of its
// own moment instead: the motion even, no jumps, the delay about the same;
// still x frames per game frame (the real one replaced on the refresh after).
TEST(pacing_even_fast_smooths_one_to_two_refreshes_a_frame) {
    for (int m : {3, 4, 8}) {
        for (double fps : {80.0, 95.0, 105.0, 120.0}) {
            const Fast pinned = runFast(fps, 1.0, m, false), even = runFast(fps, 1.0, m, true);
            std::printf("even_fast x%d %.0f fps: step rms %.2f -> %.2f ms, jumps %.1f -> %.1f /s, held %.1f -> %.1f /s, "
                        "delay %.1f -> %.1f ms\n", m, fps, pinned.stepRmsMs, even.stepRmsMs, pinned.jumpsPerS,
                        even.jumpsPerS, pinned.holdsPerS, even.holdsPerS, pinned.delayMs, even.delayMs);
            CHECK(even.stepRmsMs < 0.5 * pinned.stepRmsMs);
            CHECK(even.stepRmsMs < 0.8);
            CHECK(even.jumpsPerS < 1.0);
            CHECK(even.holdsPerS <= pinned.holdsPerS + 0.5);
            CHECK(even.delayMs < pinned.delayMs + 3.0);
            CHECK(std::abs(even.presentsPerFrame - m) < 0.02);
            CHECK(even.fastJobs > int(fps * 19));
        }
    }
}

// even_fast 2, x4 and up: two to three refreshes a frame as well (48 to 72
// fps on 144 Hz), where the pinned pairs alternate two and three refreshes.
// Smoother still, at the cost of the even clock's lag (a whole game frame).
TEST(pacing_even_fast_wide_band) {
    for (int m : {4, 8}) {
        for (double fps : {55.0, 60.0, 65.0}) {
            const Fast pinned = runFast(fps, 1.0, m, false), wide = runFast(fps, 1.0, m, true, true);
            std::printf("even_fast 2 x%d %.0f fps: step rms %.2f -> %.2f ms, held %.1f -> %.1f /s, delay %.1f -> %.1f ms\n",
                        m, fps, pinned.stepRmsMs, wide.stepRmsMs, pinned.holdsPerS, wide.holdsPerS, pinned.delayMs,
                        wide.delayMs);
            CHECK(wide.stepRmsMs < 0.5 * pinned.stepRmsMs);
            CHECK(wide.holdsPerS <= pinned.holdsPerS + 0.5);
            CHECK(wide.delayMs < pinned.delayMs + 6.0);
            CHECK(std::abs(wide.presentsPerFrame - m) < 0.02);
            // (level 1 leaves this band alone; x3 has too few generated frames)
            CHECK(runFast(fps, 1.0, m, true).fastJobs == 0);
        }
    }
    CHECK(runFast(60.0, 1.0, 3, true, true).fastJobs == 0);
    const double P = 1000.0 / 143.912;
    CHECK(pacing::evenFastBand(2.5 * P, P, false, true));
    CHECK(!pacing::evenFastBand(2.5 * P, P, false));
    CHECK(!pacing::evenFastBand(2.92 * P, P, false, true));
    CHECK(pacing::evenFastBand(2.92 * P, P, true, true));
    CHECK(!pacing::evenFastBand(2.97 * P, P, true, true));
}

// Outside its band (a whole number of refreshes a frame, a slower game, a
// game faster than the screen) or at x2, even_fast does nothing.
TEST(pacing_even_fast_only_inside_its_band) {
    for (double fps : {30.0, 47.0, 60.0, 71.5, 200.0}) {
        for (int m : {3, 4}) {
            const Fast a = runFast(fps, 0.3, m, false), b = runFast(fps, 0.3, m, true);
            CHECK(b.fastJobs == 0);
            CHECK(a.stepRmsMs == b.stepRmsMs && a.delayMs == b.delayMs);
        }
    }
    const double P = 1000.0 / 143.912;
    CHECK(!pacing::evenFastBand(1.06 * P, P, false));
    CHECK(pacing::evenFastBand(1.06 * P, P, true));
    CHECK(pacing::evenFastBand(1.5 * P, P, false));
    CHECK(!pacing::evenFastBand(1.97 * P, P, false));
    CHECK(pacing::evenFastBand(1.97 * P, P, true));
    CHECK(!pacing::evenFastBand(2.01 * P, P, true));
    CHECK(!pacing::evenFastBand(1.02 * P, P, true));
    CHECK(!pacing::evenFastBand(1.5 * P, 0.0, true));
}

// even_fast's clock: a present that misses its first refresh starts at the
// first it can make (the real frame before shows meanwhile) without raising
// the lag for good; a lag cut where the outputs ran out comes back to its
// target over a few frames, not a refresh at a time through misses.
TEST(pacing_even_fast_clock_fallback) {
    const double P = 1000.0 / 143.912, I = 10.0;
    pacing::Grid g{true, 0.0, P};
    g.lead = pacing::kX11Lead;
    g.readyMarginMs = pacing::kX11ReadyMarginMs;
    pacing::EvenClock clock;
    clock.fallback = true;
    double lastPlanned = 0.0, lands[8] = {}, realMs = 0.0, now = 1000.0;
    for (int f = 0; f < 50; ++f, now += I) clock.plan(now, I, 2.0, g, 3, lastPlanned, lands, realMs);
    const double settled = clock.lagMs;
    CHECK(settled > I && settled < I + 2.0 + pacing::kX11ReadyMarginMs + P);
    // (a slow GPU this once: ready 12 ms after the present, two refreshes
    // missed — a quarter of its larger target, not the refreshes given up)
    const double chainBefore = lastPlanned;
    const int n = clock.plan(now, I, 12.0, g, 3, lastPlanned, lands, realMs);
    now += I;
    CHECK(n >= 1 && lands[0] > chainBefore + 1.5 * P);
    CHECK(clock.lagMs <= settled + 0.25 * 10.0 + 0.01);
    for (int f = 0; f < 50; ++f, now += I) clock.plan(now, I, 2.0, g, 3, lastPlanned, lands, realMs);
    CHECK(std::abs(clock.lagMs - settled) < 0.3);
    // Cut far under the target (outputs ran out): back within 12 frames.
    clock.lagMs = settled - 8.0;
    for (int f = 0; f < 12; ++f, now += I) clock.plan(now, I, 2.0, g, 3, lastPlanned, lands, realMs);
    CHECK(std::abs(clock.lagMs - settled) < 0.5);
    // The historical clock (even_motion) still raises the lag on a miss.
    pacing::EvenClock historical;
    double chain = 0.0;
    now = 1000.0;
    for (int f = 0; f < 50; ++f, now += I) historical.plan(now, I, 2.0, g, 3, chain, lands, realMs);
    const double before = historical.lagMs;
    historical.plan(now, I, 2.0 + 3.0 * P, g, 3, chain, lands, realMs);
    CHECK(historical.lagMs > before + P);
}

namespace {
// gpu_headroom against a modelled game: each frame takes gpuMs of GPU time
// (its rendering and our synthesis) and cpuMs of its own; it runs at the
// slower of the two, or of the cap. The GPU's busy share is gpuMs over the
// frame interval (read every 200 ms where `known`); the game waits for the GPU
// (max_frame_latency) whenever the GPU, not its CPU or the cap, sets the pace.
struct HeadroomRun { double fps, busy, minFps, maxBusy, capFps; };
HeadroomRun runHeadroom(const std::vector<std::pair<double, double>>& phases, double seconds, bool known,
                        double headroom = 0.1) {
    pacing::HeadroomCap cap;
    pacing::IntervalFilter filter;
    double now = 0.0, interval = 0.0, lastRead = -1000.0;
    HeadroomRun r{0.0, 0.0, 1e9, 0.0, 0.0};
    double sumFps = 0.0, sumBusy = 0.0;
    int n = 0;
    const double phaseMs = seconds * 1000.0 / phases.size();
    while (now < seconds * 1000.0) {
        const auto& [gpuMs, cpuMs] = phases[std::min(phases.size() - 1, size_t(now / phaseMs))];
        const double capMs = cap.capFps > 0.0 ? 1000.0 / cap.capFps : 0.0;
        const double frame = std::max({gpuMs, cpuMs, capMs});
        now += frame;
        interval = filter.update(interval, frame);
        const double busy = 100.0 * gpuMs / frame;
        int read = -1;
        if (known && now - lastRead >= 200.0) {
            read = int(std::round(busy));
            lastRead = now;
        }
        const double waited = gpuMs > std::max(cpuMs, capMs) ? gpuMs - std::max(cpuMs, capMs) : 0.0;
        cap.update(now, waited, interval, read, headroom);
        // (the last phase's last 5 s)
        if (now > seconds * 1000.0 - 5000.0) {
            sumFps += 1000.0 / frame, sumBusy += busy, ++n;
            r.minFps = std::min(r.minFps, 1000.0 / frame);
            r.maxBusy = std::max(r.maxBusy, busy);
        }
    }
    r.fps = sumFps / n;
    r.busy = sumBusy / n;
    r.capFps = cap.capFps;
    return r;
}
} // namespace

// A GPU-bound game generating x4 (11 ms of GPU work a frame: 91 fps, the GPU
// always busy): held at about 90 % of that, the GPU idle a tenth of the time.
// Heavier, the cap follows it down; lighter, back up; a game held by its own
// CPU is never capped; headroom 0 changes nothing.
TEST(pacing_gpu_headroom_holds_a_saturated_game_under_the_gpu) {
    for (bool known : {true, false}) {
        const HeadroomRun steady = runHeadroom({{11.0, 3.0}}, 20.0, known);
        std::printf("gpu_headroom %s: 90.9 -> %.1f fps, GPU %.0f %% busy (max %.0f)\n", known ? "NVML" : "waits only",
                    steady.fps, steady.busy, steady.maxBusy);
        CHECK(steady.fps > 78.0 && steady.fps < 88.5);
        CHECK(steady.busy < 95.0);
        if (known) CHECK(steady.busy > 83.0 && steady.busy < 95.0);
        CHECK(steady.minFps > 0.8 * 90.9 - 0.1);
        // Heavier (14 ms): under 71 fps, the GPU not saturated for good.
        const HeadroomRun heavier = runHeadroom({{11.0, 3.0}, {14.0, 3.0}}, 40.0, known);
        CHECK(heavier.fps < 70.0 && heavier.fps > 0.8 * 1000.0 / 14.0 - 0.1);
        if (known) CHECK(heavier.busy < 95.0);
        // Lighter (7 ms): the cap rises with it (up to 128 fps at 90 %).
        const HeadroomRun lighter = runHeadroom({{11.0, 3.0}, {7.0, 3.0}}, 40.0, known);
        std::printf("  heavier %.1f fps %.0f %%, lighter %.1f fps %.0f %%\n", heavier.fps, heavier.busy, lighter.fps, lighter.busy);
        CHECK(lighter.fps > 115.0);
        if (known) CHECK(lighter.busy > 83.0 && lighter.busy < 95.0);
        // Held by its own CPU (15 ms a frame, 8 of GPU): never capped.
        const HeadroomRun cpu = runHeadroom({{8.0, 15.0}}, 20.0, known);
        CHECK(cpu.capFps == 0.0 && std::abs(cpu.fps - 1000.0 / 15.0) < 0.5);
        // CPU-bound again after a saturated phase: the cap goes away.
        const HeadroomRun later = runHeadroom({{11.0, 3.0}, {6.0, 15.0}}, 40.0, known);
        CHECK(later.capFps == 0.0);
        // Off.
        const HeadroomRun off = runHeadroom({{11.0, 3.0}}, 10.0, known, 0.0);
        CHECK(off.capFps == 0.0 && std::abs(off.fps - 1000.0 / 11.0) < 0.5);
    }
    // Capped, then another program keeps the GPU busy while the game's
    // frames still finish in time: the cap holds instead of walking down.
    pacing::HeadroomCap cap;
    double now = 0.0;
    while (!(cap.capFps > 0.0) && now < 2000.0) cap.update(now += 11.0, 8.0, 11.0, 100, 0.1);
    const double held = cap.capFps;
    CHECK(held > 80.0 && held < 90.0);
    for (int f = 0; f < 2000; ++f) cap.update(now += 1000.0 / held, 0.0, 1000.0 / held, 100, 0.1);
    CHECK(cap.capFps == held);
    // Waiting for good while held by the cap (the GPU shared with another
    // game that takes all it gets): it stops at half the rate it first capped.
    for (int f = 0; f < 4000; ++f) {
        const double rate = std::min(cap.capFps, 0.95 * cap.capFps);
        cap.update(now += 1000.0 / rate, 5.0, 1000.0 / rate, 100, 0.1);
    }
    CHECK(cap.capFps >= 0.5 * 1000.0 / 11.0 - 0.01 && cap.capFps < 0.5 * 1000.0 / 11.0 + 1.0);
}

// fps_limit on a game presenting through DXVK or VKD3D-Proton (Proton's
// Direct3D): the game thread renders each frame and hands it to a present
// thread of theirs, which submits its GPU work and presents (our
// vkQueuePresentKHR) in order; the game may begin frame k + 1 once present
// k - latency + 1 is released — after the GPU did it, or in FIFO with
// VK_KHR_present_wait, once the wait for that present returns. `latency`:
// 1 for VKD3D-Proton with a waitable swapchain at maximum latency 1 (the
// game waits for its last frame), 2 for DXVK at DXGI's maximum latency 1
// (Present(N) waits for frame N - 1), 3 for VKD3D-Proton without a waitable
// swapchain (its own bound: three frames), 4 for DXVK at DXGI's default 3.
// - present: the limit holds our present (FrameCap at its end): the present
//   thread waits, and the frames behind it read their input latency - 1
//   frames before they reach us;
// - wait: the limit holds that wait (fps_limit_wait), the present goes on;
// - own: the game's own limiter, before it reads its input (the reference).
struct TranslatedRun { double fps = 0.0, inputToLayerMs = 0.0, maxInputToLayerMs = 0.0, inputToGpuDoneMs = 0.0; };
enum class CapAt { Present, Wait, Own };
TranslatedRun runTranslated(CapAt at, int latency, double capFps, double cpuMs, double gpuMs, int frames = 600,
                            int spikeEvery = 0) {
    const double I = 1000.0 / capFps;
    pacing::CapSchedule cap;
    std::vector<double> start(frames + 1), call(frames + 1), handed(frames + 1), released(frames + 1);
    double presentFree = 0.0, gpuDone = 0.0, lastRelease = 0.0;
    start[1] = 0.0;
    TranslatedRun out;
    int counted = 0;
    for (int k = 1; k <= frames; ++k) {
        if (at == CapAt::Own) start[k] = std::max(start[k], cap.release(start[k], I));  // (sleeps, then reads its input)
        const double cpu = spikeEvery && k % spikeEvery == 0 ? 4.0 * cpuMs + I : cpuMs;
        call[k] = start[k] + cpu;
        handed[k] = std::max(call[k], presentFree);  // our present call (the GPU work submitted just before)
        gpuDone = std::max(handed[k], gpuDone) + gpuMs;
        double returns = handed[k], wait = gpuDone;
        if (at == CapAt::Present) returns = cap.release(handed[k], I);
        if (at == CapAt::Wait) wait = std::max(gpuDone, cap.release(handed[k], I));
        presentFree = returns;
        released[k] = lastRelease = std::max({lastRelease, returns, wait});
        // Present(k) returns once present k - latency + 1 is released.
        if (k < frames) start[k + 1] = std::max(call[k], k - latency + 1 >= 1 ? released[k - latency + 1] : 0.0);
        if (k > frames / 4) {
            out.inputToLayerMs += handed[k] - start[k];
            out.maxInputToLayerMs = std::max(out.maxInputToLayerMs, handed[k] - start[k]);
            out.inputToGpuDoneMs += gpuDone - start[k];
            ++counted;
        }
    }
    out.inputToLayerMs /= counted;
    out.inputToGpuDoneMs /= counted;
    out.fps = (frames - frames / 4 - 1) * 1000.0 / (handed[frames] - handed[frames / 4 + 1]);
    return out;
}

TEST(pacing_fps_limit_wait_holds_a_translated_game_before_its_input) {
    for (int latency : {1, 2, 3, 4}) {
        for (double capFps : {30.0, 40.0, 60.0}) {
            const double I = 1000.0 / capFps;
            const TranslatedRun present = runTranslated(CapAt::Present, latency, capFps, 4.0, 6.0);
            const TranslatedRun wait = runTranslated(CapAt::Wait, latency, capFps, 4.0, 6.0);
            const TranslatedRun own = runTranslated(CapAt::Own, latency, capFps, 4.0, 6.0);
            std::printf("  latency %d, cap %.0f: input to layer %.1f ms (present) %.1f (wait) %.1f (own), %.2f / %.2f / %.2f fps\n",
                        latency, capFps, present.inputToLayerMs, wait.inputToLayerMs, own.inputToLayerMs, present.fps,
                        wait.fps, own.fps);
            // The limit holds the rate either way...
            CHECK(std::abs(present.fps - capFps) < 0.05 * capFps);
            CHECK(std::abs(wait.fps - capFps) < 0.05 * capFps);
            // ...but the frames queued behind a held present read their
            // input latency - 1 frames earlier.
            CHECK(present.inputToLayerMs > std::max(4.0, (latency - 1) * I) - 0.5);
            // Held at its wait, the game is as fresh as with its own limiter.
            CHECK(wait.inputToLayerMs < own.inputToLayerMs + 0.5);
            CHECK(wait.maxInputToLayerMs < 4.0 + 0.5);
            CHECK(wait.inputToGpuDoneMs < own.inputToGpuDoneMs + 0.5);
        }
    }
    // A frame now and then far longer than the cap: no burst afterwards, and
    // no queue left behind.
    const TranslatedRun spiky = runTranslated(CapAt::Wait, 4, 30.0, 4.0, 6.0, 600, 37);
    const double spikeMs = 4.0 * 4.0 + 1000.0 / 30.0;
    CHECK(spiky.fps < 30.0 + 0.05);
    CHECK(spiky.inputToLayerMs < 4.0 + (spikeMs - 4.0) / 37.0 + 0.5);
    // GPU-bound under the limit (30 ms of GPU a frame, a 40 fps cap): the GPU
    // sets the rate, and the frames wait for it no longer than without a
    // limit at all.
    for (int latency : {1, 2, 3, 4}) {
        const TranslatedRun gpu = runTranslated(CapAt::Wait, latency, 40.0, 4.0, 30.0);
        const TranslatedRun free = runTranslated(CapAt::Own, latency, 1000.0, 4.0, 30.0);
        std::printf("  GPU-bound, latency %d: %.2f fps (no limit %.2f), input to GPU done %.1f ms (no limit %.1f)\n", latency,
                    gpu.fps, free.fps, gpu.inputToGpuDoneMs, free.inputToGpuDoneMs);
        CHECK(std::abs(gpu.fps - free.fps) < 0.2);
        CHECK(gpu.inputToGpuDoneMs < free.inputToGpuDoneMs + 0.5);
    }
}

// gamescope's refresh cycle for each of our swapchains (pollGamescope): the
// WSI layer's default (60 Hz) until gamescope sends the swapchain one, the
// surface's last cycle standing for a swapchain re-created without one.
TEST(pacing_gamescope_cycle_takes_the_limit_a_re_created_swapchain_got) {
    using pacing::GamescopeCycle;
    const uint64_t d = GamescopeCycle::kWsiDefaultNs, limit = 33'333'333, deck = 11'111'111;
    uint64_t known = 0;
    // The game's first swapchain on a 60 Hz display.
    GamescopeCycle a;
    CHECK(a.take(d, known) == d && known == d);
    // Steam's limit at 30: the limiter file flips before the limit's cycle
    // reaches the swapchain, which still reads 60 Hz; the WSI layer then has
    // the swapchain re-created (a game aware of the limiter), and the new one
    // gets the limit's cycle at once. Until 31/10 its first reading was taken
    // for the default: the surface's last, 60 Hz, stood under the limit.
    CHECK(a.take(d, known) == d);
    GamescopeCycle b;
    CHECK(b.take(limit, known) == limit && known == limit);
    CHECK(b.take(limit, known) == limit);
    // Re-created again, gamescope sending nothing (it sends a surface's
    // cycle only when it changes): the default read, the limit stands.
    GamescopeCycle c;
    CHECK(c.take(d, known) == limit && known == limit);
    CHECK(c.take(d, known) == limit);
    // A cycle sent to a swapchain that had read the default: its own.
    CHECK(c.take(deck, known) == deck && known == deck);
    // ...and after a change, even back to the default's value: the limit
    // lifted on a 60 Hz display, sent to the swapchain that had the limit.
    CHECK(b.take(d, known) == d && known == d);
    GamescopeCycle e;
    CHECK(e.take(d, known) == d);
}
