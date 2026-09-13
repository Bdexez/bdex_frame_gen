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

    bool  enabled       = true;   // false: layer stays loaded but does nothing
    bool  autoTune      = true;   // pick the quality preset from the detected GPU (until the user sets a preset or a flow option)
    std::string autoTunedTo;      // preset autoConfigure() selected, for the log/stats
    int   multiplier    = 2;      // generated frames per real frame: 2 = x2, 3 = x3, 4 = x4
    int   levels        = 4;      // optical flow pyramid levels
    bool  lowLatency    = false;  // block the game until the previous frames are handed to the display
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
    bool  overlay       = false;  // draw an on-screen fps HUD (game fps -> output fps) on every frame
    int   presentMode   = -2;     // VkPresentModeKHR override; -1 = keep the game's; -2 = auto (fifo -> mailbox)
    bool  pacing        = true;   // sleep-based pacing when the present mode is not FIFO
    Debug debug         = Debug::None;
    int   logLevel      = 1;
    std::string logFile;
    float sceneCutLow   = 0.05f;  // mean matching cost above which we start fading to the real frame
    float sceneCutHigh  = 0.09f;  // ... and above which the generated frame is a plain duplicate
    float smoothness    = 0.004f; // penalty per pixel of deviation from the coarse prediction
    float zeroBias      = 0.002f; // bias toward zero motion (helps static HUDs)
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
    void loadFile(const std::string& path, const std::string& processName);
    void loadEnv();

    // Named presets applied before the other options: "quality",
    // "balanced" (the defaults), "performance", "latency" (extrapolation for
    // minimal input lag) and "auto" (choose from the GPU, see autoConfigure).
    bool applyPreset(const std::string& name);

    // When autoTune is still set (no preset or flow option chosen explicitly),
    // pick the quality preset from the GPU: integrated / software renderers get
    // "performance", discrete GPUs "balanced", large discrete GPUs "quality".
    void autoConfigure(const VkPhysicalDeviceProperties& props, const VkPhysicalDeviceMemoryProperties& mem);

    static Config load();  // defaults + file + env

    // Executable names of the current process (from /proc/self/cmdline),
    // lower-case, without directory or .exe suffix; used for section matching.
    static std::vector<std::string> processNames();
    static bool sectionMatches(const std::string& section, const std::vector<std::string>& names);
    std::string describe() const;
};

const char* presentModeName(int mode);

} // namespace bdex
