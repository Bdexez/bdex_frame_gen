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
//                  At x2 hybrid is interpolation.
// Shared by the layer (swapchain.cpp), the capture app and bdex_offline.
struct FrameOrder {
    int m = 2;
    int real = 1;

    static FrameOrder of(const Config& cfg, int multiplier) {
        FrameOrder o;
        o.m = std::max(1, multiplier);
        o.real = cfg.extrapolate ? 0 : cfg.hybrid ? std::min(1, o.m - 1) : o.m - 1;
        return o;
    }
    int position(int output) const { return output < real ? output : output + 1; }
    bool predicted(int output) const { return position(output) > real; }
    // The synthesis time of output o, written so that the pure modes keep
    // their historical values bit for bit (i/m, and 1 + i/m).
    float t(int output) const {
        const int p = position(output);
        return p < real ? float(m - real + p) / float(m) : 1.f + float(p - real) / float(m);
    }
};

} // namespace bdex
