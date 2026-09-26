#include "budget.h"
#include "test.h"

using namespace bdex;

TEST(budget_steps_down_after_two_windows) {
    GpuBudget b;
    // 16.7 ms frames, 25 % budget = 4.2 ms.
    CHECK(b.update(6.0, 16.7, 0.25, 4) == 0);   // one spike: no change
    CHECK(b.update(6.0, 16.7, 0.25, 4) == 1);
    CHECK(b.update(5.0, 16.7, 0.25, 4) == 1);
    CHECK(b.update(5.0, 16.7, 0.25, 4) == 2);
    CHECK(b.update(9.0, 16.7, 0.25, 2) == 2);   // capped by maxLevel
    CHECK(b.update(9.0, 16.7, 0.25, 2) == 2);
}

TEST(budget_ignores_isolated_spikes) {
    GpuBudget b;
    for (int i = 0; i < 10; ++i) {
        CHECK(b.update(6.0, 16.7, 0.25, 4) == 0);
        CHECK(b.update(3.0, 16.7, 0.25, 4) == 0);
    }
}

TEST(budget_steps_up_only_when_the_level_above_fits) {
    GpuBudget b;
    b.update(6.0, 16.7, 0.25, 4);
    CHECK(b.update(6.0, 16.7, 0.25, 4) == 1);   // level 0 costs 6 ms: does not fit
    for (int i = 0; i < 20; ++i) CHECK(b.update(2.0, 16.7, 0.25, 4) == 1);
    // The game slows down (33 ms frames): 6 ms now fits in 8.3, go back up.
    int l = 1;
    for (int i = 0; i < 5; ++i) l = b.update(2.0, 33.3, 0.25, 4);
    CHECK(l == 0);
}

TEST(budget_off_or_unmeasured_keeps_the_level) {
    GpuBudget b;
    CHECK(b.update(0.0, 16.7, 0.25, 4) == 0);
    CHECK(b.update(6.0, 0.0, 0.25, 4) == 0);
    CHECK(b.update(6.0, 16.7, 0.0, 4) == 0);
}
