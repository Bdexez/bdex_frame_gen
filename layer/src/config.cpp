#include "config.h"
#include "log.h"
#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <locale>
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

// Always a dot: strtof follows the process's locale, and a game or toolkit
// that sets a French one would reject "0.3" (and read "0,3"). (A stream in
// the classic locale rather than std::from_chars: its floating-point
// overloads need GCC 11, and the release build uses GCC 10 for an older
// glibc.)
static bool parseFloat(const std::string& v, float& out, float lo, float hi) {
    const size_t start = !v.empty() && v[0] == '+';
    if (start >= v.size() || std::isspace(static_cast<unsigned char>(v[start]))) return false;
    std::istringstream in(v.substr(start));
    in.imbue(std::locale::classic());
    float x = 0.f;
    in >> x;
    if (in.fail() || in.peek() != std::char_traits<char>::eof()) return false;
    out = std::clamp(x, lo, hi);
    return true;
}

const char* presentModeName(int mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR:      return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR:         return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "relaxed";
    case 1000361000:                       return "fifo_latest_ready";  // (headers before 1.4.304)
    case 1000111000: case 1000111001:      return "shared";
    case -1:                               return "app";
    default:                               return "auto";
    }
}

bool Config::applyPreset(const std::string& name) {
    const std::string v = lower(trim(name));
    if (v == "auto") { autoTune = true; presetName.clear(); return true; }  // resolved later by autoConfigure()
    // An explicit preset pins the quality settings; stop auto-tuning from the GPU.
    autoTune = false;
    presetName = v;
    if (v == "balanced" || v == "default") {
        // (search_fine 3 since 14/10, as quality: recorded games +0.02 to
        // +0.06 dB and less flicker, edge scenes up to +1.0, worst -0.02.)
        flowScale = 0; refineAll = true; flowIterations = 1; levels = 4; searchRadius = 4; searchFine = 3;
    } else if (v == "quality") {
        // Full-resolution flow up to ~1080p, half above (flowScaleFor): at
        // 1440p the full-resolution pyramid followed a third of the camera
        // motion the half-resolution one does — recorded games, fast turns:
        // +0.3 to +0.5 dB at half the flow's GPU time (13/10). A search
        // radius of 3 at the finer levels (14/10): recorded games +0.02 to
        // +0.08 dB, flicker and grossly wrong pixels down on every one, edge
        // scenes up to +1.2, worst -0.05; +0.1 to +0.2 ms of flow at 1440p.
        flowScale = -1; refineAll = true; flowIterations = 2; levels = 5; searchRadius = 4; searchFine = 3;
    } else if (v == "performance" || v == "perf" || v == "fast") {
        presetName = "performance";
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
        flowScale = 0; refineAll = true; flowIterations = 1; levels = 4; searchRadius = 4; searchFine = 3;
        zeroBiasContrast = 0.02f; pixelVectors = true; flowTemporal = 1.f; hudMask = 24; sceneCutLocal = true;
        dedup = true; extrapolateSplat = true;
    } else if (v == "latency" || v == "lowlatency" || v == "low_latency") {
        // Minimal input lag: extrapolation presents the real frame immediately
        // (no half-frame hold), x2 keeps the least work on the critical path,
        // balanced flow otherwise. Mailbox (the default present mode) completes
        // the picture; measured added hold ~0.1 ms vs ~8.4 ms for interpolation.
        extrapolate = true; hybrid = false; multiplier = 2;
        flowScale = 0; refineAll = true; flowIterations = 1; levels = 4; searchRadius = 4; searchFine = 2;
    } else {
        return false;
    }
    return true;
}

void Config::autoConfigure(const VkPhysicalDeviceProperties& props, const VkPhysicalDeviceMemoryProperties& mem) {
    uint64_t vram = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i)
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            vram = std::max<uint64_t>(vram, mem.memoryHeaps[i].size);
    // Discrete GPUs with a healthy amount of VRAM run the balanced flow (itself
    // resolution-adaptive); integrated, software and small/old GPUs get the
    // cheaper performance flow. Everything stays overridable.
    // On battery (battery_saver): the performance flow, as for a small GPU —
    // the flow is most of the generation's GPU time, so its power too.
    const bool strong = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && vram >= (2ull << 30) && !onBattery;
    if (autoTune) {  // no preset or flow option pinned by the user
        const std::string preset = strong ? "balanced" : "performance";
        applyPreset(preset);   // clears autoTune
        autoTune = true;       // keep the marker: this was auto-tuned, not user-pinned
        autoTunedTo = preset;
    }
    // The options below are synthesis features of a strong GPU, not flow
    // settings: a preset the user picked keeps them (until 13/10 any explicit
    // preset dropped them — « Qualité » in the panel ran without
    // pixel_vectors, interp_splat, rot_weight or temporal_prediction; on a
    // real game, PRAGMATA at 1440p, adding them to quality cut the grossly
    // wrong pixels by 17 % at x2 and 20 % at x3, +0.06 / +0.29 dB, and
    // +0.4 to +1.1 dB on the game-like edge sequences at x4). Not with
    // « performance », picked to spend less.
    if (!strong || presetName == "performance") return;
    // Per-pixel vectors at motion boundaries (measured: demo +0.3 dB, very
    // fast +0.6, HUD +0.7, occlusions +2.9 dB and their edges +6.6) cost a
    // discrete GPU ~0.3-0.6 ms at 1440p, 1-2 ms at 4K per generated frame:
    // on, where the synthesis stays affordable (FrameGen::effectiveConfig).
    if (strong && !(autoOptionsSet & kAutoPixelVectors)) {
        pixelVectors = true;
        pixelVectorsAuto = true;
    }
    // The predicted frames (extrapolation, hybrid): the forward splat and
    // the revealed-area fill together, measured x3 against without them —
    // extrapolation: demo +0.9, fast +1.2, very fast +0.35, pan +1.2, HUD
    // +0.7 dB; hybrid: +0.3, +0.6, +0.2, +0.4, +0.3; occlusions -0.02, swing
    // +0.02. The splat runs after the real frame (not on its path).
    if (strong && !(autoOptionsSet & kAutoSplat)) extrapolateSplat = true;
    if (strong && !(autoOptionsSet & kAutoRevealFill)) revealFill = true;
    // Both frames splatted to t as candidate trajectories of the
    // interpolated frames (03/10): on top of pixel_vectors occlusions +0.22,
    // scrolling text +0.41, popup +0.06, the demo +0.01 to +0.03, HUD -0.12;
    // alone +0.05 to +0.7. ~0.6 ms at 1440p.
    // The matcher's rotation / scale re-ranking (rot_weight), in its own
    // variant (03/10: the full one halved the occupancy): on top of
    // pixel_vectors occlusions +1.17 dB, 8-degree rotation +1.09, 5 % zoom
    // +1.1, scrolling text +0.40, sub-pixel +0.23, the demo +0.07 to +0.12,
    // nothing lost; the flow ~1.5x (+0.2 ms at 960x540).
    if (strong && !(autoOptionsSet & kAutoRot)) rotWeight = 1.f;
    // The coarsest matching level searched around the previous pair's motion
    // (03/10, on top of the options above, 960x540 corpus + demo, no GPU
    // cost): interpolation x2 occlusions +3.7 dB, zoom +4.4, fade +5.4,
    // swing +1.7, demo +0.15 / +0.19; extrapolation occlusions +0.8, swing
    // +1.3, zoom +3.9; hybrid x3 (larger motion a pair) HUD +9.8, pan +11.7,
    // menu +5.3. Losses: rotation -0.16, menu -0.18 (x2), swing -0.37
    // (hybrid). Nothing either way with the performance preset's coarse flow.
    if (strong && !(autoOptionsSet & kAutoTemporalPrediction)) temporalPrediction = true;
    // The frame's dominant shift as a second search centre of the coarsest
    // blocks (13/10, global_motion.comp): a fast camera turn in a game goes
    // past the pyramid's reach and the picture breaks up. Recorded games at
    // 1440p, fast turns: grossly wrong pixels -17 to -19 %, +0.3 to +0.8 dB
    // (with the quality preset's half-resolution flow there: -23 to -29 %,
    // +0.5 to +1.0); everything else within +-0.04 dB; ~0.05 ms.
    if (strong && !(autoOptionsSet & kAutoGlobalMotion)) globalMotion = true;
    // Pixels that stayed unchanged for 8 game frames while the camera moved
    // (a HUD: icons, text, crosshair) are kept as they are instead of being
    // dragged along by the motion of the blocks they share with the scene
    // (13/10, x4 on 30 fps game-like turns: their HUD icons came out smeared
    // and doubled — interpolation +0.21 to +0.47 dB, +0.9 to +2.0 on edges;
    // extrapolation +0.67 / +1.06; nothing lost on the recorded games, the
    // corpus or the game-like scenes without a HUD). One pass a game frame.
    // (4 frames since 14/10 — a HUD element that appears, a quest update,
    // is held after 0.13 s at 30 fps instead of 0.27: HUD scenes +0.01 to
    // +0.16 dB, extrapolation +0.13, worst -0.01; 2 caught still-looking
    // scenery: low contrast -0.72; 16 -0.14.)
    if (strong && !(autoOptionsSet & kAutoHudMask) && hudMask == 0) hudMask = 4;  // (enhanced: its own 24)
    // ... and its "holds still" evidence for the HUD pixels never quite
    // unchanged — anti-aliased text, whose 1-2 pixel strokes mix the moving
    // scene (13/10, x4 game-like scenes with anti-aliased HUD text: +0.05 to
    // +0.25 dB, edges +0.5 to +4.5; recorded games and the corpus within
    // -0.02 dB).
    if (strong && !(autoOptionsSet & kAutoStaticSoft)) staticSoft = 1.f;
    // Interpolated frames on the curved trajectory where the motion changed
    // since the previous pair, coherently over ~80 pixels (14/10, a mouse
    // turn speeding up or slowing down: recorded games +0.03 to +0.17 dB,
    // flicker -0.04 to -0.51, grossly wrong pixels down on every one; scenes
    // at constant speed within -0.02, one upscaled recording -0.09; +0.1 ms a
    // game frame at 1440p x4, accel_map.comp).
    if (strong && !(autoOptionsSet & kAutoInterpAccel)) interpAccel = 1.f;
    // The generated frames' own fine detail — edges between neighbouring
    // blocks that took different vectors, a mosaic over motion-blurred or
    // low-contrast content — leans to its local average where neither input
    // has such detail within 16 pixels (14/10: 1440p recordings +0.07 /
    // +0.14 dB, flicker -0.18 / -0.39, grossly wrong pixels down; x4 game-like
    // scenes +0.03 to +0.21; nothing lower anywhere; ~1 ms a generated
    // frame at 1440p, ~0.3 ms more a game frame for the inputs' map).
    if (strong && !(autoOptionsSet & kAutoFalseDetail)) falseDetail = 1.f;
    // Blocks whose match is off-screen (content entering or leaving the
    // picture in a camera move) keep the predicted motion instead of a
    // match against the clamped edge (15/10: camera turns +0.15 to +0.19 dB
    // at x4, 720p and 1440p; PRAGMATA flicker -0.25 / -0.32, grossly wrong
    // pixels down; Cyberpunk and Oblivion within 0.02; worst -0.06 on an edge
    // scene; the corpus's menu popping up over noise -0.33, HUD -0.15; free —
    // the search is skipped).
    if (strong && !(autoOptionsSet & kAutoBorderPredict)) borderPredict = true;
    // The content entering (leaving) the picture in a camera turn from cur
    // (prev) alone, where the forward (backward) flow is still the camera's,
    // instead of the cross-fade its off-screen trajectories fell to (17/10:
    // x4 game-like scenes walk +0.47 dB, turn +0.13, first-person turn +0.11,
    // the others +0.01 to +0.03, flicker down on every one; 14 recorded games
    // -0.02 to +0.06, PRAGMATA's fast turns up and less flicker; one flow
    // lookup more, at the edges only).
    if (strong && !(autoOptionsSet & kAutoBorderFill)) borderFill = true;
    // With hud_mask: the scene between still HUD elements no longer copies
    // their colour through trajectories that run from overlay to overlay
    // (17/10: x4 game-like scenes walk +1.03 dB, strafe +0.61, turn +0.28,
    // dark +0.25, fps +0.18, their HUD icons 18-29 -> 26-36 dB; 8 recorded
    // games within 0.005; four mask lookups a pixel).
    if (strong && !(autoOptionsSet & kAutoHudAvoid)) hudAvoid = true;
    // While the world moves, the scene coming out from (going behind) the
    // edge of what holds still on screen — a first-person weapon, the HUD —
    // from the input it is visible in (21/10: x4 game-like scenes first
    // person +0.13 dB, the band along the weapon's edge no longer a smear;
    // walk +0.05, turn +0.02 (the HUD icons), the others within 0.001).
    if (strong && !(autoOptionsSet & kAutoStillOccluder)) stillOccluder = true;
    // Where the finest flow is chaos (a character turning on itself, an arm
    // or a leg in a fast turn: the synthesis assembles the picture from
    // blocks with unrelated motions), the generated frame leans to its local
    // average — a soft smear in place of a mosaic flickering from pair to
    // pair (21/10, the nine recordings of the night, preset quality: +0.045
    // dB at x2, +0.059 at x4, every sequence up, flicker -0.05 / -0.14;
    // false_detail kept: without it -0.02. The game-like scenes' sharp
    // self-moving edges lose up to 0.5 dB, measured 17/10: synthetic, not
    // what the games show. ~0.9 ms a game frame at 1440p x4.)
    if (strong && !(autoOptionsSet & kAutoMosaicBlur) && mosaicBlur == 0.f) mosaicBlur = 1.f;
    // HUD text over the moving scene, two layers (21/10): where static_soft's
    // evidence is dense, contrasted and lasting, the in-place cross-fade plus
    // the low frequencies of the synthesis's difference to it. Game-like
    // scenes at x4 (make_game.py, a 30 fps game at 120 Hz) +0.01 to +0.25
    // dB, +0.09 on average, moving edges +0.6, every scene up; recorded games
    // +0.010 / +0.006 at x2 / x4. ~0.36 ms a game frame at 1440p x4.
    if (strong && !(autoOptionsSet & kAutoHudLayer) && hudLayer == 0.f) hudLayer = 1.f;
    // No bonus for the zero-motion candidate (03/10, on top of everything
    // above, 960x540 corpus + demo): the free bonus won on soft content in
    // fast motion — a ghostly blend instead of the moving picture. The HUD
    // stays still without it (a static HUD matches exactly at zero; the
    // candidate is still searched). Interpolation x2: zoom +1.57 dB, pan
    // +0.45, swing +0.31, demo +0.08 / +0.26, fade +0.16; hybrid x3: zoom
    // +1.71, pan +1.51, rotation +0.82, popup +0.49, flash +0.26;
    // extrapolation x2: zoom +2.16, pan +1.57, flash +0.50, demo +0.26.
    // Losses: occlusions -0.11 (x2) / -0.30 (extrapolation), HUD -0.13
    // (hybrid), sub-pixel -0.06.
    if (strong && !(autoOptionsSet & kAutoZeroBias)) zeroBias = 0.f;
    if (strong && !(autoOptionsSet & kAutoInterpSplat)) {
        interpSplat = true;
        interpSplatAuto = true;
    }
    // The scene-cut fallback per region, each tile needing its own histogram
    // evidence (scene_cut_hist): a menu opening over part of the picture
    // +1.4 dB (flicker -6.3), elsewhere within +-0.02 dB. Kept off the lean
    // synthesis of smaller GPUs.
    if (strong && !(autoOptionsSet & kAutoLocalCut)) {
        sceneCutLocal = true;
        sceneCutLocalAuto = true;
    }
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
    I("multiplier", "mult", &Config::multiplier, 1, 8),
    I("levels", nullptr, &Config::levels, 1, 6),
    B("low_latency", "lowlatency", &Config::lowLatency),
    I("max_frame_latency", "frame_latency", &Config::maxFrameLatency, 0, 3, kLive),
    B("latency_limiter", nullptr, &Config::latencyLimiter, kLive),
    F("interp_hold", nullptr, &Config::interpHold, -1.f, 1.f, kLive),
    B("tight_hold", nullptr, &Config::tightHold, kLive),
    I("hybrid_predict", nullptr, &Config::hybridPredict, 0, 6),
    B("skip_fast", nullptr, &Config::skipFast, kLive),
    B("vrr_hold", nullptr, &Config::vrrHold, kLive),
    I("refresh_fill", nullptr, &Config::refreshFill, 0, 2),
    B("pace_plan", nullptr, &Config::pacePlan, kLive),
    B("phase_smooth", nullptr, &Config::phaseSmooth, kLive),
    B("even_motion", nullptr, &Config::evenMotion),
    I("even_fast", nullptr, &Config::evenFast, 0, 2, kLive),
    F("gpu_headroom", nullptr, &Config::gpuHeadroom, 0.f, 0.5f, kLive),
    B("catchup_smooth", nullptr, &Config::catchupSmooth, kLive),
    B("virtual_grid", nullptr, &Config::virtualGrid, kLive),
    B("x11_clock", nullptr, &Config::x11Clock),
    B("precise_wake", nullptr, &Config::preciseWake, kLive),
    B("thread_priority", nullptr, &Config::threadPriority),
    B("pipeline_cache", nullptr, &Config::pipelineCache),
    B("crash_log", nullptr, &Config::crashLog),
    B("flow_confidence", nullptr, &Config::flowConfidence),
    F("false_detail", nullptr, &Config::falseDetail, 0.f, 1.f, kLive),
    B("border_predict", nullptr, &Config::borderPredict, kLive),
    B("parallax_search", nullptr, &Config::parallaxSearch, kLive),
    F("learned_correction", nullptr, &Config::learnedCorrection, 0.f, 1.f, kLive),
    F("luma_match", nullptr, &Config::lumaMatch, 0.f, 0.5f, kLive),
    F("learned_flow", nullptr, &Config::learnedFlow, 0.f, 1.f, kLive),
    B("interp_splat", nullptr, &Config::interpSplat, kLive),
    B("sync_present", "sync", &Config::syncPresent),
    B("hide_present_ext", nullptr, &Config::hidePresentExt),
    B("ignore_anticheat", nullptr, &Config::ignoreAntiCheat),
    I("battery_saver", nullptr, &Config::batterySaver, 0, 2),
    I("search", "search_radius", &Config::searchRadius, 1, 4),
    I("search_fine", nullptr, &Config::searchFine, 1, 4),
    F("render_scale", nullptr, &Config::renderScale, 0.5f, 1.0f),
    F("sharpness", "sharpen", &Config::sharpness, 0.f, 1.f),
    B("pacing", nullptr, &Config::pacing),
    B("require_app", nullptr, &Config::requireApp),
    B("present_feedback", nullptr, &Config::presentFeedback),
    B("present_timing", nullptr, &Config::presentTiming),
    B("async_compute", nullptr, &Config::asyncCompute),
    B("present_queue", nullptr, &Config::presentQueue),
    B("synth_priority", nullptr, &Config::synthPriority),
    I("log", "log_level", &Config::logLevel, 0, 3),
    F("scene_cut_low", nullptr, &Config::sceneCutLow, 0.f, 1.f, kLive),
    F("scene_cut_high", nullptr, &Config::sceneCutHigh, 0.f, 1.f, kLive),
    B("scene_cut_local", nullptr, &Config::sceneCutLocal, kLive),
    F("scene_cut_hist", nullptr, &Config::sceneCutHist, 0.f, 1.f, kLive),
    F("smoothness", nullptr, &Config::smoothness, 0.f, 1.f, kLive),
    F("zero_bias", nullptr, &Config::zeroBias, 0.f, 1.f, kLive),
    F("zero_bias_contrast", nullptr, &Config::zeroBiasContrast, 0.f, 1.f, kLive, Tune::Always, "0.1", 1),
    F("zero_bias_cap", nullptr, &Config::zeroBiasCap, 0.f, 1.f, kLive),
    F("grad_weight", nullptr, &Config::gradWeight, 0.f, 8.f, kLive),
    F("census_weight", nullptr, &Config::censusWeight, 0.f, 8.f, kLive),
    F("census_margin", nullptr, &Config::censusMargin, 0.f, 1.f, kLive),
    F("rot_weight", nullptr, &Config::rotWeight, 0.f, 1.f, kLive),
    F("chroma_weight", nullptr, &Config::chromaWeight, 0.f, 8.f),
    F("occlusion_strength", nullptr, &Config::occlusionStrength, 0.f, 1.f, kLive, Tune::SynthInterpolation, "1", 10),
    B("global_motion", nullptr, &Config::globalMotion, kLive),
    F("static_soft", nullptr, &Config::staticSoft, 0.f, 1.f, kLive),
    B("temporal_prediction", nullptr, &Config::temporalPrediction, kLive, Tune::Always, "1", 2),
    B("extrapolate_splat", nullptr, &Config::extrapolateSplat, kLive, Tune::SynthExtrapolation, "1", 10),
    F("interp_accel", nullptr, &Config::interpAccel, 0.f, 1.f, kLive),
    F("extrapolate_accel", nullptr, &Config::extrapolateAccel, 0.f, 1.f, kLive, Tune::SynthExtrapolation, "1", 11),
    F("flow_temporal", nullptr, &Config::flowTemporal, 0.f, 1.f, kLive, Tune::Always, "1", 4),
    I("subpixel_lk", nullptr, &Config::subpixelLk, 0, 4, kLive, Tune::Extrapolation, "2", 5),
    B("synth_bicubic", nullptr, &Config::synthBicubic, kLive, Tune::SynthAlways, "1", 13),
    I("hud_mask", nullptr, &Config::hudMask, 0, 1000, kLive),
    B("reveal_fill", nullptr, &Config::revealFill, kLive, Tune::SynthExtrapolation, "1", 12),
    B("border_fill", nullptr, &Config::borderFill, kLive),
    B("still_occluder", nullptr, &Config::stillOccluder, kLive),
    B("hud_avoid", nullptr, &Config::hudAvoid, kLive),
    I("record_scale", nullptr, &Config::recordScale, 0, 4),
    F("mosaic_blur", nullptr, &Config::mosaicBlur, 0.f, 1.f, kLive),
    F("hud_layer", nullptr, &Config::hudLayer, 0.f, 1.f, kLive),
    B("online_tune", nullptr, &Config::onlineTune),
    F("upscale_temporal", nullptr, &Config::upscaleTemporal, 0.f, 0.95f),
    B("frame_timing", nullptr, &Config::frameTiming, kLive),
    B("content_clock", nullptr, &Config::contentClock, kLive),
    B("dedup", nullptr, &Config::dedup),
    B("paused", "pause", &Config::paused, kLive),
    B("adaptive", nullptr, &Config::adaptive, kLive),
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
    {"present_mode", nullptr}, {"debug", nullptr},     {"latency_log", nullptr},    {"gpu", nullptr},
    {"method", nullptr},       {"hotkey", nullptr}};

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

std::string Config::outOfBounds() const {
    for (const auto& d : registry()) {
        if (d.i && (this->*(d.i) < int(d.lo) || this->*(d.i) > int(d.hi))) return d.key;
        if (d.f && !(this->*(d.f) >= d.lo && this->*(d.f) <= d.hi)) return d.key;
    }
    return "";
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
    if (key == "interp_hold" && lower(value) == "auto") { interpHold = -1.f; return true; }  // (registry: -1)
    if (key == "pixel_vectors") autoOptionsSet |= kAutoPixelVectors;
    if (key == "extrapolate_splat") autoOptionsSet |= kAutoSplat;
    if (key == "reveal_fill") autoOptionsSet |= kAutoRevealFill;
    if (key == "scene_cut_local") autoOptionsSet |= kAutoLocalCut;
    if (key == "interp_splat") autoOptionsSet |= kAutoInterpSplat;
    if (key == "rot_weight") autoOptionsSet |= kAutoRot;
    if (key == "temporal_prediction") autoOptionsSet |= kAutoTemporalPrediction;
    if (key == "zero_bias") autoOptionsSet |= kAutoZeroBias;
    if (key == "global_motion") autoOptionsSet |= kAutoGlobalMotion;
    if (key == "hud_mask") autoOptionsSet |= kAutoHudMask;
    if (key == "static_soft") autoOptionsSet |= kAutoStaticSoft;
    if (key == "interp_accel") autoOptionsSet |= kAutoInterpAccel;
    if (key == "false_detail") autoOptionsSet |= kAutoFalseDetail;
    if (key == "border_predict") autoOptionsSet |= kAutoBorderPredict;
    if (key == "border_fill") autoOptionsSet |= kAutoBorderFill;
    if (key == "still_occluder") autoOptionsSet |= kAutoStillOccluder;
    if (key == "hud_avoid") autoOptionsSet |= kAutoHudAvoid;
    if (key == "mosaic_blur") autoOptionsSet |= kAutoMosaicBlur;
    if (key == "hud_layer") autoOptionsSet |= kAutoHudLayer;
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
        if (v == "interpolate" || v == "interp") extrapolate = hybrid = false;
        else if (v == "extrapolate" || v == "extrap" || v == "lowlatency") { extrapolate = true; hybrid = false; }
        else if (v == "hybrid") { extrapolate = false; hybrid = true; }
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
        if (v == "pacing" || v == "graph" || v == "frametime")              { overlayMode = 6; return true; }
        return parseInt(v, overlayMode, 0, 6);   // explicit level 0..6
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
    if (key == "gpu") {
        const std::string v = lower(value);
        gpu = v == "auto" || v == "game" ? std::string() : value;
        return true;
    }
    if (key == "method") {
        const std::string v = lower(value);
        if (v == "auto" || v == "0") method = 0;
        else if (v == "layer" || v == "injection" || v == "inject" || v == "1") method = 1;
        else if (v == "capture" || v == "2") method = 2;
        else return false;
        return true;
    }
    if (key == "hotkey")                      { hotkey = value; return true; }
    if (key == "latency_log")                 { latencyLog = platformExpandPath(value); return true; }
    // "auto" (-1): from the refresh rate the present feedback measures.
    // "auto", or any negative number (the UIs' spin buttons write -1 for it).
    if (key == "target_fps") {
        if (lower(value) == "auto" || (parseFloat(value, targetFps, -1000.f, 1000.f) && targetFps < 0.f)) {
            targetFps = -1.f;
            return true;
        }
        return parseFloat(value, targetFps, 0.f, 1000.f);
    }
    if (key == "fps_limit" || key == "fps_cap") {
        if (lower(value) == "auto" || (parseFloat(value, fpsLimit, -1000.f, 1000.f) && fpsLimit < 0.f)) {
            fpsLimit = -1.f;
            return true;
        }
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
        else if (v == "confidence" || v == "conf") debug = Debug::Confidence;
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
    if (in) loadStream(in, path, processName, bundled);
}

void Config::loadStream(std::istream& in, const std::string& path, const std::string& processName, bool bundled) {
    std::vector<std::string> names = processNames();
    if (!processName.empty()) names.push_back(baseName(processName));
    std::string line;
    int lineNo = 0;
    bool active = !bundled;  // lines before any section are global
    bool named = false;      // ... and this one names the process
    while (std::getline(in, line)) {
        ++lineNo;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            const std::string section = trim(t.substr(1, t.size() - 2));
            // Shipped profiles only tune named games; they never apply globally.
            active = !(bundled && section == "*") && sectionMatches(section, names);
            named = active && !baseName(section).empty() && baseName(section) != "*";
            continue;
        }
        if (!active) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(t.substr(0, eq)));
        if (bundled) {
            // Turning the layer on (or pausing it) stays the user's decision.
            if (key == "enabled" || key == "enable" || key == "paused" || key == "pause") continue;
        } else if (named && (key == "enabled" || key == "enable" || key == "require_app")) {
            processNamed = true;
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
        if (enabled) {
            requireApp = false;  // explicit per-game launch (Steam option, CLI)
            processNamed = true;
        }
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
    pixelVectorsAuto = n.pixelVectorsAuto;
    interpSplatAuto = n.interpSplatAuto;
    sceneCutLocalAuto = n.sceneCutLocalAuto;
    if (gpuBudget > 0.f && n.gpuBudget > 0.f) gpuBudget = n.gpuBudget;  // 0 <-> >0 creates the timestamp queries
    // (dedup sizes the output stage in extrapolation: restart.)
    if (sharpness > 0.f && n.sharpness > 0.f) sharpness = n.sharpness;  // 0 <-> >0 changes the passes
    if (chromaWeight > 0.f && n.chromaWeight > 0.f) chromaWeight = n.chromaWeight;  // likewise (chroma pyramid)
}

Config Config::keepingCreationSettings(Config n) const {
    n.enabled = enabled;
    n.antiCheatDisabled = antiCheatDisabled;
    n.antiCheatIgnored = antiCheatIgnored;
    n.ignoreAntiCheat = ignoreAntiCheat;
    n.requireApp = requireApp;
    n.renderScale = renderScale;
    n.presentMode = presentMode;
    n.hidePresentExt = hidePresentExt;
    n.asyncCompute = asyncCompute;
    n.synthPriority = synthPriority;
    n.presentFeedback = presentFeedback;
    n.presentTiming = presentTiming;
    n.syncPresent = syncPresent;
    n.onlineTune = onlineTune;  // DeviceData's OnlineTune, created with the device
    n.tuneName = tuneName;
    n.captureHdr = captureHdr;
    n.logLevel = logLevel;
    n.logFile = logFile;
    n.dumpDir = dumpDir;
    n.dumpFrames = dumpFrames;
    if (!(gpuBudget > 0.f && n.gpuBudget > 0.f)) n.gpuBudget = gpuBudget;  // as applyLive
    n.rebuildGen = rebuildGen;
    return n;
}

bool Config::sameOutputSettings(const Config& o) const {
    for (const auto& d : registry()) {
        if (d.b && this->*(d.b) != o.*(d.b)) return false;
        if (d.i && this->*(d.i) != o.*(d.i)) return false;
        if (d.f && this->*(d.f) != o.*(d.f)) return false;
    }
    return extrapolate == o.extrapolate && hybrid == o.hybrid && flowScale == o.flowScale && upscaleFilter == o.upscaleFilter &&
           overlayMode == o.overlayMode && debug == o.debug;
}

Config Config::loadWithDefaults(Config c, const std::string& extraProcess) {
    const std::string profiles = platformBundledProfilesPath();
    if (!profiles.empty()) c.loadFile(profiles, extraProcess, true);
    const std::string path = filePath();
    if (!path.empty()) c.loadFile(path, extraProcess);
    c.loadEnv();
    // battery_saver: the power state now (pollConfig reloads when it changes).
    c.onBattery = c.batterySaver > 0 && !platformIsSteamDeck() && platformOnBattery() == 1;
    if (c.onBattery && c.batterySaver >= 2 && c.multiplier > 2) {
        BDEX_INFO("on battery: multiplier x%d capped at x2 (battery_saver = 2)", c.multiplier);
        c.multiplier = 2;
    }
    // Multiplayer anti-cheat present: stay a pass-through for this process,
    // whatever the config/env says. Injection into a protected process is
    // what anti-cheats flag, so the safe side wins (see platform.cpp).
    // ignore_anticheat=1 (opt-in, the player's own risk) keeps the layer on.
    // platformAntiCheatException: a game the user chose to keep injecting
    // into (Rocket League on Windows), the same warning.
    std::string antiCheat;
    bool exception = false;
    for (const auto& n : processNames()) exception = exception || platformAntiCheatException(n);
    if (c.ignoreAntiCheat || exception) {
        if (platformAntiCheatPresent(&antiCheat)) {
            c.antiCheatIgnored = antiCheat;
            BDEX_WARN("anti-cheat '%s' detected but %s: layer stays on (ban risk: the player's own decision)",
                      antiCheat.c_str(), exception ? "injection kept for this game" : "ignore_anticheat=1");
        }
    } else if (platformAntiCheatPresent(&antiCheat)) {
        c.enabled = false;
        c.antiCheatDisabled = true;
        BDEX_WARN("anti-cheat '%s' detected: layer disabled for this process", antiCheat.c_str());
    }
    // require_app (the control panel, like the Windows launcher): games are
    // armed — virtual swapchain, real frames passed through — and generate
    // once « Démarrer » targets this process. Upscaling changes what the game
    // renders, so it only applies to games started while the panel is open.
    if (c.enabled && c.requireApp) {
        bool nonGame = platformIsDesktopApp() || platformIsNonGame(platformExeName());
        for (const auto& n : processNames()) nonGame = nonGame || platformIsNonGame(n);
        if (nonGame) {
            c.enabled = false;
        } else {
            const AppSession s = platformAppSession();
            if (!s.open) c.renderScale = 1.0f;
            c.panelDisplayHz = s.displayHz;
            c.appTargeted = s.target != 0 && s.target == platformPid();
            if (!c.appTargeted) {
                c.paused = true;
                c.overlayMode = 0;
            }
        }
    }
    // On for every program without the panel — [*] enabled with require_app =
    // 0, the panel's « sans le panneau » for all games (Steam's Game Mode,
    // where it cannot stay open beside a game): the desktop and Steam's own
    // processes stay untouched, as under require_app — gamescope above all,
    // Game Mode's compositor. Their own [section] or BDEX_FG=1 still counts.
    if (c.enabled && !c.requireApp && !c.processNamed) {
        bool nonGame = platformIsDesktopApp() || platformIsNonGame(platformExeName());
        for (const auto& n : processNames()) nonGame = nonGame || platformIsNonGame(n);
        if (nonGame) c.enabled = false;
    }
    if (c.sceneCutHigh < c.sceneCutLow) c.sceneCutHigh = c.sceneCutLow;
    if (!extraProcess.empty()) c.tuneName = baseName(extraProcess);
    else if (const auto names = processNames(); !names.empty()) c.tuneName = names.front();
    return c;
}

int Config::flowScaleFor(uint32_t width, uint32_t height) const {
    if (flowScale > 0) return flowScale;
    const uint64_t pixels = uint64_t(width) * height;
    if (flowScale < 0) return pixels <= 2600ull * 1000 ? 1 : 2;  // the quality preset: one step finer
    if (pixels <= 640ull * 1000) return 1;   // up to ~1024x600: full resolution is cheap
    if (pixels <= 2600ull * 1000) return 2;  // up to ~1080p/1440p-ish
    return 4;                                // 4K and beyond
}

std::string Config::describe() const {
    static const char* const dbg[] = {"none", "flow", "split", "passthrough", "confidence"};
    static_assert(sizeof(dbg) / sizeof(dbg[0]) == size_t(Debug::Confidence) + 1, "a name per Debug value");
    std::ostringstream o;
    o << "enabled=" << enabled << " multiplier=" << multiplier << " levels=" << levels
      << " mode=" << modeName() << " flow_scale=" << (flowScale > 0 ? std::to_string(flowScale) : flowScale < 0 ? "fine" : "auto") << " search=" << searchRadius << "/" << searchFine << " refine=" << refine
      << " present_mode=" << presentModeName(presentMode) << " pacing=" << pacing
      << " debug=" << dbg[static_cast<int>(debug)] << " log=" << logLevel;
    if (gradWeight > 0.f) o << " grad_weight=" << gradWeight;
    if (censusWeight > 0.f) o << " census_weight=" << censusWeight << " census_margin=" << censusMargin;
    if (rotWeight > 0.f) o << " rot_weight=" << rotWeight;
    if (chromaWeight > 0.f) o << " chroma_weight=" << chromaWeight;
    if (occlusionStrength > 0.f) o << " occlusion_strength=" << occlusionStrength;
    if (zeroBiasContrast > 0.f) o << " zero_bias_contrast=" << zeroBiasContrast;
    if (temporalPrediction) o << " temporal_prediction=1";
    if (globalMotion) o << " global_motion=1";
    if (extrapolateSplat) o << " extrapolate_splat=1";
    if (extrapolateAccel > 0.f) o << " extrapolate_accel=" << extrapolateAccel;
    if (flowTemporal > 0.f) o << " flow_temporal=" << flowTemporal;
    if (subpixelLk > 0) o << " subpixel_lk=" << subpixelLk;
    if (synthBicubic) o << " synth_bicubic=1";
    if (hudMask > 0) o << " hud_mask=" << hudMask;
    if (falseDetail > 0.f) o << " false_detail=" << falseDetail;
    if (mosaicBlur > 0.f) o << " mosaic_blur=" << mosaicBlur;
    if (hudLayer > 0.f) o << " hud_layer=" << hudLayer;
    if (borderPredict) o << " border_predict=1";
    if (parallaxSearch) o << " parallax_search=1";
    if (revealFill) o << " reveal_fill=1";
    if (borderFill) o << " border_fill=1";
    if (stillOccluder) o << " still_occluder=1";
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
    if (onBattery) o << " [on battery: battery_saver=" << batterySaver << "]";
    return o.str();
}

} // namespace bdex
