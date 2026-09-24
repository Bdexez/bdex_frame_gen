#include "budget.h"

#include <algorithm>

namespace bdex {

int GpuBudget::update(double gpuMs, double intervalMs, double budget, int maxLevel) {
    maxLevel = std::clamp(maxLevel, 0, kMaxLevels - 1);
    level_ = std::min(level_, maxLevel);
    if (!(gpuMs > 0.0) || !(intervalMs > 0.0) || !(budget > 0.0)) return level_;
    const double limit = budget * intervalMs;
    costAt_[level_] = gpuMs;
    if (gpuMs > limit) {
        under_ = 0;
        // Two windows in a row: a single spike (a loading hitch, a shader
        // compile) must not cost quality.
        if (++over_ >= 2 && level_ < maxLevel) {
            ++level_;
            over_ = 0;
        }
    } else if (gpuMs < 0.6 * limit && level_ > 0) {
        over_ = 0;
        // Step back up after five calm windows, unless the level above is
        // known not to fit (then it would only oscillate).
        const double above = costAt_[level_ - 1];
        if (++under_ >= 5 && (above == 0.0 || above < 0.9 * limit)) {
            --level_;
            under_ = 0;
        }
    } else {
        over_ = under_ = 0;
    }
    return level_;
}

} // namespace bdex
