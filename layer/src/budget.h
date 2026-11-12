#pragma once

namespace bdex {

// gpu_budget: keeps the frame generation's GPU time under a share of the
// game's frame interval by stepping down a quality ladder, and back up when
// there is room again. Fed about once a second with measured timings; pure
// logic, so it is unit-tested on its own (tests/test_budget.cpp).
//
// Levels: 0 the configured settings; 1 the costly optional passes off
// (per-pixel vectors, re-rankings, sub-pixel fits, bicubic sampling,
// acceleration, extra flow lookups, and the passes over the finished frame:
// false_detail, mosaic_blur); 2 the narrowest search and no flow lookup iteration; 3 and up
// the flow at half, then a quarter of its resolution (the output stage is
// rebuilt) — as far as `maxLevel` allows.
class GpuBudget {
public:
    static constexpr int kMaxLevels = 5;

    // gpuMs: average GPU time of the generation per game frame over the
    // last window; intervalMs: the game's frame interval; budget: the share
    // of it the generation may use (0..1). Returns the level to use.
    int update(double gpuMs, double intervalMs, double budget, int maxLevel);
    int level() const { return level_; }
    void reset() { *this = GpuBudget{}; }

private:
    int level_ = 0;
    int over_ = 0, under_ = 0;        // consecutive windows over / well under the budget
    double costAt_[kMaxLevels] = {};  // last cost measured at each level (0 = unknown)
};

} // namespace bdex
