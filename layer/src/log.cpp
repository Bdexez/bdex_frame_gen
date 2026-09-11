#include "log.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace bdex {

static std::atomic<int> g_level{1};
static FILE* g_file = nullptr;
static std::mutex g_mutex;

void log_set_level(int level) { g_level.store(level); }
int  log_get_level() { return g_level.load(); }

void log_set_file(const char* path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file && g_file != stderr) fclose(g_file);
    g_file = nullptr;
    if (path && *path) g_file = fopen(path, "a");
}

void log_write(LogLevel lvl, const char* fmt, ...) {
    if (static_cast<int>(lvl) > g_level.load(std::memory_order_relaxed)) return;
    static const char* names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    const double t = std::chrono::duration<double>(clock::now() - t0).count();

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lock(g_mutex);
    FILE* out = g_file ? g_file : stderr;
    fprintf(out, "[bdex-fg %9.3f %s] %s\n", t, names[static_cast<int>(lvl)], buf);
    fflush(out);
}

} // namespace bdex
