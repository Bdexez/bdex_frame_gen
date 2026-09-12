// Tiny self-contained test harness (no external dependency).
#include "test.h"

#include <cstdio>

namespace test {
std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
int failures = 0;
} // namespace test

int main() {
    int failedCases = 0;
    for (const auto& c : test::cases()) {
        int before = test::failures;
        c.fn();
        if (test::failures != before) {
            ++failedCases;
            printf("FAIL %s\n", c.name);
        } else {
            printf("ok   %s\n", c.name);
        }
    }
    printf("%zu tests, %d failed\n", test::cases().size(), failedCases);
    return failedCases ? 1 : 0;
}
