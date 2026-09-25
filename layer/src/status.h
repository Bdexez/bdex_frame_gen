#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace bdex {

// Live status of this process for the control panel (tools/bdex-framegen-gui):
// one small key=value file per process, <platformStatusDir()>/<pid>.status,
// rewritten atomically at most once a second (at once when a state changes)
// and removed when the last entry goes. It tells the user what the layer is
// doing in a running game — and, above all, why it passes frames through
// when it does, which otherwise takes a log file to find out.
struct StatusEntry {
    std::string kind;        // "device" or "swapchain"
    std::string state;       // generating | paused | pass-through | active | anti-cheat | inactive
    std::string reason;      // why pass-through / inactive (empty otherwise)
    std::string mode;        // interpolate | extrapolate
    int multiplier = 0;
    double gameFps = 0.0, outFps = 0.0;
    double refreshHz = 0.0;  // measured (present feedback), 0 unknown
    double latencyMs = -1.0; // present-to-screen, -1 unknown
    int budgetLevel = 0;     // gpu_budget quality level
    uint32_t width = 0, height = 0;
    std::string device;      // GPU name
};

// Keys: any Vulkan handle or pointer, on 64- and 32-bit builds alike (a
// non-dispatchable handle is a pointer on one, a uint64_t on the other).
template <typename H> uint64_t statusKey(H handle) { return (uint64_t)(handle); }

void statusSet(uint64_t key, const StatusEntry& entry);
void statusRemove(uint64_t key);

// The file's text (unit-tested).
std::string statusFormat(const std::string& process, long pid, long long unixTime,
                         const std::map<uint64_t, StatusEntry>& entries);

} // namespace bdex
