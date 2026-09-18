// bdex_capture's native launcher (docs/windows-capture.md phase 3): running
// bdex_capture without arguments opens this window instead of capturing the
// foreground one. Pick the game window, set the frame-generation options —
// the same ones as the Linux control panel (tools/bdex-framegen-gui) — and
// press Démarrer; the choices persist to the shared config file.
#pragma once
#include <windows.h>

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

// Base name of a process image (…\DarkSoulsIII.exe -> DarkSoulsIII.exe).
std::wstring processName(DWORD pid);

// One capture run as chosen in the launcher.
struct LaunchChoice {
    bool ok = false;        // false: the window was closed (quit)
    HWND target = nullptr;  // game window to capture
    int multiplier = 2;     // frames shown per captured frame: 1 (off), 2..4
    bool extrapolate = true;
    std::string preset;     // "quality" | "balanced" | "performance"; "" = auto
    int filter = 2;         // 0 bilinear, 1 bicubic, 2 Lanczos-2
    float sharpness = 0.f;  // 0..1
    int hud = 0;            // on-screen fps HUD level, 0..4
    bool fifo = false;      // vsync (FIFO) presentation instead of mailbox
    float scale = 1.f;      // overlay size factor (unused when fit)
    bool fit = false;       // overlay fills the game's monitor
};

// Runs the launcher modally (its own message loop). `choice` carries both
// the defaults shown and the result; on Démarrer the options are persisted
// to the config file's [*] section (game sections and unknown keys are
// preserved). Returns choice.ok.
bool runLauncher(LaunchChoice& choice);

// Value of a key in the config file's global ([*] / preamble) section, or ""
// when unset. Lets the launcher prefill from a hand-written config.
std::string globalConfigValue(const std::string& key);

} // namespace bdex
