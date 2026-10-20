#pragma once
#include "config.h"

#include <algorithm>

namespace bdex {

// Where the frames of one present go on screen: m positions spread evenly
// over the frame interval, the real frame at position `real`, generated
// output o at position o when it comes before the real frame (interpolated
// between the previous and the current frame: t in (0, 1)) or o + 1 after it
// (extrapolated past the current frame: t in (1, 2)). Every frame shows the
// content of its position, so the real frame is `real`/m of a frame interval
// late:
//   interpolation: real last  — (m-1)/m of a frame of added latency;
//   extrapolation: real first — none, every generated frame is a prediction;
//   hybrid:        real second — one interpolated frame first, then the real
//                  frame, then the predictions: 1/m of latency at x3/x4 (x4:
//                  a quarter of a frame instead of three quarters), and no
//                  prediction reaches as far ahead as in pure extrapolation.
//                  At x2 hybrid is interpolation. hybrid_predict N: N
//                  predictions after the real frame (x4, 1: half a frame of
//                  latency, one prediction a quarter of a frame ahead).
// Shared by the layer (swapchain.cpp), the capture app and bdex_offline.
//
// interp_hold (h < 1, the layer's present path only: `withHold`) shortens
// the real frame's hold: the positions up to the real frame are h/m of an
// interval apart instead of 1/m (the real frame shows h*real/m of an
// interval after the first one), the predictions after it 1/m apart as
// before. Each frame still shows the content of its own display time — the
// content is a constant h*real/m (not real/m) of an interval behind the
// display — so motion stays right; the frames are just unevenly spaced (x2,
// h = 0.5: a quarter interval, then three quarters). h = 1 is the historical
// order, bit for bit.
struct FrameOrder {
    int m = 2;
    int real = 1;
    float hold = 1.f;

    static FrameOrder of(const Config& cfg, int multiplier, bool withHold = false) {
        FrameOrder o;
        o.m = std::max(1, multiplier);
        // (hybrid_predict: that many predicted frames after the real one —
        // x4 with 1: two interpolated, the real one, a prediction a quarter
        // of a frame ahead; 0: the real one second)
        o.real = cfg.extrapolate ? 0
                 : cfg.hybrid    ? (cfg.hybridPredict > 0 ? std::max(1, o.m - 1 - cfg.hybridPredict) : std::min(1, o.m - 1))
                                 : o.m - 1;
        // (auto, negative: the smallest, which the caller raises to one refresh)
        if (withHold && o.real > 0 && cfg.interpHold < 1.f) o.hold = cfg.interpHold < 0.f ? 0.1f : std::clamp(cfg.interpHold, 0.1f, 1.f);
        return o;
    }
    int position(int output) const { return output < real ? output : output + 1; }
    bool predicted(int output) const { return position(output) > real; }
    // The synthesis time of output o, written so that the pure modes keep
    // their historical values bit for bit (i/m, and 1 + i/m).
    float t(int output) const {
        const int p = position(output);
        if (p < real && hold < 1.f) return 1.f - float(real - p) * hold / float(m);
        return p < real ? float(m - real + p) / float(m) : 1.f + float(p - real) / float(m);
    }
    // When position p goes on screen, in frame intervals after position 0.
    double offset(int p) const {
        if (hold >= 1.f || p <= 0) return double(p) / double(m);
        return p <= real ? double(p) * hold / double(m) : (double(real) * hold + double(p - real)) / double(m);
    }
};

} // namespace bdex
