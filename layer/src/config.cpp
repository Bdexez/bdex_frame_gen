#include "config.h"
#include "log.h"
#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

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
    } else if (v == "enhanced") {
        // Balanced flow plus the opt-ins measured positive together on the
        // corpus and the demo replays (2026-09-25, x2, vs the defaults):
        // interpolation +0.08 to +10.8 dB (demo +0.71, fast demo +0.28,
        // occlusions +2.8, HUD +0.75), extrapolation -0.04 to +7.1 (demo
        // +2.0). zero_bias_contrast at 0.02: at 0.1 it lost 1.2 dB on the
        // demo together with pixel_vectors. Costs ~2x the synthesis
        // (pixel_vectors). frame_timing, extrapolate_accel and reveal_fill
        // lose on some content: left out.
        flowScale = 0; refineAll = true; flowIterations = 1; levels = 4; searchRadius = 4; searchFine = 2;
        zeroBiasContrast = 0.02f; pixelVectors = true; flowTemporal = 1.f; hudMask = 24; sceneCutLocal = true;
        dedup = true; extrapolateSplat = true;
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

// ---------------------------------------------------------------------------
// Option registry: every option that is a plain bool / int / float member,
// declared once — the parser, the live reload, the online_tune candidates
// and the list of known keys (Config::optionKeys, checked against the GUI,
// the launcher and the README by tests/test_option_keys.py) all read it.
// Options with a syntax of their own (mode, overlay, present_mode, "auto"
// values...) are parsed in Config::apply and listed in kCustomKeys.
namespace {

enum class Kind { Bool, Int, Float };
// online_tune candidates: flow options (rated by the self-evaluation) in
// both modes or in extrapolation only; synthesis options (rated by the
// probe: a held-out frame) in the mode where their code runs.
enum class Tune { Never, Always, Extrapolation, SynthAlways, SynthInterpolation, SynthExtrapolation };
constexpr unsigned kLive = 1;  // copied by Config::applyLive (read per frame, sizes nothing)

struct OptionDef {
    const char* key;
    const char* alias;
    Kind kind;
    bool Config::*b = nullptr;
    int Config::*i = nullptr;
    float Config::*f = nullptr;
    float lo = 0.f, hi = 0.f;
    unsigned flags = 0;
    Tune tune = Tune::Never;   // online_tune candidate (a flow option the self-evaluation can rate)
    const char* trial = nullptr;  // ... with this trial value
    int tuneOrder = 0;            // ... tried in this order
};

OptionDef B(const char* k, const char* a, bool Config::*m, unsigned fl = 0, Tune t = Tune::Never,
            const char* trial = nullptr, int order = 0) {
    OptionDef d{k, a, Kind::Bool};
    d.b = m; d.flags = fl; d.tune = t; d.trial = trial; d.tuneOrder = order;
    return d;
}
OptionDef I(const char* k, const char* a, int Config::*m, int lo, int hi, unsigned fl = 0, Tune t = Tune::Never,
            const char* trial = nullptr, int order = 0) {
    OptionDef d{k, a, Kind::Int};
    d.i = m; d.lo = float(lo); d.hi = float(hi); d.flags = fl; d.tune = t; d.trial = trial; d.tuneOrder = order;
    return d;
}
OptionDef F(const char* k, const char* a, float Config::*m, float lo, float hi, unsigned fl = 0,
            Tune t = Tune::Never, const char* trial = nullptr, int order = 0) {
    OptionDef d{k, a, Kind::Float};
    d.f = m; d.lo = lo; d.hi = hi; d.flags = fl; d.tune = t; d.trial = trial; d.tuneOrder = order;
    return d;
}

const std::vector<OptionDef>& registry() {
    static const std::vector<OptionDef> defs = {
    B("enabled", "enable", &Config::enabled),
    I("multiplier", "mult", &Config::multiplier, 1, 4),
    I("levels", nullptr, &Config::levels, 1, 6),
    B("low_latency", "lowlatency", &Config::lowLatency),
    B("sync_present", "sync", &Config::syncPresent),
    B("hide_present_ext", nullptr, &Config::hidePresentExt),
    I("search", "search_radius", &Config::searchRadius, 1, 4),
    I("search_fine", nullptr, &Config::searchFine, 1, 4),
    F("render_scale", nullptr, &Config::renderScale, 0.5f, 1.0f),
    F("sharpness", "sharpen", &Config::sharpness, 0.f, 1.f),
    B("pacing", nullptr, &Config::pacing),
    B("require_app", nullptr, &Config::requireApp),
    B("present_feedback", nullptr, &Config::presentFeedback),
    B("async_compute", nullptr, &Config::asyncCompute),
    I("log", "log_level", &Config::logLevel, 0, 3),
    F("scene_cut_low", nullptr, &Config::sceneCutLow, 0.f, 1.f, kLive),
    F("scene_cut_high", nullptr, &Config::sceneCutHigh, 0.f, 1.f, kLive),
    B("scene_cut_local", nullptr, &Config::sceneCutLocal, kLive),
    F("smoothness", nullptr, &Config::smoothness, 0.f, 1.f, kLive),
    F("zero_bias", nullptr, &Config::zeroBias, 0.f, 1.f, kLive),
    F("zero_bias_contrast", nullptr, &Config::zeroBiasContrast, 0.f, 1.f, kLive, Tune::Always, "0.1", 1),
    F("grad_weight", nullptr, &Config::gradWeight, 0.f, 8.f, kLive),
    F("census_weight", nullptr, &Config::censusWeight, 0.f, 8.f, kLive),
    F("census_margin", nullptr, &Config::censusMargin, 0.f, 1.f, kLive),
    F("rot_weight", nullptr, &Config::rotWeight, 0.f, 1.f, kLive),
    F("chroma_weight", nullptr, &Config::chromaWeight, 0.f, 8.f),
    F("occlusion_strength", nullptr, &Config::occlusionStrength, 0.f, 1.f, kLive, Tune::SynthInterpolation, "1", 10),
    B("temporal_prediction", nullptr, &Config::temporalPrediction, kLive, Tune::Always, "1", 2),
    B("extrapolate_splat", nullptr, &Config::extrapolateSplat, kLive, Tune::SynthExtrapolation, "1", 10),
    F("extrapolate_hold", nullptr, &Config::extrapolateHold, 0.f, 1.f, kLive),
    F("extrapolate_accel", nullptr, &Config::extrapolateAccel, 0.f, 1.f, kLive, Tune::SynthExtrapolation, "1", 11),
    F("flow_temporal", nullptr, &Config::flowTemporal, 0.f, 1.f, kLive, Tune::Always, "1", 4),
    I("subpixel_lk", nullptr, &Config::subpixelLk, 0, 4, kLive, Tune::Extrapolation, "2", 5),
    B("synth_bicubic", nullptr, &Config::synthBicubic, kLive, Tune::SynthAlways, "1", 13),
    I("hud_mask", nullptr, &Config::hudMask, 0, 1000, kLive),
    B("reveal_fill", nullptr, &Config::revealFill, kLive, Tune::SynthExtrapolation, "1", 12),
    B("online_tune", nullptr, &Config::onlineTune),
    F("upscale_temporal", nullptr, &Config::upscaleTemporal, 0.f, 0.95f),
    B("frame_timing", nullptr, &Config::frameTiming, kLive),
    B("dedup", nullptr, &Config::dedup),
    B("paused", "pause", &Config::paused, kLive),
    F("gpu_budget", nullptr, &Config::gpuBudget, 0.f, 1.f),
    B("pixel_vectors", nullptr, &Config::pixelVectors, kLive, Tune::SynthInterpolation, "1", 11),
    B("subpixel", nullptr, &Config::subpixel, kLive, Tune::Extrapolation, "1", 3),
    // Not an online_tune candidate: the probe rebuilds a frame from frames two
    // apart, where an object crossing a pixel leaves it unchanged at both
    // ends — static_guard's known failure, magnified (demo: +0.32 dB for real,
    // +6.9 % error in the probe).
    F("static_guard", nullptr, &Config::staticGuard, 0.f, 1.f, kLive),
    B("refine", nullptr, &Config::refine),
    B("refine_all", nullptr, &Config::refineAll),
    I("flow_iterations", nullptr, &Config::flowIterations, 0, 3, kLive),
    F("refine_bias", nullptr, &Config::refineBias, 0.f, 64.f, kLive),
    B("stats", nullptr, &Config::stats),
    B("profile", nullptr, &Config::profile),
    B("shared_queue", nullptr, &Config::sharedQueue),
    F("stats_interval", nullptr, &Config::statsInterval, 0.5f, 3600.f),
    I("dump_frames", nullptr, &Config::dumpFrames, 1, 100000),
    };
    return defs;
}

// Parsed by hand in Config::apply (their own value syntax): key, alias.
const std::pair<const char*, const char*> kCustomKeys[] = {
    {"preset", nullptr},      {"fullres", nullptr},    {"flow_scale", nullptr},     {"mode", nullptr},
    {"upscale", nullptr},     {"overlay", "hud"},      {"upscale_filter", nullptr}, {"capture_hdr", nullptr},
    {"log_file", nullptr},    {"target_fps", nullptr}, {"fps_limit", "fps_cap"},    {"dump", "dump_dir"},
    {"present_mode", nullptr}, {"debug", nullptr}};

const OptionDef* findOption(const std::string& key) {
    for (const auto& d : registry())
        if (key == d.key || (d.alias && key == d.alias)) return &d;
    return nullptr;
}

} // namespace

std::vector<std::string> Config::optionKeys(bool aliases) {
    std::vector<std::string> keys;
    for (const auto& d : registry()) {
        keys.push_back(d.key);
        if (aliases && d.alias) keys.push_back(d.alias);
    }
    for (const auto& [k, a] : kCustomKeys) {
        keys.push_back(k);
        if (aliases && a) keys.push_back(a);
    }
    return keys;
}

// online_tune candidates that are several options tried together: options
// measured to pay only in combination (sub-pixel vectors in interpolation
// lose 0.8 dB alone — the synthesis's bilinear sampling blurs — and gain
// with the Catmull-Rom sampling). Key "a+b", value "x+y" (OnlineTune expands
// them); rated by the synthesis probe, in the mode given.
struct TunePair {
    const char* keys;
    const char* values;
    bool interpolation;  // else extrapolation
    int order;
};
const TunePair kTunePairs[] = {
    {"subpixel_lk+synth_bicubic", "2+1", true, 14},
    {"subpixel+synth_bicubic", "1+1", true, 15},
};

std::string Config::canonicalKey(const std::string& key) {
    if (const OptionDef* d = findOption(key)) return d->key;
    for (const auto& [k, a] : kCustomKeys)
        if (key == k || (a && key == a)) return k;
    return "";
}

std::vector<Config::OptionInfo> Config::optionInfo() {
    const Config defaults;
    auto num = [](double v) {
        char buf[32];
        snprintf(buf, sizeof buf, "%g", v);
        return std::string(buf);
    };
    std::vector<OptionInfo> out;
    for (const auto& d : registry()) {
        OptionInfo i;
        i.key = d.key;
        i.live = (d.flags & kLive) != 0;
        i.lo = d.lo;
        i.hi = d.hi;
        switch (d.kind) {
        case Kind::Bool:  i.kind = "bool"; i.lo = 0; i.hi = 1; i.def = defaults.*(d.b) ? "1" : "0"; break;
        case Kind::Int:   i.kind = "int"; i.def = std::to_string(defaults.*(d.i)); break;
        case Kind::Float: i.kind = "float"; i.def = num(defaults.*(d.f)); break;
        }
        out.push_back(i);
    }
    return out;
}

std::vector<Config::TuneCandidate> Config::tuneCandidates() const {
    std::vector<const OptionDef*> defs;
    for (const auto& d : registry()) {
        const bool on = d.tune == Tune::Always || d.tune == Tune::SynthAlways ||
                        ((d.tune == Tune::Extrapolation || d.tune == Tune::SynthExtrapolation) && extrapolate) ||
                        (d.tune == Tune::SynthInterpolation && !extrapolate);
        if (on) defs.push_back(&d);
    }
    std::stable_sort(defs.begin(), defs.end(), [](const OptionDef* a, const OptionDef* b) { return a->tuneOrder < b->tuneOrder; });
    std::vector<std::pair<int, TuneCandidate>> out;
    for (const OptionDef* d : defs)
        out.push_back({d->tuneOrder, {d->key, d->trial,
                       d->tune == Tune::SynthAlways || d->tune == Tune::SynthInterpolation || d->tune == Tune::SynthExtrapolation}});
    for (const TunePair& p : kTunePairs)
        if (p.interpolation != extrapolate) out.push_back({p.order, {p.keys, p.values, true}});
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<TuneCandidate> list;
    for (auto& [order, c] : out) list.push_back(c);
    return list;
}

bool Config::apply(const std::string& rawKey, const std::string& rawValue) {
    const std::string key = lower(trim(rawKey));
    const std::string value = trim(rawValue);
    // Any explicit flow-quality option pins the settings and disables GPU auto-tuning.
    for (const char* k : {"levels", "fullres", "flow_scale", "search", "search_radius", "search_fine",
                          "refine", "refine_all", "flow_iterations"})
        if (key == k) { autoTune = false; break; }
    if (key == "preset")                      return applyPreset(value);
    if (const OptionDef* d = findOption(key)) {
        switch (d->kind) {
        case Kind::Bool:  return parseBool(value, this->*(d->b));
        case Kind::Int:   return parseInt(value, this->*(d->i), int(d->lo), int(d->hi));
        case Kind::Float: return parseFloat(value, this->*(d->f), d->lo, d->hi);
        }
    }
    if (key == "fullres")                     { bool b; if (!parseBool(value, b)) return false; flowScale = b ? 1 : 0; return true; }
    if (key == "flow_scale") {
        const std::string v = lower(value);
        if (v == "auto" || v == "0") { flowScale = 0; return true; }
        int x;
        if (!parseInt(v, x, 1, 4) || (x != 1 && x != 2 && x != 4)) return false;
        flowScale = x;
        return true;
    }
    if (key == "mode") {
        const std::string v = lower(value);
        if (v == "interpolate" || v == "interp") extrapolate = false;
        else if (v == "extrapolate" || v == "extrap" || v == "lowlatency") extrapolate = true;
        else return false;
        return true;
    }
    if (key == "upscale") {  // display / render ratio, e.g. 1.5 -> renderScale 0.667
        float ratio;
        if (!parseFloat(value, ratio, 1.0f, 2.0f)) return false;
        renderScale = 1.0f / ratio;
        return true;
    }
    if (key == "overlay" || key == "hud") {
        const std::string v = lower(value);
        if (v == "off" || v == "false" || v == "no" || v == "none")        { overlayMode = 0; return true; }
        if (v == "fps")                                                     { overlayMode = 1; return true; }
        if (v == "framegen" || v == "fg" || v == "on" || v == "true" || v == "yes") { overlayMode = 2; return true; }
        if (v == "low" || v == "1low" || v == "lows")                       { overlayMode = 3; return true; }
        if (v == "full" || v == "detailed" || v == "all")                   { overlayMode = 4; return true; }
        if (v == "latency" || v == "ms")                                    { overlayMode = 5; return true; }
        return parseInt(v, overlayMode, 0, 5);   // explicit level 0..5
    }
    if (key == "upscale_filter") {
        const std::string v = lower(value);
        if (v == "bilinear" || v == "0") upscaleFilter = 0;
        else if (v == "bicubic" || v == "catmull" || v == "catmull-rom" || v == "1") upscaleFilter = 1;
        else if (v == "lanczos" || v == "lanczos2" || v == "2") upscaleFilter = 2;
        else return false;
        return true;
    }
    if (key == "capture_hdr") {
        if (lower(value) == "auto") { captureHdr = -1; return true; }
        bool b;
        if (!parseBool(value, b)) return false;
        captureHdr = b ? 1 : 0;
        return true;
    }
    if (key == "log_file")                    { logFile = platformExpandPath(value); return true; }
    // "auto" (-1): from the refresh rate the present feedback measures.
    if (key == "target_fps") {
        if (lower(value) == "auto") { targetFps = -1.f; return true; }
        return parseFloat(value, targetFps, 0.f, 1000.f);
    }
    if (key == "fps_limit" || key == "fps_cap") {
        if (lower(value) == "auto") { fpsLimit = -1.f; return true; }
        return parseFloat(value, fpsLimit, 0.f, 1000.f);
    }
    if (key == "dump" || key == "dump_dir")   { dumpDir = value; return true; }
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
    return platformProcessNames();
}

bool Config::sectionMatches(const std::string& section, const std::vector<std::string>& names) {
    const std::string s = baseName(trim(section));
    if (s.empty() || s == "*") return true;
    return std::find(names.begin(), names.end(), s) != names.end();
}

void Config::loadFile(const std::string& path, const std::string& processName, bool bundled) {
    std::ifstream in(path);
    if (!in) return;
    std::vector<std::string> names = processNames();
    if (!processName.empty()) names.push_back(baseName(processName));
    std::string line;
    int lineNo = 0;
    bool active = !bundled;  // lines before any section are global
    while (std::getline(in, line)) {
        ++lineNo;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            const std::string section = trim(t.substr(1, t.size() - 2));
            // Shipped profiles only tune named games; they never apply globally.
            active = !(bundled && section == "*") && sectionMatches(section, names);
            continue;
        }
        if (!active) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (bundled) {
            // Turning the layer on (or pausing it) stays the user's decision.
            const std::string key = lower(trim(t.substr(0, eq)));
            if (key == "enabled" || key == "enable" || key == "paused" || key == "pause") continue;
        }
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
    for (char** e = platformEnviron(); e && *e; ++e) {
        std::string entry(*e);
        if (entry.compare(0, sizeof(prefix) - 1, prefix) != 0) continue;
        size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        std::string key = entry.substr(sizeof(prefix) - 1, eq - (sizeof(prefix) - 1));
        std::string value = entry.substr(eq + 1);
        if (lower(key) == "config" || lower(key) == "preset" || lower(key) == "disable") continue;  // loader handles disable
        if (!apply(key, value))
            BDEX_WARN("ignored environment option %s", entry.c_str());
    }
    // BDEX_FG=0 disables generation even though the layer is loaded.
    if (const char* v = getenv("BDEX_FG")) {
        bool b;
        if (parseBool(v, b)) enabled = b;
        if (enabled) requireApp = false;  // explicit per-game launch (Steam option, CLI)
    }
}

Config Config::load(const std::string& extraProcess) {
    return loadWithDefaults(Config{}, extraProcess);
}

std::string Config::filePath() {
    if (const char* p = getenv("BDEX_FG_CONFIG")) return p;
    return platformConfigPath();
}

void Config::applyLive(const Config& n) {
    // Only settings read by the presenting thread while recording a frame
    // (push constants and per-frame decisions): anything that sizes
    // resources (multiplier, render scale, levels, flow scale, HUD, formats)
    // or that the presentation worker reads (pacing, present mode) needs a
    // restart. Environment overrides still win, since `n` was loaded with them.
    for (const auto& d : registry()) {
        if (!(d.flags & kLive)) continue;
        if (d.b) this->*(d.b) = n.*(d.b);
        if (d.i) this->*(d.i) = n.*(d.i);
        if (d.f) this->*(d.f) = n.*(d.f);
    }
    fpsLimit = n.fpsLimit; targetFps = n.targetFps;
    if (gpuBudget > 0.f && n.gpuBudget > 0.f) gpuBudget = n.gpuBudget;  // 0 <-> >0 creates the timestamp queries
    // (dedup sizes the output stage in extrapolation: restart.)
    if (sharpness > 0.f && n.sharpness > 0.f) sharpness = n.sharpness;  // 0 <-> >0 changes the passes
    if (chromaWeight > 0.f && n.chromaWeight > 0.f) chromaWeight = n.chromaWeight;  // likewise (chroma pyramid)
}

Config Config::loadWithDefaults(Config c, const std::string& extraProcess) {
    const std::string profiles = platformBundledProfilesPath();
    if (!profiles.empty()) c.loadFile(profiles, extraProcess, true);
    const std::string path = filePath();
    if (!path.empty()) c.loadFile(path, extraProcess);
    c.loadEnv();
    // Multiplayer anti-cheat present: stay a pass-through for this process,
    // whatever the config/env says. Injection into a protected process is
    // what anti-cheats flag, so the safe side wins (see platform.cpp).
    std::string antiCheat;
    if (platformAntiCheatPresent(&antiCheat)) {
        c.enabled = false;
        c.antiCheatDisabled = true;
        BDEX_WARN("anti-cheat '%s' detected: layer disabled for this process", antiCheat.c_str());
    }
    // require_app: the control panel must be open with generation started.
    if (c.enabled && c.requireApp && !platformAppSessionActive()) {
        c.enabled = false;
        BDEX_INFO("control panel not started (require_app): layer inactive for this process");
    }
    if (c.sceneCutHigh < c.sceneCutLow) c.sceneCutHigh = c.sceneCutLow;
    if (!extraProcess.empty()) c.tuneName = baseName(extraProcess);
    else if (const auto names = processNames(); !names.empty()) c.tuneName = names.front();
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
    if (gradWeight > 0.f) o << " grad_weight=" << gradWeight;
    if (censusWeight > 0.f) o << " census_weight=" << censusWeight << " census_margin=" << censusMargin;
    if (rotWeight > 0.f) o << " rot_weight=" << rotWeight;
    if (chromaWeight > 0.f) o << " chroma_weight=" << chromaWeight;
    if (occlusionStrength > 0.f) o << " occlusion_strength=" << occlusionStrength;
    if (zeroBiasContrast > 0.f) o << " zero_bias_contrast=" << zeroBiasContrast;
    if (temporalPrediction) o << " temporal_prediction=1";
    if (extrapolateSplat) o << " extrapolate_splat=1";
    if (extrapolateHold > 0.f) o << " extrapolate_hold=" << extrapolateHold;
    if (extrapolateAccel > 0.f) o << " extrapolate_accel=" << extrapolateAccel;
    if (flowTemporal > 0.f) o << " flow_temporal=" << flowTemporal;
    if (subpixelLk > 0) o << " subpixel_lk=" << subpixelLk;
    if (synthBicubic) o << " synth_bicubic=1";
    if (hudMask > 0) o << " hud_mask=" << hudMask;
    if (revealFill) o << " reveal_fill=1";
    if (onlineTune) o << " online_tune=1";
    if (upscaleTemporal > 0.f) o << " upscale_temporal=" << upscaleTemporal;
    if (targetFps != 0.f) o << " target_fps=" << (targetFps < 0.f ? std::string("auto") : std::to_string(targetFps));
    if (fpsLimit != 0.f) o << " fps_limit=" << (fpsLimit < 0.f ? std::string("auto") : std::to_string(fpsLimit));
    if (gpuBudget > 0.f) o << " gpu_budget=" << gpuBudget;
    if (pixelVectors) o << " pixel_vectors=1";
    if (subpixel) o << " subpixel=1";
    if (staticGuard > 0.f) o << " static_guard=" << staticGuard;
    if (frameTiming) o << " frame_timing=1";
    if (sceneCutLocal) o << " scene_cut_local=1";
    if (dedup) o << " dedup=1";
    if (antiCheatDisabled) o << " [disabled: anti-cheat detected]";
    return o.str();
}

} // namespace bdex
