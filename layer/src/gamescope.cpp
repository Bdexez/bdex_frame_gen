#include "gamescope.h"
#include "platform.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace bdex {

namespace {
bool envIs(const char* name, const char* value) {
    const char* v = platformEnv(name);
    return v && std::strcmp(v, value) == 0;
}
} // namespace

bool gamescopeWsi() {
#ifdef _WIN32
    return false;
#else
    static const bool on = [] {
        const char* display = platformEnv("GAMESCOPE_WAYLAND_DISPLAY");
        return display && *display && envIs("ENABLE_GAMESCOPE_WSI", "1") && !envIs("DISABLE_GAMESCOPE_WSI", "1");
    }();
    return on;
#endif
}

bool gamescopeLimiterActive() {
#ifdef _WIN32
    return false;
#else
    static int fd = -1;
    static std::once_flag opened;
    std::call_once(opened, [] {
        if (const char* path = platformEnv("GAMESCOPE_LIMITER_FILE"); path && *path)
            fd = open(path, O_RDONLY | O_CLOEXEC);
    });
    if (fd < 0) return false;
    uint32_t state = 0;
    return pread(fd, &state, sizeof state, 0) == static_cast<ssize_t>(sizeof state) && state == 1;
#endif
}

} // namespace bdex
