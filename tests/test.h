#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
// The tests drive Config through the environment; MSVC's CRT spells it differently.
inline int setenv(const char* k, const char* v, int) { return _putenv_s(k, v); }
inline int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif

namespace test {
struct Case { const char* name; void (*fn)(); };
// Scratch file path in the platform's temp directory.
inline std::string tmpPath(const char* name) {
#ifdef _WIN32
    const char* t = std::getenv("TEMP");
    return std::string(t ? t : ".") + "\\" + name;
#else
    return std::string("/tmp/") + name;
#endif
}
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
