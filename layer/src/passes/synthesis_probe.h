#pragma once
// online_tune's synthesis probe: rates the synthesis options on trial by
// comparing a frame the engine synthesises with a real frame the game
// rendered — which the synthesis must not have seen.
//
//   interpolation: when frame C arrives, B is rebuilt from A and C (flows
//     A->C composed from A->B and B->C, flow_compose.comp) at t = 0.5 and
//     compared with the real B; the naive predictor is the cross-fade of A
//     and C.
//   extrapolation: C is predicted from A and B one frame ahead (t = 2) with
//     the A->B flows still in the flow images at the start of recordFlow,
//     and compared with the real C when it is recorded; the naive predictor
//     holds B.
//
// A is a copy of the previous-but-one real frame (the game may reuse its
// image by then). The engine's own synthesis shader runs (linear rgba16f
// variant), with the settings of the frame — the options on trial included.
// Results go to the online_tune slot counters (probe_compare.comp).
#include "fg_pass.h"

namespace bdex {

class SynthesisProbe {
public:
    void create(FgContext& ctx, bool extrapolate, VkBuffer counters);
    void destroy(FgContext& ctx);
    void frameBegin() { ++frame_; }
    // At flowBegin, after the frame's settings are chosen. `active`: a
    // synthesis option is on trial.
    void flowBegin(FgContext& ctx, VkCommandBuffer cmd, bool active);
    // At the frame's first synthesis (prev = B, cur = C). Records the
    // comparison into counter slot `slot` and returns true when a probe
    // was made; keeps the copy of B for the next frame.
    bool measure(FgContext& ctx, VkCommandBuffer cmd, uint32_t prev, uint32_t cur, uint32_t slot, bool active);

private:
    void copyImage(FgContext& ctx, VkCommandBuffer cmd, VkImage src, VkImage dst, VkExtent2D extent);

    bool extrapolate_ = false;
    FgPipeline synth_, compose_, compare_;
    AllocatedImage copyA_;       // previous-but-one real frame (source format, render size)
    AllocatedImage fwdAB_;       // interpolation: the A->B forward flow, copied before recordFlow rewrites it
    AllocatedImage fwdAC_, bwdCA_;  // interpolation: composed flows
    AllocatedImage probe_;       // the synthesised frame (display size, linear rgba16f)
    std::vector<VkDescriptorSet> synthSets_;    // [source]: cur = C (interpolation) or B (extrapolation)
    std::vector<VkDescriptorSet> compareSets_;  // [prev * N + cur]
    VkDescriptorSet composeSet_ = VK_NULL_HANDLE;
    uint64_t frame_ = 0;
    uint64_t copyFrame_ = ~0ull;   // frame at which copyA_ was taken (it holds that frame's prev)
    uint64_t lastFrame_ = ~0ull;   // frame of the last measure, and its cur
    uint32_t lastCur_ = 0;
    uint64_t fwdFrame_ = ~0ull;    // interpolation: frame at which fwdAB_ was copied
    uint64_t probeFrame_ = ~0ull;  // extrapolation: frame at which the prediction was synthesised
};

} // namespace bdex
