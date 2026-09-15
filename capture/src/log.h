// Minimal stderr logging for the capture PoC, same shape as the layer's
// statistics lines so the two products' logs read alike.
#pragma once

#include <cstdarg>
#include <cstdio>

namespace bdex {
inline void logLine(const char* level, const char* fmt, va_list ap) {
    std::fprintf(stderr, "[bdex-cap %s] ", level);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}
inline void logInfo(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logLine("INFO", fmt, ap); va_end(ap);
}
inline void logWarn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logLine("WARN", fmt, ap); va_end(ap);
}
inline void logError(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logLine("ERROR", fmt, ap); va_end(ap);
}
} // namespace bdex

#define LOGI(...) bdex::logInfo(__VA_ARGS__)
#define LOGW(...) bdex::logWarn(__VA_ARGS__)
#define LOGE(...) bdex::logError(__VA_ARGS__)
