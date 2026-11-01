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

// Keep the historical EMA for jitter and isolated slow frames. Two similar
// intervals on the same side of a large cadence change confirm a new rate:
// following the old rate for another ~10 frames otherwise overproduces after
// an FPS rise and builds a display queue that has to discard interpolation.
struct IntervalFilter {
    double previousMs = 0.0;
    double update(double estimateMs, double measuredMs) {
        if (!(measuredMs > 0.5 && measuredMs < 500.0)) {
            previousMs = 0.0;
            return estimateMs;
        }
        const bool consistent = previousMs > 0.0 &&
            std::abs(measuredMs - previousMs) <= 0.1 * std::min(measuredMs, previousMs);
        const bool changed = (measuredMs < 0.75 * estimateMs && previousMs < 0.75 * estimateMs) ||
                             (measuredMs > 1.25 * estimateMs && previousMs > 1.25 * estimateMs);
        const double result = consistent && changed ? 0.5 * (measuredMs + previousMs)
                                                   : estimateMs * 0.8 + measuredMs * 0.2;
        previousMs = measuredMs;
        return result;
    }
};

// x11_clock's refreshes are the moments the X server hands the window's
// content over (Xwayland: at the compositor's frame callback); whatever was
// presented since goes out then, wherever it falls in the period, but the
// X server's copy into the window — on a GPU busy with the next synthesis —
// and the compositor's own work must be done by its deadline. Measured on
// gamescope's Xwayland, x4 at 30 fps, 144 Hz: 0.3 period ahead, 5 to 9
// refreshes skipped a second at 720p; 0.85 to 0.95 (right after the previous
// one), 3 to 35 at 1440p — a frame not ready by then goes out when it is,
// late in the period, into the deadline; 0.6, none at 720p, 1 to 10 at 1440p
// (headless gamescope, its compositor short of GPU time, varies most). On
// Hyprland (1440p): 1.5 and 0.3 a second, against 9 and 15 for the virtual
// grid at the phase it happened to take.
constexpr double kX11Lead = 0.6;
// ...and a present must be done that long before x11_clock's refresh to go
// out with it (Xwayland's handling): measured on gamescope's Xwayland at
// 144 Hz, first frames of a job sent 1.5 ms or more before their refresh all
// made it, 0.5 to 1 ms before 64 % missed it (then two frames met in the
// next refresh with tight_hold). pace_plan plans a frame for the first
// refresh at least this long after it is usually ready, where frames go a
// refresh apart (adaptive, tight_hold: x4 at 30 fps on 144 Hz, replaced 1.9
// a second -> 0.1, the real frame still 2.7 ms sooner than evenly spaced).
constexpr double kX11ReadyMarginMs = 1.5;

// The display's refresh grid, when the present feedback knows it.
struct Grid {
    bool valid = false;
    double lastMs = 0.0;    // a refresh
    double periodMs = 0.0;
    // How far ahead of its refresh a frame is presented, in periods: 0.3 for
    // a display's refresh (a present just after it would slip a whole
    // period); kX11Lead for x11_clock's.
    double lead = 0.3;
    // How long before its refresh a frame must be ready to make it
    // (kX11ReadyMarginMs for x11_clock's, 0 otherwise): pace_plan's margin.
    double readyMarginMs = 0.0;
    // The first refresh at or after `ms`.
    double land(double ms) const { return lastMs + std::ceil((ms - lastMs) / periodMs) * periodMs; }
};

// When frame k of a present goes out (ms): k/m of the frame interval after
// the origin (when the GPU finished the job's first frame) — interp_hold:
// FrameOrder::offset —, never less than half a slot after the previous
// present (a job whose GPU work ended early must not land on the previous
// frame's refresh), and, with a known grid, 0.3 refresh before the refresh it
// should show on (a present landing just after a refresh would slip a whole
// period) and on a refresh of its own — the one planned (planLands) when
// there is a plan.
inline double presentTargetMs(double originMs, double intervalMs, int frames, const FrameOrder& order, int k,
                              double lastPresentMs, double lastLandMs, const Grid& grid, double plannedLandMs = 0.0) {
    if (grid.valid && plannedLandMs > 0.0) {  // planLands: for that refresh, or the next one free
        double land = plannedLandMs;
        if (lastLandMs > 0.0 && land < lastLandMs + 0.5 * grid.periodMs) land = lastLandMs + grid.periodMs;
        return land - grid.lead * grid.periodMs;
    }
    const double slot = intervalMs / std::max(1, frames);
    const bool held = order.hold < 1.f;
    double t = originMs + (held ? intervalMs * order.offset(k) : slot * k);
    if (lastPresentMs > 0.0) t = std::max(t, lastPresentMs + 0.5 * slot * (held ? order.hold : 1.0));
    if (grid.valid) {
        double land = grid.land(t);
        if (lastLandMs > 0.0 && land < lastLandMs + 0.5 * grid.periodMs) land = lastLandMs + grid.periodMs;
        t = land - grid.lead * grid.periodMs;
    }
    return t;
}

// The refresh each frame of a present should show on, decided when the
// present is recorded — frame_timing then gives every frame the content of
// that moment, and the worker presents it for that refresh
// (presentTargetMs's `plannedLandMs`). `baseMs[k]` is frame k's place in the
// present's schedule (0 for the first), `lagMs` how long after the present
// call the first frame is usually ready (the game's rendering and the
// synthesis still running then). Predicting from the present call alone (the
// layer before 2026-10-14) put a frame on the refresh before the one it made
// whenever that work crossed a refresh — measured in the pacing simulator,
// 30 fps x4 on 144 Hz: the content steps off their display steps by 4.4 ms
// rms, 1.5 with the plan. Each frame on a refresh after the one planned
// before it; a plan that ran more than two refreshes ahead of this one
// starts again (the worker fell behind: those refreshes are gone). The
// previous plan's last refresh is taken on the grid as it is now: the grid's
// phase moves (virtual_grid's half period, x11_clock's grid replacing the
// virtual one, a measured grid refined), and a plan whose frames fill every
// refresh chains each job on the one before — it kept the old phase for good
// (measured, x11_clock: every frame planned 0.6 period off the X server's
// refreshes for the whole run, 30 to 37 frames a second replaced).
inline void planLands(double nowMs, double lagMs, const double* baseMs, int frames, const Grid& grid,
                      double& lastPlannedMs, double* landMs) {
    if (lastPlannedMs > 0.0)
        lastPlannedMs = grid.lastMs + std::round((lastPlannedMs - grid.lastMs) / grid.periodMs) * grid.periodMs;
    if (lastPlannedMs > 0.0 && lastPlannedMs > grid.land(nowMs + lagMs) + 2.0 * grid.periodMs) lastPlannedMs = 0.0;
    for (int k = 0; k < frames; ++k) {
        double land = grid.land(nowMs + lagMs + grid.readyMarginMs + baseMs[k]);
        if (lastPlannedMs > 0.0 && land < lastPlannedMs + 0.5 * grid.periodMs) land = lastPlannedMs + grid.periodMs;
        landMs[k] = land;
        lastPlannedMs = land;
    }
}

// Join the previous real frame where it was scheduled, rather than assume
// it was one content interval before this one. At 30 fps / 144 Hz the real
// frames alternate four and five refreshes apart. The content interval
// leaves the phase error in one generated step (up to 5.5 ms); the scheduled
// span spreads it over the pair. No present moves and no real is replaced.
struct MotionClock {
    double previousRealMs = 0.0;
    void reset() { previousRealMs = 0.0; }
    double span(double realMs, double intervalMs, bool active) {
        if (!active || !std::isfinite(realMs) || !std::isfinite(intervalMs) || intervalMs <= 0.5 || intervalMs >= 500.0) {
            reset();
            return 0.0;
        }
        const double elapsed = realMs - previousRealMs;
        const bool valid = previousRealMs > 0.0 && elapsed >= 0.5 * intervalMs && elapsed <= 2.0 * intervalMs;
        previousRealMs = realMs;
        return valid ? elapsed : 0.0;
    }
};

// even_motion: every refresh shows the content of its own moment. The
// display runs `lagMs` behind the game's present clock; a present covers
// the refreshes whose moment falls after the previous present (nowMs - dtMs)
// and up to this one, each a generated frame, t = (refresh - lag - previous)
// / dt, and the real frame only stands for its moment (`realMs`), never on
// a refresh of its own. Pinned on refreshes (pace_plan, phase_smooth), the
// real frames of a 30 fps game on 144 Hz make pairs of 5 and 4 refreshes:
// the motion a quarter faster through one pair in five, and a game's present
// jitter on top (PRAGMATA: 5 to 10 ms of content a 6.95 ms refresh). The lag
// must let the job's first refresh — just after the previous present's
// moment — come after its first frame is ready: lag >= dt + ready + lead.
// It follows a slowly decaying high mark of the game's frame times (faster,
// each new long frame gave a refresh up); a refresh the job cannot make is
// given up and the lag grows by as much (the picture holds one refresh; its
// motion does not jump). A long frame needs a refresh more than the
// multiplier's usual ceiling (39 ms on 144 Hz: 6): the outputs are sized so.
struct EvenClock {
    double lagMs = 0.0;
    double highDtMs = 0.0;  // the game's frame time, high mark (decays 0.002 ms a frame: 2 ms in 30 s at 30 fps)
    void reset() { lagMs = highDtMs = 0.0; }
    // The frames of this present (0: none fits, nothing planned) and their
    // refreshes; lastPlannedMs chains the presents as planLands does.
    int plan(double nowMs, double dtMs, double readyLagMs, const Grid& g, int maxFrames, double& lastPlannedMs,
             double* landMs, double& realMs) {
        const double P = g.periodMs;
        if (!(P > 0.0) || !(dtMs > 0.5 && dtMs < 500.0) || maxFrames < 1) {
            reset();
            return 0;
        }
        highDtMs = std::max(dtMs, highDtMs - 0.002);
        const double target = highDtMs + std::max(readyLagMs, 0.0) + std::max(g.lead * P, g.readyMarginMs) + 0.5;
        const double earliest = g.land(nowMs + std::max(readyLagMs, 0.0) + g.lead * P);
        double first = earliest;
        if (lastPlannedMs > 0.0 && lagMs > 0.0) {
            const double next = g.lastMs + std::round((lastPlannedMs - g.lastMs) / P) * P + P;
            // (Never restarted ahead of the chain: the motion would go back.)
            if (next >= earliest) first = next;
            else lagMs += earliest - next;  // refreshes given up: the picture holds
        }
        const double previous = nowMs - dtMs;
        if (!(lagMs > 0.0)) lagMs = std::max(target, first - previous - 0.5 * P);  // (start: half a refresh into the pair)
        else if (lagMs > target) lagMs = std::max(target, lagMs - 0.05);
        int n = 0;
        for (double land = first; land <= nowMs + lagMs + 1e-6 && n < maxFrames; land += P) landMs[n++] = land;
        if (n == 0) return 0;
        if (landMs[n - 1] + P <= nowMs + lagMs) lagMs = landMs[n - 1] - nowMs;  // (past the outputs: the rest skipped)
        lastPlannedMs = landMs[n - 1];
        realMs = nowMs + lagMs;
        return n;
    }
};

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

// tight_hold: a fixed multiplier whose frames fall short of the refresh rate
// by less than a refresh and a quarter per game frame (30 fps x4 on 144 Hz:
// 4.8 refreshes for 4 frames) holds a frame a refresh more in most game
// frames anyway — wherever the even spacing happens to put it. With the
// generated frames one refresh apart, that refresh goes to the real frame,
// which comes (m - 1) * (I/m - P) sooner (x4 at 30 fps on 144 Hz: 4.2 ms;
// x3 at 40 fps: 2.8): as many refreshes held, the sharpest frame the one
// shown longer. The hold that spaces them so (FrameOrder::hold), or 1 —
// at most one frame a refresh, more than that or under 2 ms saved: unchanged.
// `wasOn`: it applied to the previous present — kept past the bounds by a
// few percent, so a game hovering at one (an uncapped 32-33 fps at x4 on
// 144 Hz) does not switch the spacing from frame to frame.
inline float tightHold(double periodMs, int mult, double intervalMs, int real, bool wasOn = false) {
    if (!(periodMs > 0.0) || !(intervalMs > 0.0) || mult < 2 || real < 1) return 1.f;
    const double perFrame = intervalMs / mult;
    const double slack = wasOn ? 1.03 : 1.0;
    if (perFrame <= periodMs || intervalMs > (mult + 1.25) * periodMs * slack) return 1.f;
    const double savedMs = real * (perFrame - periodMs);
    return savedMs >= (wasOn ? 1.6 : 2.0) ? float(periodMs * mult / intervalMs) : 1.f;
}

// A job that started too late for its first frame to make the first refresh
// it could make once ready (origin + 0.3 refresh), or that began a refresh
// late (the worker still presenting the one before).
inline bool jobLate(double originMs, double lastLandMs, double jobStartDelayMs, const Grid& grid) {
    const double ready = originMs + 0.3 * grid.periodMs;
    const double firstLand = grid.land(ready);
    return lastLandMs - firstLand > 0.9 * grid.periodMs || jobStartDelayMs > 0.9 * grid.periodMs;
}

// Notice a new whole-refresh slip of a presentation plan. Repeating the
// same offset is one backlog, not a fresh event on every job. Half-refresh
// virtual-grid phase changes, pauses and rate changes restart observation.
// The game can then re-space the next pair over one fewer output, rather
// than omit an already-generated intermediate and double a movement step.
struct PlanSlip {
    double previousSlots = 0.0;
    double periodMs = 0.0;
    void reset() { previousSlots = periodMs = 0.0; }
    bool update(double plannedMs, double landedMs, double period, bool active) {
        if (!active || !(plannedMs > 0.0) || !std::isfinite(plannedMs) || !std::isfinite(landedMs) ||
            !std::isfinite(period) || !(period > 0.0)) {
            reset();
            return false;
        }
        const double delta = (landedMs - plannedMs) / period;
        const double rounded = std::round(delta);
        if (std::abs(delta - rounded) > 0.1) { reset(); return false; }
        const double slots = std::max(0.0, rounded);
        const bool slipped = periodMs > 0.0 && std::abs(period - periodMs) <= 0.01 * period && slots > previousSlots;
        previousSlots = slots;
        periodMs = period;
        return slipped;
    }
};

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

// refresh_fill also caps output when the chosen multiplier meets or exceeds
// the refresh rate. A hole around an integer ratio (36 fps x4 on 143.912 Hz)
// otherwise leaves independent GPU origins and occasional replaced frames.
// Keep at least two outputs per job so the virtual grid need not switch off
// on single-frame jobs; a slower game still needs more outputs than allocated.
inline bool refreshFillFits(double refreshesPerFrame, int ceiling) {
    return ceiling >= 2 && refreshesPerFrame >= 2.0 && refreshesPerFrame <= double(ceiling) * 1.02;
}

// A persistent display backlog with no worker/GPU pressure needs a refresh
// back, not an entire real-only job. Keep interpolation and its motion clock
// while removing one output. Worker pressure, a slow GPU, x2, or an unknown
// output period keep the immediate real-only recovery.
inline int catchUpFrames(int nominal, bool persistentLate, double workerWaitMs, double intervalMs,
                         double outputPeriodMs, double readyLagMs) {
    if (nominal < 3 || !persistentLate || !(outputPeriodMs > 0.0) || !(intervalMs > 0.0) ||
        !std::isfinite(workerWaitMs) || !std::isfinite(readyLagMs) ||
        workerWaitMs > 0.25 * intervalMs || readyLagMs >= 0.5 * intervalMs)
        return 1;
    return nominal - 1;
}

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
