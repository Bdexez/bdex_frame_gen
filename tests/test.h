#pragma once
#include <cstdio>
#include <vector>

namespace test {
struct Case { const char* name; void (*fn)(); };
std::vector<Case>& cases();
extern int failures;
struct Register { Register(const char* n, void (*f)()) { cases().push_back({n, f}); } };
} // namespace test

#define TEST(name)                                     \
    static void name();                                \
    static test::Register name##_reg(#name, name);     \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) { ++test::failures; printf("  %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); } \
    } while (0)
