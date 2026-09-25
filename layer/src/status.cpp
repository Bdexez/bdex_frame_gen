#include "status.h"
#include "platform.h"

#include <chrono>
#include <cstdlib>
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
        if (e.kind == "device") continue;
        if (!e.target.empty()) o << "target=" << e.target << "\n";
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

StatusFile statusParse(const std::string& text) {
    StatusFile f;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            StatusFile::Section s;
            s.kind = line.substr(1, line.find(' ') == std::string::npos ? line.size() - 2 : line.find(' ') - 1);
            f.sections.push_back(s);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (!f.sections.empty()) {
            f.sections.back().values[key] = value;
        } else if (key == "process") {
            f.process = value;
        } else if (key == "pid") {
            f.pid = std::strtol(value.c_str(), nullptr, 10);
        } else if (key == "time") {
            f.time = std::strtoll(value.c_str(), nullptr, 10);
        }
    }
    return f;
}

std::vector<std::string> statusDescribe(const StatusFile& file) {
    std::vector<std::string> out;
    auto num = [](const std::map<std::string, std::string>& v, const char* k, double def) {
        auto it = v.find(k);
        return it == v.end() ? def : std::strtod(it->second.c_str(), nullptr);
    };
    auto get = [](const std::map<std::string, std::string>& v, const char* k) {
        auto it = v.find(k);
        return it == v.end() ? std::string() : it->second;
    };
    for (const auto& s : file.sections) {
        const auto& v = s.values;
        const std::string state = get(v, "state");
        if (s.kind == "device") {
            if (state != "active") out.push_back("Couche inactive : " + (v.count("reason") ? get(v, "reason") : state));
            continue;
        }
        const std::string who = s.kind == "capture" && !get(v, "target").empty() ? get(v, "target") + " : " : "";
        if (state == "pass-through") {
            out.push_back(who + "transparente — " + (v.count("reason") ? get(v, "reason") : "raison inconnue"));
            continue;
        }
        char buf[160];
        snprintf(buf, sizeof buf, "%.0f → %.0f i/s (×%s, %s)", num(v, "game_fps", 0), num(v, "output_fps", 0),
                 get(v, "multiplier").c_str(), get(v, "mode") == "extrapolate" ? "extrapolation" : "interpolation");
        std::string line = (state == "paused" ? "En pause — " : "") + who + buf;
        if (num(v, "refresh_hz", 0) > 0) {
            snprintf(buf, sizeof buf, " · écran %.0f Hz", num(v, "refresh_hz", 0));
            line += buf;
        }
        if (num(v, "latency_ms", -1) >= 0) {
            snprintf(buf, sizeof buf, " · latence %.0f ms", num(v, "latency_ms", -1));
            line += buf;
        }
        if (num(v, "gpu_budget_level", 0) > 0) {
            snprintf(buf, sizeof buf, " · budget GPU : niveau %.0f", num(v, "gpu_budget_level", 0));
            line += buf;
        }
        out.push_back(line);
        if (!get(v, "passes").empty()) {
            std::string p = get(v, "passes");
            for (size_t i = 0; (i = p.find(',', i)) != std::string::npos; i += 2) p.replace(i, 1, ", ");
            out.push_back("   passes : " + p);
        }
        if (!get(v, "online_tune").empty()) out.push_back("   réglage auto : " + get(v, "online_tune"));
    }
    return out;
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
