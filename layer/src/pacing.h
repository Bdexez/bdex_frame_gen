#pragma once
// The pacing's decisions, apart from Vulkan: when each frame of a present
// goes out, how much a GPU-bound game is held, how many frames the adaptive
// multiplier gives a game frame, when generation idles for a fast game, when
// a job counts as late. VirtualSwapchain (swapchain.cpp) calls these with
// its clocks and the present feedback's refresh grid; tests/test_pacing.cpp
// drives them with a simulated game, GPU and screen.
#include "frame_order.h"

#include <algorithm>
#include <cmath>

namespace bdex {
namespace pacing {

// The display's refresh grid, when the present feedback knows it.
struct Grid {
    bool valid = false;
    double lastMs = 0.0;    // a refresh
    double periodMs = 0.0;
    // The first refresh at or after `ms`.
    double land(double ms) const { return lastMs + std::ceil((ms - lastMs) / periodMs) * periodMs; }
};

// When frame k of a present goes out (ms): k/m of the frame interval after
// the origin (when the GPU finished the job's first frame) — interp_hold:
// FrameOrder::offset —, never less than half a slot after the previous
// present (a job whose GPU work ended early must not land on the previous
// frame's refresh), and, with a known grid, 0.3 refresh before the refresh it
// should show on (a present landing just after a refresh would slip a whole
// period) and on a refresh of its own.
inline double presentTargetMs(double originMs, double intervalMs, int frames, const FrameOrder& order, int k,
                              double lastPresentMs, double lastLandMs, const Grid& grid) {
    const double slot = intervalMs / std::max(1, frames);
    const bool held = order.hold < 1.f;
    double t = originMs + (held ? intervalMs * order.offset(k) : slot * k);
    if (lastPresentMs > 0.0) t = std::max(t, lastPresentMs + 0.5 * slot * (held ? order.hold : 1.0));
    if (grid.valid) {
        double land = grid.land(t);
        if (lastLandMs > 0.0 && land < lastLandMs + 0.5 * grid.periodMs) land = lastLandMs + grid.periodMs;
        t = land - 0.3 * grid.periodMs;
    }
    return t;
}

// interp_hold as the screen allows: never closer than a refresh apart (also
// the fastest a VRR screen shows) — two frames inside one refresh would
// replace each other —, auto (cfgHold < 0) with a quarter refresh of margin
// for the jitter and only where it saves 2 ms or more; unknown refresh rate:
// as asked, or even for auto.
inline float effectiveHold(float hold, float cfgHold, double hz, int mult, double intervalMs, int real) {
    if (!(hold < 1.f && intervalMs > 0.0)) return hold;
    const double minGap = (cfgHold < 0.f ? 1.25 : 1.0) * 1000.0 / std::max(hz, 1.0);
    if (hz > 0.0) hold = float(std::min(1.0, std::max<double>(hold, minGap * mult / intervalMs)));
    const double savedMs = (1.0 - hold) * real * intervalMs / std::max(1, mult);
    if (cfgHold < 0.f && (hz <= 0.0 || savedMs < 2.0)) hold = 1.f;
    return hold;
}

// A job that started too late for its first frame to make the first refresh
// it could make once ready (origin + 0.3 refresh), or that began a refresh
// late (the worker still presenting the one before).
inline bool jobLate(double originMs, double lastLandMs, double jobStartDelayMs, const Grid& grid) {
    const double ready = originMs + 0.3 * grid.periodMs;
    const double firstLand = grid.land(ready);
    return lastLandMs - firstLand > 0.9 * grid.periodMs || jobStartDelayMs > 0.9 * grid.periodMs;
}

// latency_limiter: from how long a GPU-bound game waited for its previous
// frame's GPU work (W, the game's lead on the GPU), the hold before its next
// frame starts: half of (W - slack) added each frame, halved when W is about
// zero (the GPU may be idling), at most 3/4 of the frame interval; the slack
// follows the jitter (x1.5 at each late frame, x0.99 otherwise, at least
// 1 ms + 5 % of the interval, at most 30 %). Holds under 0.25 ms are not
// worth a sleep: 0 then.
struct StartLimiter {
    double delayMs = 0.0;
    double slackMs = 0.0;
    double step(double waitedMs, double intervalMs) {
        const bool late = waitedMs < 0.05;
        slackMs = std::clamp(slackMs * (late ? 1.5 : 0.99), 1.0 + 0.05 * intervalMs, 0.3 * intervalMs);
        if (late) delayMs *= 0.5;
        else delayMs += 0.5 * (waitedMs - slackMs);
        delayMs = std::clamp(delayMs, 0.0, 0.75 * intervalMs);
        return delayMs < 0.25 ? 0.0 : delayMs;
    }
};

// Adaptive multiplier: as many frames as fit at the target rate, the fraction
// carried over (50 fps on 144 Hz: 3, 3, 2, ...), one output period apart;
// past the ceiling or under one frame, nothing is carried. `periodMs` is
// cleared when the game frame gets a single frame.
struct AdaptivePacer {
    double acc = 0.0;
    int next(double intervalMs, double& periodMs, int maxMult) {
        acc += intervalMs / periodMs;
        const int n = std::clamp(static_cast<int>(acc), 1, maxMult);
        acc = std::clamp(acc - n, 0.0, 0.999);
        if (n < 2) periodMs = 0.0;
        return n;
    }
};

// skip_fast: generation idles while the game runs at 85 % of the refresh
// rate or more, and resumes under 75 %, either crossing held for half a
// second. `toggled` tells the call that flipped it.
struct FastGate {
    bool idle = false;
    double sinceMs = 0.0;
    bool update(bool enabled, double gameIntervalMs, double hz, double nowMs, bool& toggled) {
        toggled = false;
        if (!enabled || hz <= 0.0 || gameIntervalMs <= 0.05) {
            idle = false;
            return false;
        }
        const double ratio = 1000.0 / gameIntervalMs / hz;
        const bool crossing = idle ? ratio < 0.75 : ratio >= 0.85;
        if (!crossing) sinceMs = 0.0;
        else if (sinceMs == 0.0) sinceMs = nowMs;
        else if (nowMs - sinceMs >= 500.0) {
            idle = !idle;
            sinceMs = 0.0;
            toggled = true;
        }
        return idle;
    }
};

} // namespace pacing
} // namespace bdex
