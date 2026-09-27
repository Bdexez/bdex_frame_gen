#pragma once
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace bdex {

// Runtime configuration of the layer. Values come from (lowest to highest
// priority): built-in defaults, ~/.config/bdex-framegen.conf, environment
// variables named BDEX_FG_<KEY>.
struct Config {
    enum class Debug { None, Flow, Split, Passthrough };

    // Resolution divisor of the finest flow level for a given swapchain
    // size: keeps that level around 0.5 Mpixel (half resolution at 1080p,
    // quarter at 4K).
    int flowScaleFor(uint32_t width, uint32_t height) const;

    bool  enabled       = false;  // the layer is now always loaded (no enable_environment);
                                  // it stays a pure pass-through until turned on by the
                                  // config file ([*] or a [game] section) or BDEX_FG=1.
    bool  antiCheatDisabled = false;  // forced off by platformAntiCheatPresent() (multiplayer
                                      // anti-cheat running); wins over enabled, file and env.
    bool  autoTune      = true;   // pick the quality preset from the detected GPU (until the user sets a preset or a flow option)
    std::string autoTunedTo;      // preset autoConfigure() selected, for the log/stats
    int   multiplier    = 2;      // generated frames per real frame: 2 = x2, 3 = x3, 4 = x4
    int   levels        = 4;      // optical flow pyramid levels
    bool  lowLatency    = false;  // block the game until the previous frames are handed to the display
    bool  syncPresent   = false;  // block the game's present call until the worker has presented this frame (D3D12/VKD3D compat test)
    bool  hidePresentExt = true;  // hide present-timing extensions (swapchain_maintenance1, present_id/wait) from the game; their semantics target the real swapchain we virtualise, and VKD3D-Proton hangs otherwise
    bool  extrapolate   = false;  // predict the frame after the last one instead of interpolating (no added latency)
    int   flowScale     = 0;      // finest flow level at 1/flowScale resolution: 1, 2, 4; 0 = auto from the resolution
    int   searchRadius  = 4;      // block matching search radius at the coarsest level, 1..4
    int   searchFine    = 2;      // search radius at the finer levels (around the coarse prediction), 1..4
    // Spatial upscaling: the game is told a swapchain of renderScale x the
    // display size, so it renders fewer pixels; every frame (real and
    // generated) is upscaled to the display resolution. 1.0 disables it.
    float renderScale   = 1.0f;   // render resolution / display resolution, 0.5..1.0 (1.0 = no upscaling)
    int   upscaleFilter = 2;      // 0 bilinear, 1 Catmull-Rom (bicubic), 2 Lanczos-2
    float sharpness     = 0.0f;   // contrast-adaptive sharpening after upscaling, 0 = off .. 1 = strong
    int   overlayMode   = 0;      // on-screen fps HUD: 0 off, 1 fps, 2 fps+framegen, 3 +1% low, 4 +0.1% low
    int   presentMode   = -2;     // VkPresentModeKHR override; -1 = keep the game's; -2 = auto (fifo -> mailbox)
    int   captureHdr    = -1;     // capture app: -1 auto (HDR when the game's display is in HDR mode), 0 SDR, 1 HDR if possible
    bool  presentFeedback = true; // measure when frames are really displayed (VK_KHR_present_wait, where supported): refresh rate, drops, vblank-aligned pacing
    bool  pacing        = true;   // sleep-based pacing when the present mode is not FIFO
    Debug debug         = Debug::None;
    int   logLevel      = 1;
    std::string logFile;
    float sceneCutLow   = 0.05f;  // mean matching cost above which we start fading to the real frame
    float sceneCutHigh  = 0.09f;  // ... and above which the generated frame is a plain duplicate
    bool  sceneCutLocal = false;  // scene-cut test per region (8x8 grid) instead of on the frame mean (opt-in)
    float smoothness    = 0.004f; // penalty per pixel of deviation from the coarse prediction
    float zeroBias      = 0.002f; // bias toward zero motion (helps static HUDs)
    float zeroBiasContrast = 0.0f; // mean luma gradient at which zero_bias fully applies; lower-contrast blocks get proportionally less (0 = absolute bias, bit-for-bit)
    float gradWeight    = 0.0f;   // weight of the gradient-constancy matching term (0 = plain luma SAD; ~1 sharpens fast motion at some cost to static fidelity)
    float censusWeight  = 0.0f;   // weight of the census re-ranking term: when the block matcher is ambiguous (runner-up within census_margin of the winner), near-winners are re-ranked on census Hamming distance (0 = off, bit-for-bit)
    float censusMargin  = 0.05f;  // relative cost margin that makes a match ambiguous (triggers the census re-ranking)
    float chromaWeight  = 0.0f;   // chroma re-ranking of the block matcher: candidates luma cannot tell apart are re-ranked on Cb/Cr SAD (0 = off, bit-for-bit; enabling it builds a chroma pyramid, read at start-up)
    float rotWeight     = 0.0f;   // rotation/scale re-ranking of ambiguous winners: the top-2 displacements are re-evaluated under local rotations and scales (0 = off, bit-for-bit; 1 = on)
    float occlusionStrength = 0.0f; // opt-in flow confidence and reduced cross-fade fallback (0 = historical synthesis)
    bool  temporalPrediction = false; // coarsest matching level searches around the previous frame pair's motion instead of zero, so sustained fast pans are followed beyond the pyramid's range (opt-in; off = bit-for-bit)
    bool  extrapolateSplat = false; // extrapolation: forward-splat the current frame, backward warp only where nothing lands (opt-in)
    float extrapolateAccel = 0.0f; // extrapolation: weight of a second-order (acceleration) term from the previous pair's motion (0 = constant velocity, bit-for-bit)
    float extrapolateHold = 0.0f; // extrapolation: keep pixels with inconsistent motion in place (1 = pre-0.4 behaviour, shows double images; 0 = follow the warp)
    bool  frameTiming   = false;  // t of each generated frame from the real content times (frame-time spikes) instead of the fixed i/multiplier (opt-in)
    bool  dedup         = false;  // detect frames the game presents twice: keep the previous motion, time the output on first appearances (opt-in)
    bool  paused        = false;  // live toggle: present only the real frames (reloaded while running; see tools/bdex-framegen --toggle)
    float targetFps     = 0.0f;   // dynamic multiplier: aim at this output rate (the refresh rate), up to `multiplier` (0 = fixed multiplier, -1 = auto: the measured refresh rate)
    float gpuBudget     = 0.0f;   // share of the game's frame interval the generation may take on the GPU; above it the quality steps down (0 = off)
    float fpsLimit      = 0.0f;   // cap the game's frame rate (0 = off; -1 = auto: measured refresh / multiplier); pick a divisor of the refresh rate
    bool  pixelVectors  = false;  // interpolation: per-pixel choice among neighbouring blocks' vectors (opt-in)
    bool  subpixel      = false;  // parabolic sub-pixel fit of the finest flow (opt-in; off = bit-for-bit)
    float staticGuard   = 0.0f;   // opt-in per-pixel static guard: pixels identical in both frames (3x3) keep the current frame, so static HUD/UI over moving content does not smear with the block's motion (0 = off, bit-for-bit)
    bool  refine        = true;   // flow refinement pass (better motion boundaries, costs GPU time)
    bool  refineAll     = true;   // refine every pyramid level (off: only the finest, cheaper, ~2 dB worse)
    int   flowIterations = 1;     // fixed-point iterations of the flow lookup in the interpolation (0-3)
    float refineBias    = 0.5f;   // SAD bonus (sum over 64 px) for keeping a block's own flow in the refinement
    bool  stats         = true;   // print FPS statistics periodically
    bool  profile       = false;  // measure GPU time per pass (printed with the statistics)
    bool  sharedQueue   = false;  // force sharing a queue with the application (testing)
    float statsInterval = 5.0f;   // seconds
    std::string dumpDir;          // when set, presented frames are written there as PPM files
    int   dumpFrames    = 24;     // how many presented frames to dump

    // Apply a single key=value pair (keys are case-insensitive, without the
    // BDEX_FG_ prefix). Returns false for unknown keys or unparsable values.
    bool apply(const std::string& key, const std::string& value);

    // Reads `clé = valeur` lines. A `[name]` section restricts the options
    // that follow to processes whose command line contains an executable
    // called `name` (case-insensitive, `.exe` optional); `[*]` or no section
    // applies to every process. Sections are applied in file order, so a
    // game section placed after the global options overrides them.
    // `bundled`: a shipped profiles file — only named-game sections, never
    // `enabled` / `paused` (see platformBundledProfilesPath).
    void loadFile(const std::string& path, const std::string& processName, bool bundled = false);
    void loadEnv();

    // Named presets applied before the other options: "quality",
    // "balanced" (the defaults), "performance", "latency" (extrapolation for
    // minimal input lag) and "auto" (choose from the GPU, see autoConfigure).
    bool applyPreset(const std::string& name);

    // When autoTune is still set (no preset or flow option chosen explicitly),
    // pick the quality preset from the GPU: integrated / software renderers get
    // "performance", discrete GPUs with at least 2 GiB of VRAM "balanced".
    void autoConfigure(const VkPhysicalDeviceProperties& props, const VkPhysicalDeviceMemoryProperties& mem);

    // defaults + file + env. `extraProcess` (an executable name, `.exe`
    // optional) is added to the process names that `[section]` headers match,
    // so a caller that runs on behalf of another process — bdex_capture, which
    // targets a game window — gets that game's `[game]` section applied even
    // though it is not the current process. Empty (the default) = current
    // process only, the layer's case.
    static Config load(const std::string& extraProcess = "");
    static std::string filePath();  // BDEX_FG_CONFIG, else the platform's config file
    // Copies the settings that can change while a game runs (see config.cpp).
    void applyLive(const Config& fresh);
    // Same, starting from product defaults instead of Config{} (the capture
    // app defaults to extrapolation, still overridable by the file and env).
    static Config loadWithDefaults(Config defaults, const std::string& extraProcess = "");

    // Executable names of the current process (platform-specific source),
    // lower-case, without directory or .exe suffix; used for section matching.
    static std::vector<std::string> processNames();
    static bool sectionMatches(const std::string& section, const std::vector<std::string>& names);
    std::string describe() const;
};

const char* presentModeName(int mode);

} // namespace bdex
