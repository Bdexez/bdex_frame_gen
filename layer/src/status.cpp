#include "status.h"
#include "platform.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace bdex {

namespace {

struct Board {
    std::mutex mutex;
    std::map<uint64_t, StatusEntry> entries;
    std::chrono::steady_clock::time_point written{};
    bool dirAttempted = false;
    std::string path;  // "" = unavailable
};

// Never destroyed (like the layer's global tables): games may exit without
// tearing their devices down.
Board& board() {
    static Board* b = new Board();
    return *b;
}

void writeLocked(Board& b) {
    if (!b.dirAttempted) {
        b.dirAttempted = true;
        const std::string dir = platformStatusDir();
        std::error_code ec;
        if (!dir.empty() && (std::filesystem::create_directories(dir, ec) || std::filesystem::is_directory(dir, ec)))
            b.path = dir + "/" + std::to_string(platformPid()) + ".status";
    }
    if (b.path.empty()) return;
    std::error_code ec;
    if (b.entries.empty()) {
        std::filesystem::remove(b.path, ec);
        return;
    }
    const auto names = platformProcessNames();
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string tmp = b.path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        out << statusFormat(names.empty() ? std::string("?") : names.front(), platformPid(), now, b.entries);
    }
    std::filesystem::rename(tmp, b.path, ec);  // atomic for the panel's reader
    b.written = std::chrono::steady_clock::now();
}

} // namespace

std::string statusFormat(const std::string& process, long pid, long long unixTime,
                         const std::map<uint64_t, StatusEntry>& entries) {
    std::ostringstream o;
    o << "process=" << process << "\npid=" << pid << "\ntime=" << unixTime << "\n";
    int n = 0;
    for (const auto& [key, e] : entries) {
        o << "\n[" << e.kind << " " << ++n << "]\n";
        o << "state=" << e.state << "\n";
        if (!e.reason.empty()) o << "reason=" << e.reason << "\n";
        if (!e.device.empty()) o << "device=" << e.device << "\n";
        if (e.kind != "swapchain") continue;
        char buf[64];
        o << "size=" << e.width << "x" << e.height << "\n";
        if (!e.mode.empty()) o << "mode=" << e.mode << "\n";
        o << "multiplier=" << e.multiplier << "\n";
        snprintf(buf, sizeof buf, "%.1f", e.gameFps);
        o << "game_fps=" << buf << "\n";
        snprintf(buf, sizeof buf, "%.1f", e.outFps);
        o << "output_fps=" << buf << "\n";
        snprintf(buf, sizeof buf, "%.1f", e.refreshHz);
        o << "refresh_hz=" << buf << "\n";
        snprintf(buf, sizeof buf, "%.1f", e.latencyMs);
        o << "latency_ms=" << buf << "\n";
        o << "gpu_budget_level=" << e.budgetLevel << "\n";
        if (!e.passes.empty()) o << "passes=" << e.passes << "\n";
        if (!e.tune.empty()) o << "online_tune=" << e.tune << "\n";
    }
    return o.str();
}

void statusSet(uint64_t key, const StatusEntry& entry) {
    Board& b = board();
    std::lock_guard<std::mutex> lock(b.mutex);
    auto it = b.entries.find(key);
    const bool changed = it == b.entries.end() || it->second.state != entry.state || it->second.reason != entry.reason;
    b.entries[key] = entry;
    if (changed || std::chrono::steady_clock::now() - b.written >= std::chrono::seconds(1)) writeLocked(b);
}

void statusRemove(uint64_t key) {
    Board& b = board();
    std::lock_guard<std::mutex> lock(b.mutex);
    if (b.entries.erase(key)) writeLocked(b);
}

} // namespace bdex
