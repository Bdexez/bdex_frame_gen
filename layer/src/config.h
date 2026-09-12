#pragma once
#include <vulkan/vulkan.h>

#include <string>

namespace bdex {

// Runtime configuration of the layer. Values come from (lowest to highest
// priority): built-in defaults, ~/.config/bdex-framegen.conf, environment
// variables named BDEX_FG_<KEY>.
struct Config {
    enum class Debug { None, Flow, Split, Passthrough };

    bool  enabled       = true;   // false: layer stays loaded but does nothing
    int   multiplier    = 2;      // generated frames per real frame: 2 = x2, 3 = x3, 4 = x4
    int   levels        = 4;      // optical flow pyramid levels
    bool  fullres       = false;  // finest flow level at full resolution (default: half)
    int   searchRadius  = 4;      // block matching search radius at the coarsest level, 1..4
    int   searchFine    = 2;      // search radius at the finer levels (around the coarse prediction), 1..4
    int   presentMode   = -1;     // VkPresentModeKHR override, -1 = keep the game's
    bool  pacing        = true;   // sleep-based pacing when the present mode is not FIFO
    Debug debug         = Debug::None;
    int   logLevel      = 1;
    std::string logFile;
    float sceneCutLow   = 0.10f;  // mean matching cost above which we start fading to the real frame
    float sceneCutHigh  = 0.22f;  // ... and above which the generated frame is a plain duplicate
    float smoothness    = 0.004f; // penalty per pixel of deviation from the coarse prediction
    float zeroBias      = 0.002f; // bias toward zero motion (helps static HUDs)
    bool  refine        = true;   // flow refinement pass (better motion boundaries, costs GPU time)
    float refineBias    = 0.5f;   // SAD bonus (sum over 64 px) for keeping a block's own flow in the refinement
    bool  stats         = true;   // print FPS statistics periodically
    bool  profile       = false;  // measure GPU time per pass (printed with the statistics)
    float statsInterval = 5.0f;   // seconds
    std::string dumpDir;          // when set, presented frames are written there as PPM files
    int   dumpFrames    = 24;     // how many presented frames to dump

    // Apply a single key=value pair (keys are case-insensitive, without the
    // BDEX_FG_ prefix). Returns false for unknown keys or unparsable values.
    bool apply(const std::string& key, const std::string& value);

    void loadFile(const std::string& path);
    void loadEnv();

    static Config load();  // defaults + file + env
    std::string describe() const;
};

const char* presentModeName(int mode);

} // namespace bdex
