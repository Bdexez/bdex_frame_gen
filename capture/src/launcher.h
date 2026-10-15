// bdex_capture's native launcher (docs/windows-capture.md phase 3): running
// bdex_capture without arguments opens this window instead of capturing the
// foreground one. Pick the game window, set the frame-generation options —
// the same ones as the Linux control panel (tools/bdex-framegen-gui) — and
// press Démarrer; the choices persist to the shared config file.
#pragma once
#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace bdex {

// A capturable top-level window (what --list prints).
struct WindowInfo {
    HWND hwnd;
    DWORD pid;
    std::wstring title;
};

// Visible, titled, un-cloaked top-level windows — what WGC can capture and
// what a user would recognise in the launcher list.
std::vector<WindowInfo> capturableWindows();

// UTF-8 for the byte-oriented log/console (main sets CP_UTF8).
std::string narrow(const std::wstring& w);

std::wstring wideLower(std::wstring s);

// The interfaces' language: French when Windows' display language is
// (GetUserDefaultUILanguage), English otherwise; BDEX_LANG=fr|en chooses.
bool launcherFrench();

// Base name of a process image (…\DarkSoulsIII.exe -> DarkSoulsIII.exe).
std::wstring processName(DWORD pid);

// One capture run as chosen in the launcher.
struct LaunchChoice {
    bool ok = false;        // false: the window was closed (quit)
    HWND target = nullptr;  // game window to capture
    int multiplier = 2;     // frames shown per captured frame: 1 (off), 2..4
    bool extrapolate = true;
    bool hybrid = false;    // mode=hybrid (frame_order.h): x3/x4 with less latency than interpolation
    bool adaptive = false;  // adaptive multiplier: the monitor's whole refresh rate, fractional ratios included (up to x4)
    std::string preset;     // "quality" | "balanced" | "performance"; "" = auto
    int filter = 2;         // 0 bilinear, 1 bicubic, 2 Lanczos-2
    float sharpness = 0.f;  // 0..1
    int hud = 0;            // on-screen fps HUD level, 0..4
    bool fifo = false;      // vsync (FIFO) presentation instead of mailbox
    float scale = 1.f;      // overlay size factor (unused when fit)
    bool fit = false;       // overlay fills the game's monitor
    std::string gpu;        // the GPU that generates ("" = the best: the game's), a part of its name
    int method = 0;         // 0 auto (injection where the Vulkan layer has the game, else capture), 1 injection, 2 capture
    bool perGame = false;   // save these options to the target's [game] section
                            // (a per-game profile) instead of the global [*]
    // Advanced optical-flow tuning (defaults mirror Config; a value left at its
    // default is not written to the file). Same keys the Linux panel exposes.
    float gradWeight = 0.f;      // grad_weight
    float smoothness = 0.004f;   // smoothness
    float zeroBias = 0.002f;     // zero_bias
    int   searchFine = 2;        // search_fine
    int   flowIterations = 1;    // flow_iterations
    float sceneCutLow = 0.05f;   // scene_cut_low
    float sceneCutHigh = 0.09f;  // scene_cut_high
    // The other engine options shown in the launcher's "Options avancées"
    // column, by config key (kExtra in launcher.cpp). Values are config
    // strings; an absent key means the default. Persisted to the config file
    // like the rest, which the capture then loads.
    std::map<std::string, std::string> extra;
};

// Runs the launcher modally (its own message loop). `choice` carries both
// the defaults shown and the result; on Démarrer the options are persisted
// to the config file's [*] section (game sections and unknown keys are
// preserved). Returns choice.ok.
bool runLauncher(LaunchChoice& choice);

// The Vulkan GPUs of this machine (software renderers left out), by name.
std::vector<std::string> vulkanGpuNames();
// The Vulkan layer (VkLayer_bdex_framegen.dll) is registered as an implicit
// layer (register-layer.ps1): Vulkan games load it, and the launcher can
// generate in them by injection.
bool layerRegistered();
// With the layer registered: Vulkan games armed (enabled = 1, require_app =
// 1 in [*], unless the file already sets them) — they pass their frames
// through until the launcher targets one.
void armLayer();

// Value of a key in the config file's global ([*] / preamble) section, or ""
// when unset. Lets the launcher prefill from a hand-written config.
std::string globalConfigValue(const std::string& key);

} // namespace bdex
