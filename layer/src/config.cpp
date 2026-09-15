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
    case -1:                               return "app";
    default:                               return "auto";
    }
}

bool Config::applyPreset(const std::string& name) {
    const std::string v = lower(trim(name));
    if (v == "auto") { autoTune = true; return true; }  // resolved later by autoConfigure()
    // An explicit preset pins the quality settings; stop auto-tuning from the GPU.
    autoTune = false;
    if (v == "balanced" || v == "default") {
        flowScale = 0; refineAll = true; flowIterations = 1; levels = 4; searchRadius = 4; searchFine = 2;
    } else if (v == "quality") {
        flowScale = 1; refineAll = true; flowIterations = 2; levels = 5; searchRadius = 4; searchFine = 2;
    } else if (v == "performance" || v == "perf" || v == "fast") {
        flowScale = 4; refineAll = false; flowIterations = 0; levels = 4; searchRadius = 4; searchFine = 2;
    } else if (v == "latency" || v == "lowlatency" || v == "low_latency") {
        // Minimal input lag: extrapolation presents the real frame immediately
        // (no half-frame hold), x2 keeps the least work on the critical path,
        // balanced flow otherwise. Mailbox (the default present mode) completes
        // the picture; measured added hold ~0.1 ms vs ~8.4 ms for interpolation.
        extrapolate = true; multiplier = 2;
        flowScale = 0; refineAll = true; flowIterations = 1; levels = 4; searchRadius = 4; searchFine = 2;
    } else {
        return false;
    }
    return true;
}

void Config::autoConfigure(const VkPhysicalDeviceProperties& props, const VkPhysicalDeviceMemoryProperties& mem) {
    if (!autoTune) return;  // the user pinned a preset or a flow option
    uint64_t vram = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i)
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            vram = std::max<uint64_t>(vram, mem.memoryHeaps[i].size);
    // Discrete GPUs with a healthy amount of VRAM run the balanced flow (itself
    // resolution-adaptive); integrated, software and small/old GPUs get the
    // cheaper performance flow. Everything stays overridable.
    const bool strong = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && vram >= (2ull << 30);
    const std::string preset = strong ? "balanced" : "performance";
    applyPreset(preset);   // clears autoTune
    autoTune = true;       // keep the marker: this was auto-tuned, not user-pinned
    autoTunedTo = preset;
}

bool Config::apply(const std::string& rawKey, const std::string& rawValue) {
    const std::string key = lower(trim(rawKey));
    const std::string value = trim(rawValue);
    // Any explicit flow-quality option pins the settings and disables GPU auto-tuning.
    for (const char* k : {"levels", "fullres", "flow_scale", "search", "search_radius", "search_fine",
                          "refine", "refine_all", "flow_iterations"})
        if (key == k) { autoTune = false; break; }
    if (key == "preset")                      return applyPreset(value);
    if (key == "enabled" || key == "enable")  return parseBool(value, enabled);
    if (key == "multiplier" || key == "mult") return parseInt(value, multiplier, 1, 4);
    if (key == "levels")                      return parseInt(value, levels, 1, 6);
    if (key == "fullres")                     { bool b; if (!parseBool(value, b)) return false; flowScale = b ? 1 : 0; return true; }
    if (key == "flow_scale") {
        const std::string v = lower(value);
        if (v == "auto" || v == "0") { flowScale = 0; return true; }
        int x;
        if (!parseInt(v, x, 1, 4) || (x != 1 && x != 2 && x != 4)) return false;
        flowScale = x;
        return true;
    }
    if (key == "low_latency" || key == "lowlatency") return parseBool(value, lowLatency);
    if (key == "sync_present" || key == "sync")      return parseBool(value, syncPresent);
    if (key == "hide_present_ext")            return parseBool(value, hidePresentExt);
    if (key == "mode") {
        const std::string v = lower(value);
        if (v == "interpolate" || v == "interp") extrapolate = false;
        else if (v == "extrapolate" || v == "extrap" || v == "lowlatency") extrapolate = true;
        else return false;
        return true;
    }
    if (key == "search" || key == "search_radius") return parseInt(value, searchRadius, 1, 4);
    if (key == "search_fine")                 return parseInt(value, searchFine, 1, 4);
    if (key == "render_scale")                return parseFloat(value, renderScale, 0.5f, 1.0f);
    if (key == "upscale") {  // display / render ratio, e.g. 1.5 -> renderScale 0.667
        float ratio;
        if (!parseFloat(value, ratio, 1.0f, 2.0f)) return false;
        renderScale = 1.0f / ratio;
        return true;
    }
    if (key == "sharpness" || key == "sharpen") return parseFloat(value, sharpness, 0.f, 1.f);
    if (key == "overlay" || key == "hud")     return parseBool(value, overlay);
    if (key == "upscale_filter") {
        const std::string v = lower(value);
        if (v == "bilinear" || v == "0") upscaleFilter = 0;
        else if (v == "bicubic" || v == "catmull" || v == "catmull-rom" || v == "1") upscaleFilter = 1;
        else if (v == "lanczos" || v == "lanczos2" || v == "2") upscaleFilter = 2;
        else return false;
        return true;
    }
    if (key == "pacing")                      return parseBool(value, pacing);
    if (key == "log" || key == "log_level")   return parseInt(value, logLevel, 0, 3);
    if (key == "log_file")                    { logFile = value; return true; }
    if (key == "scene_cut_low")               return parseFloat(value, sceneCutLow, 0.f, 1.f);
    if (key == "scene_cut_high")              return parseFloat(value, sceneCutHigh, 0.f, 1.f);
    if (key == "smoothness")                  return parseFloat(value, smoothness, 0.f, 1.f);
    if (key == "zero_bias")                   return parseFloat(value, zeroBias, 0.f, 1.f);
    if (key == "refine")                      return parseBool(value, refine);
    if (key == "refine_all")                  return parseBool(value, refineAll);
    if (key == "flow_iterations")             return parseInt(value, flowIterations, 0, 3);
    if (key == "refine_bias")                 return parseFloat(value, refineBias, 0.f, 64.f);
    if (key == "stats")                       return parseBool(value, stats);
    if (key == "profile")                     return parseBool(value, profile);
    if (key == "shared_queue")                return parseBool(value, sharedQueue);
    if (key == "stats_interval")              return parseFloat(value, statsInterval, 0.5f, 3600.f);
    if (key == "dump" || key == "dump_dir")   { dumpDir = value; return true; }
    if (key == "dump_frames")                 return parseInt(value, dumpFrames, 1, 100000);
    if (key == "present_mode") {
        const std::string v = lower(value);
        if (v == "auto" || v.empty())               presentMode = -2;
        else if (v == "app" || v == "game")          presentMode = -1;
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

static std::string baseName(std::string s) {
    size_t p = s.find_last_of("/\\");
    if (p != std::string::npos) s = s.substr(p + 1);
    s = lower(s);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0) s.resize(s.size() - 4);
    return s;
}

std::vector<std::string> Config::processNames() {
    std::vector<std::string> names;
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string arg;
    while (std::getline(in, arg, '\0')) {
        std::string b = baseName(trim(arg));
        if (!b.empty() && std::find(names.begin(), names.end(), b) == names.end()) names.push_back(b);
    }
    return names;
}

bool Config::sectionMatches(const std::string& section, const std::vector<std::string>& names) {
    const std::string s = baseName(trim(section));
    if (s.empty() || s == "*") return true;
    return std::find(names.begin(), names.end(), s) != names.end();
}

void Config::loadFile(const std::string& path, const std::string& processName) {
    std::ifstream in(path);
    if (!in) return;
    std::vector<std::string> names = processNames();
    if (!processName.empty()) names.push_back(baseName(processName));
    std::string line;
    int lineNo = 0;
    bool active = true;
    while (std::getline(in, line)) {
        ++lineNo;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            active = sectionMatches(t.substr(1, t.size() - 2), names);
            continue;
        }
        if (!active) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (!apply(t.substr(0, eq), t.substr(eq + 1)))
            BDEX_WARN("%s:%d: ignored option '%s'", path.c_str(), lineNo, t.c_str());
    }
}

void Config::loadEnv() {
    static const char prefix[] = "BDEX_FG_";
    // The preset goes first so that individual variables override it
    // whatever the order of the environment.
    if (const char* p = getenv("BDEX_FG_PRESET")) {
        if (!applyPreset(p)) BDEX_WARN("unknown preset '%s'", p);
    }
    for (char** e = environ; e && *e; ++e) {
        std::string entry(*e);
        if (entry.compare(0, sizeof(prefix) - 1, prefix) != 0) continue;
        size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        std::string key = entry.substr(sizeof(prefix) - 1, eq - (sizeof(prefix) - 1));
        std::string value = entry.substr(eq + 1);
        if (lower(key) == "config" || lower(key) == "preset") continue;  // handled above / by load()
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
    if (!path.empty()) c.loadFile(path, "");
    c.loadEnv();
    if (c.sceneCutHigh < c.sceneCutLow) c.sceneCutHigh = c.sceneCutLow;
    return c;
}

int Config::flowScaleFor(uint32_t width, uint32_t height) const {
    if (flowScale) return flowScale;
    const uint64_t pixels = uint64_t(width) * height;
    if (pixels <= 640ull * 1000) return 1;   // up to ~1024x600: full resolution is cheap
    if (pixels <= 2600ull * 1000) return 2;  // up to ~1080p/1440p-ish
    return 4;                                // 4K and beyond
}

std::string Config::describe() const {
    static const char* dbg[] = {"none", "flow", "split", "passthrough"};
    std::ostringstream o;
    o << "enabled=" << enabled << " multiplier=" << multiplier << " levels=" << levels
      << " mode=" << (extrapolate ? "extrapolate" : "interpolate") << " flow_scale=" << (flowScale ? std::to_string(flowScale) : "auto") << " search=" << searchRadius << "/" << searchFine << " refine=" << refine
      << " present_mode=" << presentModeName(presentMode) << " pacing=" << pacing
      << " debug=" << dbg[static_cast<int>(debug)] << " log=" << logLevel;
    return o.str();
}

} // namespace bdex
