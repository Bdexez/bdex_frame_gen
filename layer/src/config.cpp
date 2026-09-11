#include "config.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

extern char** environ;

namespace bdex {

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool parseBool(const std::string& v, bool& out) {
    std::string s = lower(trim(v));
    if (s == "1" || s == "true" || s == "on" || s == "yes") { out = true; return true; }
    if (s == "0" || s == "false" || s == "off" || s == "no") { out = false; return true; }
    return false;
}

static bool parseInt(const std::string& v, int& out, int lo, int hi) {
    char* end = nullptr;
    long x = strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0') return false;
    out = static_cast<int>(std::clamp<long>(x, lo, hi));
    return true;
}

static bool parseFloat(const std::string& v, float& out, float lo, float hi) {
    char* end = nullptr;
    float x = strtof(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0') return false;
    out = std::clamp(x, lo, hi);
    return true;
}

const char* presentModeName(int mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR:      return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR:         return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "relaxed";
    default:                               return "app";
    }
}

bool Config::apply(const std::string& rawKey, const std::string& rawValue) {
    const std::string key = lower(trim(rawKey));
    const std::string value = trim(rawValue);
    if (key == "enabled" || key == "enable")  return parseBool(value, enabled);
    if (key == "multiplier" || key == "mult") return parseInt(value, multiplier, 1, 4);
    if (key == "levels")                      return parseInt(value, levels, 1, 6);
    if (key == "fullres")                     return parseBool(value, fullres);
    if (key == "search" || key == "search_radius") return parseInt(value, searchRadius, 1, 4);
    if (key == "search_fine")                 return parseInt(value, searchFine, 1, 4);
    if (key == "pacing")                      return parseBool(value, pacing);
    if (key == "log" || key == "log_level")   return parseInt(value, logLevel, 0, 3);
    if (key == "log_file")                    { logFile = value; return true; }
    if (key == "scene_cut_low")               return parseFloat(value, sceneCutLow, 0.f, 1.f);
    if (key == "scene_cut_high")              return parseFloat(value, sceneCutHigh, 0.f, 1.f);
    if (key == "smoothness")                  return parseFloat(value, smoothness, 0.f, 1.f);
    if (key == "zero_bias")                   return parseFloat(value, zeroBias, 0.f, 1.f);
    if (key == "refine")                      return parseBool(value, refine);
    if (key == "refine_bias")                 return parseFloat(value, refineBias, 0.f, 64.f);
    if (key == "stats")                       return parseBool(value, stats);
    if (key == "profile")                     return parseBool(value, profile);
    if (key == "stats_interval")              return parseFloat(value, statsInterval, 0.5f, 3600.f);
    if (key == "dump" || key == "dump_dir")   { dumpDir = value; return true; }
    if (key == "dump_frames")                 return parseInt(value, dumpFrames, 1, 100000);
    if (key == "present_mode") {
        const std::string v = lower(value);
        if (v == "app" || v == "auto" || v.empty()) presentMode = -1;
        else if (v == "fifo" || v == "vsync")       presentMode = VK_PRESENT_MODE_FIFO_KHR;
        else if (v == "relaxed" || v == "fifo_relaxed") presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        else if (v == "mailbox")                    presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (v == "immediate" || v == "novsync") presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else return false;
        return true;
    }
    if (key == "debug") {
        const std::string v = lower(value);
        if (v == "none" || v == "0" || v.empty()) debug = Debug::None;
        else if (v == "flow")                     debug = Debug::Flow;
        else if (v == "split")                    debug = Debug::Split;
        else if (v == "passthrough" || v == "off") debug = Debug::Passthrough;
        else return false;
        return true;
    }
    return false;
}

void Config::loadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (!apply(t.substr(0, eq), t.substr(eq + 1)))
            BDEX_WARN("%s:%d: ignored option '%s'", path.c_str(), lineNo, t.c_str());
    }
}

void Config::loadEnv() {
    static const char prefix[] = "BDEX_FG_";
    for (char** e = environ; e && *e; ++e) {
        std::string entry(*e);
        if (entry.compare(0, sizeof(prefix) - 1, prefix) != 0) continue;
        size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        std::string key = entry.substr(sizeof(prefix) - 1, eq - (sizeof(prefix) - 1));
        std::string value = entry.substr(eq + 1);
        if (!apply(key, value))
            BDEX_WARN("ignored environment option %s", entry.c_str());
    }
    // BDEX_FG=0 disables generation even though the layer is loaded.
    if (const char* v = getenv("BDEX_FG")) {
        bool b;
        if (parseBool(v, b)) enabled = b;
    }
}

Config Config::load() {
    Config c;
    std::string path;
    if (const char* p = getenv("BDEX_FG_CONFIG")) path = p;
    else if (const char* x = getenv("XDG_CONFIG_HOME")) path = std::string(x) + "/bdex-framegen.conf";
    else if (const char* h = getenv("HOME")) path = std::string(h) + "/.config/bdex-framegen.conf";
    if (!path.empty()) c.loadFile(path);
    c.loadEnv();
    if (c.sceneCutHigh < c.sceneCutLow) c.sceneCutHigh = c.sceneCutLow;
    return c;
}

std::string Config::describe() const {
    static const char* dbg[] = {"none", "flow", "split", "passthrough"};
    std::ostringstream o;
    o << "enabled=" << enabled << " multiplier=" << multiplier << " levels=" << levels
      << " fullres=" << fullres << " search=" << searchRadius << "/" << searchFine << " refine=" << refine
      << " present_mode=" << presentModeName(presentMode) << " pacing=" << pacing
      << " debug=" << dbg[static_cast<int>(debug)] << " log=" << logLevel;
    return o.str();
}

} // namespace bdex
