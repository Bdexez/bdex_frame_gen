// bdex_capture — the Windows capture product (docs/windows-capture.md):
// capture one game window with Windows.Graphics.Capture, share the frames
// into Vulkan, run the layer's frame-generation engine on them (optical flow,
// interpolation / extrapolation, upscaling, HUD) and present real + generated
// frames in a borderless always-on-top overlay placed over (or instead of)
// the game window. Console app: logs go to stderr.
//
//   bdex_capture [--title <substr> | --pid <n>] [--scale <f> | --fit]
//                [--multiplier 1..8] [--adaptive]
//                [--mode extrapolate|interpolate|hybrid]
//                [--preset <name>] [--filter 0|1|2] [--sharpness <f>]
//                [--hud 0..4] [--gpu <n>] [--fifo] [--list]
//
// With no arguments a launcher window opens instead: pick the game window,
// set the same options as the Linux control panel, press Démarrer. With
// --title/--pid only, the foreground window 5 s after launch is captured
// (alt-tab into the game). Ctrl+Alt+Q or closing the game window quits.
// Everything else comes from %APPDATA%\bdex-framegen\bdex-framegen.conf and
// the BDEX_FG_* environment, like the layer.
#include "capture.h"
#include "config.h"
#include "injection.h"
#include "launcher.h"
#include "log.h"
#include "platform.h"
#include "status.h"
#include "tray.h"
#include "vkctx.h"

#include <windows.h>
#include <dwmapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

// Precise waits for the pacing. Sleep() and WaitForMultipleObjects() tick at
// the process's timer resolution — 15.6 ms by default since Windows 10 2004,
// whatever the game asks for itself — so a 16.7 ms wait (x2 at 30 fps) ended
// at 31 ms: the generated frame reached the screen next to the real one (or
// was dropped in extrapolation) and the real frame came up to a period late.
// main() raises the resolution to 1 ms; the waits below sleep until ~1.5 ms
// before the deadline, then spin on the clock like limitFrameRate() does.
using Clock = bdex::VkCtx::clock;
constexpr auto kSpin = std::chrono::microseconds(1500);

DWORD coarseMs(Clock::time_point deadline) {
    const auto left = deadline - kSpin - Clock::now();
    return left.count() > 0 ? static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(left).count())
                            : 0;
}

void sleepUntil(Clock::time_point deadline) {
    if (const DWORD ms = coarseMs(deadline)) Sleep(ms);
    while (Clock::now() < deadline) SwitchToThread();
}

// Same, but returns true as soon as a captured frame is pending (the
// predicted frames it would have been shown before are obsolete).
bool frameBefore(bdex::Capture& cap, Clock::time_point deadline) {
    if (cap.waitPending(coarseMs(deadline))) return true;
    while (Clock::now() < deadline) {
        if (cap.waitPending(0)) return true;
        SwitchToThread();
    }
    return false;
}

struct Options {
    std::wstring title;
    DWORD pid = 0;
    float scale = 1.0f;
    bool fit = false;
    int gpu = -1;
    std::string gpuName;    // --gpu <part of a name> (else the config key gpu)
    int method = -1;        // --method: 0 auto, 1 layer (injection), 2 capture; -1 = the config's
    bool fifo = false;
    bool list = false;
    // Frame-generation overrides (-1 / empty = keep the config file's value).
    int multiplier = -1;
    int mode = -1;          // 0 interpolate, 1 extrapolate, 2 hybrid
    bool adaptive = false;
    std::string preset;
    int filter = -1;
    float sharpness = -1.f;
    int hud = -1;
};

void usage() {
    std::puts(
        "bdex_capture — capture a game window, generate frames in Vulkan, show it in an overlay\n"
        "\n"
        "  --title <substr>   target window whose title contains <substr> (case-insensitive)\n"
        "  --pid <n>          target the main window of process <n>\n"
        "  (neither)          the foreground window 5 seconds after launch\n"
        "  --scale <f>        overlay size = capture size x f (default 1.0)\n"
        "  --fit              overlay fills the game's monitor, aspect preserved\n"
        "  --multiplier <n>   frames shown per captured frame: 1 (off), 2 to 8 (default 2)\n"
        "  --adaptive         fill the monitor's refresh rate: as many frames per captured frame as fit,\n"
        "                     the fraction carried over; ceiling --multiplier, else 4, 6 or 8 by refresh rate\n"
        "  --mode <m>         interpolate (default, fewer artefacts) | extrapolate (half a frame less latency)\n"
        "                     | hybrid (x3/x4: one interpolated frame, then predictions; 1/m of a frame of latency)\n"
        "  --preset <p>       performance | balanced | quality (default: auto from the GPU)\n"
        "  --filter <0|1|2>   upscale filter: bilinear | bicubic | Lanczos-2 (default)\n"
        "  --sharpness <f>    sharpening after upscaling, 0..1 (default 0)\n"
        "  --hud <0..4>       fps HUD: off | output fps | capture > output | +1%% low | +0.1%% low\n"
        "  --gpu <n|name>     generate on the n-th usable GPU, or the one whose name contains <name>\n"
        "                     (default: the config key gpu, else the discrete one)\n"
        "  --method <m>       auto (default: injection where the Vulkan layer has the game, else capture)\n"
        "                     | layer (injection only) | capture\n"
        "  --fifo             vsync (FIFO) presentation instead of mailbox / immediate\n"
        "  --list             list capturable windows and exit\n"
        "\n"
        "Other options (levels, scene cut, pacing...) come from\n"
        "%%APPDATA%%\\bdex-framegen\\bdex-framegen.conf and BDEX_FG_* variables.\n"
        "A [game] section (the target's .exe name) overrides the globals for\n"
        "that title — a per-game profile; the launcher writes one on request.\n"
        "With no arguments, a launcher window opens (game picker + options);\n"
        "Démarrer saves them to the config file and starts the capture.\n"
        "Ctrl+Alt+Q quits; Ctrl+Alt+G toggles generation on/off (set the\n"
        "'hotkey' config key, e.g. hotkey = ctrl+shift+f1, to rebind it).\n"
        "Closing the game window also quits.\n"
        "Set BDEX_CAP_VALIDATION=1 to load the Khronos validation layer.\n");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--title") {
            const char* v = next("--title");
            if (!v) return false;
            int n = MultiByteToWideChar(CP_ACP, 0, v, -1, nullptr, 0);
            o.title.assign(static_cast<size_t>(std::max(n - 1, 0)), L'\0');
            MultiByteToWideChar(CP_ACP, 0, v, -1, o.title.data(), n);
        } else if (a == "--pid") {
            const char* v = next("--pid");
            if (!v) return false;
            o.pid = static_cast<DWORD>(std::strtoul(v, nullptr, 10));
        } else if (a == "--scale") {
            const char* v = next("--scale");
            if (!v) return false;
            o.scale = std::strtof(v, nullptr);
            if (!(o.scale > 0.05f && o.scale < 16.0f)) {
                std::fprintf(stderr, "--scale out of range\n");
                return false;
            }
        } else if (a == "--fit") {
            o.fit = true;
        } else if (a == "--filter") {
            const char* v = next("--filter");
            if (!v) return false;
            o.filter = std::clamp(std::atoi(v), 0, 2);
        } else if (a == "--hud") {
            const char* v = next("--hud");
            if (!v) return false;
            o.hud = std::clamp(std::atoi(v), 0, 4);
        } else if (a == "--multiplier" || a == "--mult") {
            const char* v = next("--multiplier");
            if (!v) return false;
            o.multiplier = std::clamp(std::atoi(v), 1, 8);
        } else if (a == "--adaptive") {
            o.adaptive = true;
        } else if (a == "--mode") {
            const char* v = next("--mode");
            if (!v) return false;
            std::string m = v;
            if (m == "extrapolate" || m == "extrapolation") o.mode = 1;
            else if (m == "interpolate" || m == "interpolation") o.mode = 0;
            else if (m == "hybrid") o.mode = 2;
            else {
                std::fprintf(stderr, "--mode: extrapolate, interpolate or hybrid\n");
                return false;
            }
        } else if (a == "--preset") {
            const char* v = next("--preset");
            if (!v) return false;
            o.preset = v;
        } else if (a == "--sharpness") {
            const char* v = next("--sharpness");
            if (!v) return false;
            o.sharpness = std::clamp(std::strtof(v, nullptr), 0.f, 1.f);
        } else if (a == "--gpu") {
            const char* v = next("--gpu");
            if (!v) return false;
            if (*v && std::all_of(v, v + std::strlen(v), [](char c) { return c >= '0' && c <= '9'; }))
                o.gpu = std::atoi(v);
            else
                o.gpuName = v;
        } else if (a == "--method") {
            const char* v = next("--method");
            if (!v) return false;
            const std::string m = v;
            o.method = m == "auto" ? 0 : (m == "layer" || m == "injection") ? 1 : m == "capture" ? 2 : -2;
            if (o.method == -2) {
                std::fprintf(stderr, "--method: auto, layer or capture\n");
                return false;
            }
        } else if (a == "--fifo") {
            o.fifo = true;
        } else if (a == "--list") {
            o.list = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            return false;
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            usage();
            return false;
        }
    }
    return true;
}

// --------------------------------------------------------- target window --
// The window picking itself (capturableWindows, processName, narrow) lives
// in launcher.{h,cpp}: the launcher window and the --list option share it.

HWND findTarget(const Options& o) {
    if (!o.title.empty() || o.pid) {
        std::wstring want = bdex::wideLower(o.title);
        for (auto& w : bdex::capturableWindows()) {
            if (o.pid && w.pid != o.pid) continue;
            if (!want.empty() && bdex::wideLower(w.title).find(want) == std::wstring::npos) continue;
            return w.hwnd;
        }
        LOGE("no visible window matches");
        return nullptr;
    }
    std::fprintf(stderr, "Focus the game window: capturing the foreground window in ");
    for (int s = 5; s > 0; --s) {
        std::fprintf(stderr, "%d ", s);
        std::fflush(stderr);
        Sleep(1000);
    }
    std::fputc('\n', stderr);
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    if (!h || pid == GetCurrentProcessId()) {
        LOGE("the foreground window is this console; use --title or --pid");
        return nullptr;
    }
    return GetAncestor(h, GA_ROOT);
}

// Screen rectangle of the window as the user sees it (without the invisible
// resize borders DWM adds around framed windows) — the area WGC captures.
RECT windowRect(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)))
        GetWindowRect(h, &r);
    return r;
}

// Where the overlay goes for a given game rectangle.
RECT overlayRect(const Options& o, HWND target, const RECT& game) {
    LONG gw = game.right - game.left, gh = game.bottom - game.top;
    if (gw <= 0 || gh <= 0) return game;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& mon = mi.rcMonitor;
    if (o.fit) {
        LONG mw = mon.right - mon.left, mh = mon.bottom - mon.top;
        double s = std::min(static_cast<double>(mw) / gw, static_cast<double>(mh) / gh);
        LONG w = static_cast<LONG>(gw * s + 0.5), hgt = static_cast<LONG>(gh * s + 0.5);
        LONG x = mon.left + (mw - w) / 2, y = mon.top + (mh - hgt) / 2;
        return {x, y, x + w, y + hgt};
    }
    LONG w = static_cast<LONG>(gw * o.scale + 0.5f), hgt = static_cast<LONG>(gh * o.scale + 0.5f);
    LONG x = std::clamp(game.left, mon.left, std::max(mon.left, mon.right - w));
    LONG y = std::clamp(game.top, mon.top, std::max(mon.top, mon.bottom - hgt));
    return {x, y, x + w, y + hgt};
}

bool sameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// ---------------------------------------------------------------- overlay --

std::atomic<bool> g_running{true};

LRESULT CALLBACK overlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        g_running = false;
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // never take focus from the game
    case WM_SETCURSOR:
        // The overlay has no class cursor: never impose our own shape. When
        // the game has hidden the hardware cursor (PRAGMATA, Dark Souls III
        // in gameplay), a class-less window still inherits the last-set
        // shape over it — so force-hide in that case instead of showing a
        // lingering/double arrow. In menus the game shows the cursor and we
        // leave its shape untouched.
        if (LOWORD(lp) == HTCLIENT) {
            CURSORINFO ci{sizeof ci};
            if (GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING)) SetCursor(nullptr);
        }
        return TRUE;
    case WM_ERASEBKGND:
        return 1;              // Vulkan paints; no GDI flicker
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

HWND createOverlay(const RECT& r) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = overlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = nullptr;  // see WM_SETCURSOR: mirror the game's cursor, if any
    wc.lpszClassName = L"bdex_capture_overlay";
    RegisterClassExW(&wc);
    // TOPMOST over the game, NOACTIVATE + TOOLWINDOW so it never takes focus
    // or a taskbar button, TRANSPARENT so mouse hit-testing skips it and
    // clicks land on the game underneath.
    HWND h = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        wc.lpszClassName, L"bdex capture", WS_POPUP, r.left, r.top, r.right - r.left,
        r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    return h;
}

// The refresh rate of the monitor the window is on (0 if unknown): the
// output rate of target_fps = auto and of the adaptive multiplier.
double monitorHz(HWND h) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return 0.0;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0.0;
    return dm.dmDisplayFrequency > 1 ? static_cast<double>(dm.dmDisplayFrequency) : 0.0;
}

BOOL WINAPI ctrlHandler(DWORD) {
    g_running = false;
    return TRUE;
}

// A global hotkey spec, e.g. "ctrl+alt+g" -> MOD_CONTROL|MOD_ALT + 'G'.
// Accepts ctrl/control, alt, shift, win/super as modifiers and a single key:
// a letter, a digit, or F1..F24. Returns false (leaving out untouched) for an
// empty or unparsable string; a lone modifier with no key is rejected too, so
// the caller keeps its default.
struct Hotkey {
    UINT mods = 0;
    UINT vk = 0;
};

bool parseHotkey(const std::string& spec, Hotkey& out) {
    Hotkey hk;
    bool haveKey = false;
    size_t i = 0;
    while (i < spec.size()) {
        while (i < spec.size() && (spec[i] == '+' || spec[i] == '-' ||
                                   std::isspace(static_cast<unsigned char>(spec[i]))))
            ++i;
        size_t j = i;
        while (j < spec.size() && spec[j] != '+' && spec[j] != '-' &&
               !std::isspace(static_cast<unsigned char>(spec[j])))
            ++j;
        if (j == i) break;
        std::string tok = spec.substr(i, j - i);
        i = j;
        for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tok == "ctrl" || tok == "control") { hk.mods |= MOD_CONTROL; continue; }
        if (tok == "alt")                       { hk.mods |= MOD_ALT; continue; }
        if (tok == "shift")                     { hk.mods |= MOD_SHIFT; continue; }
        if (tok == "win" || tok == "super" || tok == "meta") { hk.mods |= MOD_WIN; continue; }
        if (haveKey) return false;  // a second non-modifier token: ambiguous
        if (tok.size() == 1 && ((tok[0] >= 'a' && tok[0] <= 'z') || (tok[0] >= '0' && tok[0] <= '9'))) {
            hk.vk = static_cast<UINT>(std::toupper(static_cast<unsigned char>(tok[0])));
            haveKey = true;
        } else if (tok.size() >= 2 && tok[0] == 'f') {
            const int n = std::atoi(tok.c_str() + 1);
            if (n < 1 || n > 24) return false;
            hk.vk = static_cast<UINT>(VK_F1 + (n - 1));
            haveKey = true;
        } else {
            return false;
        }
    }
    if (!haveKey) return false;
    out = hk;
    return true;
}

// Per-monitor DPI awareness so window rectangles and WGC sizes are all in
// physical pixels (the API needs Windows 10 1703+; older builds fall back).
void setDpiAware() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using Fn = BOOL(WINAPI*)(HANDLE);  // DPI_AWARENESS_CONTEXT is a HANDLE
    auto fn = user32 ? reinterpret_cast<Fn>(
                           GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
                     : nullptr;
    const HANDLE perMonitorV2 = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
    if (!fn || !fn(perMonitorV2)) SetProcessDPIAware();
}

} // namespace

// The layer's view of a game (its live status file, status.h): the GPU its
// device runs on, and whether a swapchain of it is armed — the layer
// virtualises it and generates frames as soon as a session targets the game.
struct LayerView {
    bool armed = false;      // a swapchain the layer virtualises (it generates once targeted)
    bool antiCheat = false;  // the layer stayed out (anti-cheat)
    std::string device;      // the GPU the game's device runs on
    std::string antiCheatKept;  // the layer runs beside an anti-cheat (Rocket League, ignore_anticheat): its name
};

LayerView layerView(DWORD pid) {
    LayerView v;
    const std::string dir = bdex::platformStatusDir();
    if (dir.empty() || !pid) return v;
    std::ifstream in(dir + "\\" + std::to_string(pid) + ".status");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (const auto& sec : bdex::statusParse(text).sections) {
        auto get = [&](const char* k) {
            auto it = sec.values.find(k);
            return it == sec.values.end() ? std::string() : it->second;
        };
        const std::string state = get("state");
        if (sec.kind == "device") {
            v.device = get("device");
            v.antiCheat = v.antiCheat || state == "anti-cheat";
            if (!get("anticheat").empty()) v.antiCheatKept = get("anticheat");
        } else if (sec.kind == "swapchain" && (state == "ready" || state == "generating" || state == "paused")) {
            v.armed = true;
        }
    }
    return v;
}

std::wstring utf16(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n - 1, 0)), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

template <typename C> const C* T(const C* fr, const C* en) { return bdex::launcherFrench() ? fr : en; }

// What to run for a game. Injection by default (the user's choice, 03/10):
// the Vulkan layer inside the game's own presentation adds no frame of
// latency, where capture adds one. A Vulkan game the layer has armed is
// injected into at once; a Direct3D game gets DXVK / VKD3D-Proton next to
// its executable (injection.h) and is injected into from its next start —
// captured meanwhile. Capture by default for a game with an anti-cheat
// (except platformAntiCheatException: Rocket League), an OpenGL game, or
// another GPU than the game's (the layer runs on the game's own).
enum class Run { Injection, Capture, Back };
struct Plan {
    Run run = Run::Capture;
    std::string why;      // the log line
    std::wstring notice;  // shown before running (injection prepared, restart the game...)
    bool injection = false;  // the anti-cheat warning's wording
};

Plan planRun(int method, const std::string& gpu, const bdex::GameInfo& g) {
    const LayerView v = layerView(g.pid);
    Plan p;
    std::string antiCheat = g.antiCheat;
    if (antiCheat.empty() && v.antiCheat) antiCheat = "anti-cheat";
    if (antiCheat.empty()) antiCheat = v.antiCheatKept;
    const bool exception = g.antiCheatException || !v.antiCheatKept.empty();
    if (method == 2) {
        p.why = "capture chosen";
        return p;
    }
    if (method == 0 && !gpu.empty() && !v.device.empty() &&
        lowerAscii(v.device).find(lowerAscii(gpu)) == std::string::npos) {
        p.why = "capture on " + gpu + " (the game renders on " + v.device + ")";
        return p;
    }
    if (method == 0 && !antiCheat.empty() && !exception) {
        p.why = "capture (anti-cheat " + antiCheat + ": nothing is loaded into the game)";
        return p;
    }
    if (v.armed) {
        p.run = Run::Injection;
        p.injection = true;
        p.why = "injection (the Vulkan layer, on " + v.device + ")";
        return p;
    }
    // Not armed yet: get the game ready for its next start.
    std::string err;
    if (method == 1 && !antiCheat.empty() && !exception) {
        p.run = Run::Back;
        p.why = "the layer stays out of games with an anti-cheat";
        p.notice = utf16(std::string(T("Anti-triche détecté (", "Anti-cheat detected (")) + antiCheat +
                         T(") : la couche reste hors du jeu (ignore_anticheat = 1 dans sa section pour forcer, à tes risques). "
                           "Choisis Auto ou Capture.",
                           "): the layer stays out of the game (ignore_anticheat = 1 in its section to force it, at your own risk). "
                           "Choose Auto or Capture."));
        return p;
    }
    const bool registered = bdex::layerRegistered() || (bdex::registerLayerForUser(err) && (bdex::armLayer(), true));
    if (!registered) {
        p.why = "capture (the Vulkan layer could not be registered: " + err + ")";
        p.notice = utf16(std::string(T("Injection impossible : la couche Vulkan n'a pas pu être enregistrée (",
                                       "Injection impossible: the Vulkan layer could not be registered (")) + err + ").");
    } else if (g.api == bdex::GameInfo::Vulkan || g.translated) {
        p.why = std::string("the layer does not have ") + g.name + " yet (started before it was registered)";
        p.notice = utf16(g.name) + T(L" : l'injection sera active au prochain lancement du jeu (relance-le).",
                                     L": injection will be active from the game's next start (restart it).");
        p.injection = true;
    } else if (!bdex::translationFiles(g.api).empty()) {
        if (bdex::installTranslation(g, err)) {
            p.why = std::string("injection prepared: ") + bdex::apiName(g.api) + " -> Vulkan next to " + g.name;
            p.notice = utf16(g.name) + T(L" (", L" (") + utf16(bdex::apiName(g.api)) +
                       T(L") : injection installée (DXVK à côté du jeu). Relance le jeu pour en profiter : "
                         L"la latence de la capture disparaît.",
                         L"): injection installed (DXVK next to the game). Restart the game to use it: "
                         L"capture's latency goes away.");
            p.injection = true;
        } else {
            p.why = "injection impossible: " + err;
            p.notice = utf16(std::string(T("Injection impossible : ", "Injection impossible: ")) + err);
        }
    } else {
        p.why = std::string("no injection for ") + bdex::apiName(g.api) + " games";
        if (method == 1)
            p.notice = utf16(std::string(T("Pas d'injection possible pour un jeu ", "No injection for a game in ")) +
                             bdex::apiName(g.api) + T(" : choisis Auto ou Capture.", ": choose Auto or Capture."));
    }
    // Meanwhile: captured (auto), or back to the launcher (injection forced).
    p.run = method == 1 ? Run::Back : Run::Capture;
    if (p.run == Run::Capture) p.why += "; capture meanwhile";
    return p;
}

// Injection: the layer generates inside the game, armed by require_app; this
// process only holds its session — <status dir>\app.session = "<our pid>
// <game pid> <Hz>", rewritten every second (the heartbeat the layer checks)
// — until the game's window closes or Ctrl+Alt+Q. The toggle hotkey targets
// no game (0): the layer passes the frames through until toggled back.
int runInjection(HWND target) {
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    const std::string dir = bdex::platformStatusDir();
    if (dir.empty()) {
        LOGE("no status directory (TEMP unset): injection impossible");
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string session = dir + "\\app.session";
    g_running = true;
    enum { HK_QUIT = 1, HK_TOGGLE = 2 };
    if (!RegisterHotKey(nullptr, HK_QUIT, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q'))
        LOGW("Ctrl+Alt+Q hotkey unavailable; close the game or Ctrl+C to stop");
    Hotkey toggle{MOD_CONTROL | MOD_ALT, 'G'};
    const std::string toggleSpec = bdex::globalConfigValue("hotkey");
    if (!toggleSpec.empty() && !parseHotkey(toggleSpec, toggle))
        LOGW("could not parse hotkey '%s'; using Ctrl+Alt+G", toggleSpec.c_str());
    const bool toggleOk = RegisterHotKey(nullptr, HK_TOGGLE, toggle.mods | MOD_NOREPEAT, toggle.vk) != 0;
    bdex::Tray tray;
    if (!tray.create()) LOGW("notification-area icon unavailable");
    bool on = true;
    tray.setGeneration(on);
    LOGI("injection: the Vulkan layer generates in pid %lu — Ctrl+Alt+Q to stop",
         static_cast<unsigned long>(pid));
    ULONGLONG last = 0;
    while (g_running && IsWindow(target)) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT || (msg.message == WM_HOTKEY && msg.wParam == HK_QUIT) ||
                msg.message == bdex::Tray::kMsgQuit) {
                g_running = false;
            } else if ((msg.message == WM_HOTKEY && msg.wParam == HK_TOGGLE) || msg.message == bdex::Tray::kMsgToggle) {
                on = !on;
                tray.setGeneration(on);
                last = 0;  // (written at once)
                LOGI("frame generation %s", on ? "on" : "off");
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const ULONGLONG now = GetTickCount64();
        if (now - last >= 1000) {
            last = now;
            const std::string tmp = session + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                out << GetCurrentProcessId() << ' ' << (on ? pid : 0) << ' ' << monitorHz(target) << '\n';
            }
            std::filesystem::rename(tmp, session, ec);
            if (ec) LOGW("could not write %s", session.c_str());
            const LayerView v = layerView(pid);
            tray.setStatus(on ? L"bdex framegen — injection" : L"bdex framegen — injection (off)",
                           std::wstring(L"Vulkan layer: ") + (v.armed ? L"armed" : L"not armed") +
                               L"\nGPU: " + utf16(v.device) +
                               (v.antiCheatKept.empty() ? std::wstring()
                                                        : L"\n\u26a0 anti-cheat: " + utf16(v.antiCheatKept) + L" (at your own risk)"));
        }
        Sleep(50);
    }
    std::filesystem::remove(session, ec);
    tray.destroy();
    UnregisterHotKey(nullptr, HK_QUIT);
    if (toggleOk) UnregisterHotKey(nullptr, HK_TOGGLE);
    return 0;
}

// One capture session: overlay + engine + presentation loop. `choice` (the
// launcher) overrides the config file and the command line; nullptr = CLI.
int runCapture(const Options& o, HWND target, const bdex::LaunchChoice* choice) {
    g_running = true;
    std::string gameProc;  // target's executable name, for the config's [game] section
    {
        wchar_t title[256];
        GetWindowTextW(target, title, 256);
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        gameProc = bdex::narrow(bdex::processName(pid));
        LOGI("target: \"%s\" (%s, pid %lu)", bdex::narrow(title).c_str(), gameProc.c_str(),
             static_cast<unsigned long>(pid));
    }

    RECT ov = overlayRect(o, target, windowRect(target));
    HWND overlay = createOverlay(ov);
    if (!overlay) {
        LOGE("overlay window creation failed (%lu)", static_cast<unsigned long>(GetLastError()));
        return 1;
    }

    // Frame-generation settings: the layer's config file / environment, then
    // the command line, then the launcher. Interpolation is the default, as
    // in the layer: extrapolation saves half a frame of latency but its
    // predictions break down on the larger motion of low frame rates, where
    // generation matters most (--mode extrapolate / the launcher for it).
    // The game's process name is added to the section matcher, so a [game]
    // profile in the config file overrides the globals for this title — the
    // per-game profiles the launcher writes and hand-edited sections alike.
    bdex::Config cfg = bdex::Config::load(gameProc);
    cfg.enabled = true;
    if (!o.preset.empty() && !cfg.apply("preset", o.preset)) {
        LOGE("unknown preset '%s' (performance, balanced, quality)", o.preset.c_str());
        return 2;
    }
    if (o.mode >= 0) {
        cfg.extrapolate = o.mode == 1;
        cfg.hybrid = o.mode == 2;
    }
    if (o.multiplier > 0) cfg.multiplier = o.multiplier;
    if (o.adaptive) cfg.adaptive = true;
    if (o.filter >= 0) cfg.upscaleFilter = o.filter;
    if (o.sharpness >= 0.f) cfg.sharpness = o.sharpness;
    if (o.hud >= 0) cfg.overlayMode = o.hud;
    bool fifo = o.fifo;
    if (choice) {
        if (!choice->preset.empty()) cfg.apply("preset", choice->preset);
        cfg.multiplier = choice->multiplier;
        cfg.adaptive = choice->adaptive;
        cfg.extrapolate = choice->extrapolate;
        cfg.hybrid = choice->hybrid;
        cfg.upscaleFilter = choice->filter;
        cfg.sharpness = choice->sharpness;
        cfg.overlayMode = choice->hud;
        fifo = fifo || choice->fifo;
    }
    // The config file's present_mode = fifo asks for vsync in CLI mode too.
    if (!fifo && cfg.presentMode == VK_PRESENT_MODE_FIFO_KHR) fifo = true;

    // Adaptive: a ceiling that can fill the monitor from ~40 fps, unless
    // --multiplier gave one.
    if (cfg.adaptive && o.multiplier <= 0) {
        const double hz = monitorHz(overlay);
        cfg.multiplier = std::max(cfg.multiplier, hz > 250.0 ? 8 : hz > 165.0 ? 6 : 4);
    }
    bdex::VkCtx vk;
    bdex::Capture cap;
    // The GPU that generates: --gpu, else the launcher's choice, else the
    // config key gpu (a part of its name; "" = the best, the discrete one).
    std::string gpuName = !o.gpuName.empty() ? o.gpuName : choice ? choice->gpu : cfg.gpu;
    if (!vk.init(overlay, o.gpu, fifo, cfg, o.gpu >= 0 ? std::string() : gpuName)) return 1;
    // HDR when the game's display is in HDR mode (capture_hdr = auto) or when
    // forced, and the GPU / overlay surface can carry scRGB end to end.
    const bool wantHdr = cfg.captureHdr == 1 || (cfg.captureHdr < 0 && bdex::Capture::displayIsHdr(target));
    if (wantHdr && !vk.hdrCapable()) LOGW("HDR display, but no scRGB path on this GPU/driver: capturing in SDR");
    vk.setHdr(wantHdr);
    LUID luid{};
    if (!cap.start(target, vk.luid(luid) ? &luid : nullptr, vk.hdr())) return 1;
    vk.setCapture(&cap);

    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    if (!vk.setSize(static_cast<uint32_t>(ov.right - ov.left),
                    static_cast<uint32_t>(ov.bottom - ov.top)))
        return 1;

    // Global hotkeys (delivered to this thread's queue, id in msg.wParam):
    //   1 = quit (Ctrl+Alt+Q, fixed);
    //   2 = toggle frame generation on/off, configurable via the `hotkey` key
    //       (default Ctrl+Alt+G). A busy combo (RegisterHotKey fails) is only
    //       a warning: the overlay still runs, generation stays on.
    enum { HK_QUIT = 1, HK_TOGGLE = 2 };
    if (!RegisterHotKey(nullptr, HK_QUIT, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q'))
        LOGW("Ctrl+Alt+Q hotkey unavailable; close the game window or Ctrl+C to quit");
    Hotkey toggle{MOD_CONTROL | MOD_ALT, 'G'};
    const std::string toggleSpec = bdex::globalConfigValue("hotkey");
    if (!toggleSpec.empty() && !parseHotkey(toggleSpec, toggle))
        LOGW("could not parse hotkey '%s'; using Ctrl+Alt+G", toggleSpec.c_str());
    const bool toggleOk =
        RegisterHotKey(nullptr, HK_TOGGLE, toggle.mods | MOD_NOREPEAT, toggle.vk) != 0;
    if (!toggleOk) LOGW("frame-generation toggle hotkey unavailable (combo in use)");
    LOGI("overlay %ldx%ld at %ld,%ld (%s) — Ctrl+Alt+Q to quit%s", ov.right - ov.left,
         ov.bottom - ov.top, ov.left, ov.top, o.fit ? "fit" : "scaled",
         toggleOk ? ", the toggle hotkey to turn generation on/off" : "");

    auto resizeToOverlay = [&] {
        vk.setSize(static_cast<uint32_t>(ov.right - ov.left),
                   static_cast<uint32_t>(ov.bottom - ov.top));
        vk.setDisplayHz(monitorHz(overlay));  // (the overlay may have moved to another monitor)
    };
    vk.setDisplayHz(monitorHz(overlay));

    // Notification-area icon: the live status and a menu (toggle, quit).
    bdex::Tray tray;
    if (!tray.create()) LOGW("notification-area icon unavailable");
    tray.setGeneration(vk.generation());
    auto wide = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(std::max(n - 1, 0)), L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };

    ULONGLONG lastRectCheck = 0, statsT0 = GetTickCount64();
    uint32_t presented = 0, presentedReal = 0;
    std::vector<double> arrivalToSubmitMs, arrivalToPresentMs;
    bool hidden = false;
    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT || (msg.message == WM_HOTKEY && msg.wParam == HK_QUIT) ||
                msg.message == bdex::Tray::kMsgQuit) {
                g_running = false;
            } else if ((msg.message == WM_HOTKEY && msg.wParam == HK_TOGGLE) || msg.message == bdex::Tray::kMsgToggle) {
                const bool on = vk.toggleGeneration();
                tray.setGeneration(on);
                LOGI("frame generation %s", on ? "on" : "off");
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;
        if (!IsWindow(target)) {
            LOGI("game window closed");
            break;
        }

        // Follow the game window: move/resize the overlay, hide it while the
        // game is minimised, keep it above the game after alt-tabs.
        ULONGLONG now = GetTickCount64();
        if (now - lastRectCheck >= 100) {
            lastRectCheck = now;
            // A suboptimal present needs a new swapchain only if the window's
            // size changed (some drivers report it continuously otherwise).
            if (vk.takeSuboptimal() && vk.sizeChanged()) resizeToOverlay();
            bool iconic = IsIconic(target) != 0;
            if (iconic != hidden) {
                ShowWindow(overlay, iconic ? SW_HIDE : SW_SHOWNOACTIVATE);
                hidden = iconic;
            }
            if (!iconic) {
                RECT r = overlayRect(o, target, windowRect(target));
                if (!sameRect(r, ov)) {
                    ov = r;
                    SetWindowPos(overlay, HWND_TOPMOST, r.left, r.top, r.right - r.left,
                                 r.bottom - r.top, SWP_NOACTIVATE);
                    resizeToOverlay();
                } else {
                    SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
        }

        bdex::Capture::Frame frame;
        if (!cap.pop(frame, 20)) continue;
        if (hidden) {
            cap.recycle(frame, false);
            continue;
        }

        // Synthesis: analyse the new frame, generate the in-between /
        // predicted ones. Out-of-date swapchain: rebuild and retry once.
        bool consumed = false;
        int r = vk.synthesize(frame, consumed);
        if (r == 1) {
            resizeToOverlay();
            if (!consumed && vk.hasSwapchain()) r = vk.synthesize(frame, consumed);
        }
        cap.recycle(frame, consumed);
        if (r == -1) {
            LOGE("fatal error, exiting");
            break;
        }
        if (r != 0) continue;
        const double synthLatencyMs = std::chrono::duration<double, std::milli>(
            bdex::VkCtx::clock::now() - frame.dequeuedAt).count();

        // Presentation of the job, paced like the layer's worker
        // (VirtualSwapchain::runJob): with FIFO the display paces; otherwise
        // the frames are spread over the capture interval from when the GPU
        // finished them — the synthesis (interpolation) or the real frame's
        // copy (extrapolation) — not from their submission: the GPU is
        // shared with the game and may end our work much later, and frames
        // timed from the submission then reach the display together. A
        // predicted frame is obsolete as soon as the next real frame has
        // arrived: skip it rather than delay the real one.
        const bdex::VkCtx::Job job = vk.job();
        const bool paced = cfg.pacing && !vk.fifo() && job.frames > 1;
        Clock::time_point origin = job.start;
        if (paced && job.realIndex > 0) origin = vk.waitSynthesis();
        auto framePending = [](void* c) { return static_cast<bdex::Capture*>(c)->waitPending(0); };
        for (int i = 0; i < job.frames && g_running; ++i) {
            if (i > 0) {
                // Frames past the real one are predictions: obsolete, and
                // skipped, once the next real frame has arrived.
                const bool predicted = i > job.realIndex;
                if (paced) {
                    if (job.realIndex == 0 && i == 1) {
                        origin = vk.waitLastCopy(framePending, &cap);
                        if (cap.waitPending(0)) break;
                    }
                    const auto target_t =
                        origin + std::chrono::duration_cast<Clock::duration>(
                                     std::chrono::duration<double, std::milli>(
                                         job.intervalMs * i / job.frames));
                    if (predicted) {
                        if (frameBefore(cap, target_t)) break;
                    } else {
                        sleepUntil(target_t);
                    }
                } else if (predicted && cap.waitPending(0)) {
                    break;
                }
            }
            r = vk.present(i);
            if (r == 1) {
                resizeToOverlay();
                break;
            }
            if (r == -1) {
                LOGE("fatal error, exiting");
                g_running = false;
                break;
            }
            ++presented;
            if (i == job.realIndex) {
                ++presentedReal;
                arrivalToSubmitMs.push_back(synthLatencyMs);
                arrivalToPresentMs.push_back(std::chrono::duration<double, std::milli>(
                    bdex::VkCtx::clock::now() - frame.dequeuedAt).count());
            }
        }

        now = GetTickCount64();
        if (now - statsT0 >= 2000) {
            const float secs = static_cast<float>(now - statsT0) / 1000.0f;
            const float outFps = presented / secs, realFps = presentedReal / secs;
            auto stats = [](std::vector<double>& samples) {
                if (samples.empty()) return std::pair<double, double>{0.0, 0.0};
                double sum = 0.0;
                for (double v : samples) sum += v;
                std::sort(samples.begin(), samples.end());
                const size_t p95 = (samples.size() * 95 - 1) / 100;
                auto result = std::pair<double, double>{sum / samples.size(), samples[p95]};
                samples.clear();
                return result;
            };
            const auto synthLatency = stats(arrivalToSubmitMs);
            const auto presentLatency = stats(arrivalToPresentMs);
            LOGI("capture %.1f fps -> shown %.1f fps -> output %.1f fps; WGC dequeue -> synth submit %.1f/%.1f ms, -> present API %.1f/%.1f ms (mean/p95)",
                 cap.captureFps(), realFps, outFps, synthLatency.first, synthLatency.second,
                 presentLatency.first, presentLatency.second);
            vk.setHud(static_cast<int>(cap.captureFps() + 0.5f), static_cast<int>(outFps + 0.5f));
            // Latency shown: WGC frame -> present call (the part this app controls).
            vk.publishStatus(gameProc, cap.captureFps(), outFps, presentLatency.first);
            {
                // Tooltip: the first line (Windows keeps 127 characters);
                // "Statut détaillé…": all of them.
                const auto lines = vk.statusLines();
                std::wstring details;
                for (const auto& l : lines) details += wide(l) + L"\n";
                tray.setStatus(L"bdex framegen\n" + (lines.empty() ? std::wstring() : wide(lines[0])), details);
            }
            presented = presentedReal = 0;
            statsT0 = now;
        }
    }

    tray.destroy();
    UnregisterHotKey(nullptr, HK_QUIT);
    if (toggleOk) UnregisterHotKey(nullptr, HK_TOGGLE);
    // Order matters: Vulkan drops its references to the shared textures
    // before Capture destroys them.
    vk.shutdown();
    cap.stop();
    DestroyWindow(overlay);
    return 0;
}

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return 2;
    // The capture product owns its Vulkan swapchain and drives FrameGen itself.
    // Never load the implicit layer into this process, even when a global
    // enabled=1 profile is active for games.
    SetEnvironmentVariableA("BDEX_FG_DISABLE", "1");
    // Injection is the default (03/10): the Vulkan layer registered for this
    // user and Vulkan games armed, unless capture is chosen.
    if (bdex::Config::load("").method != 2) {
        std::string err;
        if (bdex::registerLayerForUser(err)) bdex::armLayer();
        else LOGW("Vulkan layer not registered (%s): capture only", err.c_str());
    }
    // Generating is this program's purpose: its config is read as explicitly
    // enabled, so that require_app (the layer's arming, for injection) does
    // not pause it nor turn its HUD off. The CRT's copy of the environment,
    // which getenv reads (SetEnvironmentVariable does not update it).
    _putenv_s("BDEX_FG", "1");
    SetConsoleOutputCP(CP_UTF8);
    setDpiAware();
    // 1 ms timer resolution for the pacing waits (see sleepUntil); per
    // process since Windows 10 2004, released at exit.
    timeBeginPeriod(1);

    if (o.list) {
        for (auto& w : bdex::capturableWindows())
            std::printf("%6lu  %-24s  %s\n", static_cast<unsigned long>(w.pid),
                        bdex::narrow(bdex::processName(w.pid)).c_str(),
                        bdex::narrow(w.title).c_str());
        return 0;
    }

    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // No arguments: the launcher window (game picker + options), then the
    // chosen capture runs; when it stops we come back to the launcher until
    // the user quits. With arguments the CLI path runs once, as before.
    const bool useLauncher = argc == 1;
    bdex::LaunchChoice choice;
    HWND target = nullptr;
    if (useLauncher) {
        const bdex::Config d = bdex::Config::load("");
        choice.multiplier = std::max(1, d.multiplier);
        choice.extrapolate = d.extrapolate;
        choice.hybrid = d.hybrid;
        choice.adaptive = d.adaptive;
        choice.filter = d.upscaleFilter;
        choice.sharpness = d.sharpness;
        choice.hud = d.overlayMode;
        choice.preset = bdex::globalConfigValue("preset");
        choice.fifo = bdex::globalConfigValue("present_mode") == "fifo";
        choice.gradWeight = d.gradWeight;
        choice.smoothness = d.smoothness;
        choice.zeroBias = d.zeroBias;
        choice.searchFine = d.searchFine;
        choice.flowIterations = d.flowIterations;
        choice.sceneCutLow = d.sceneCutLow;
        choice.sceneCutHigh = d.sceneCutHigh;
        choice.gpu = d.gpu;
        choice.method = d.method;
        if (!bdex::runLauncher(choice)) return 0;
        target = choice.target;
        if (!target) return 1;
        o.scale = choice.scale;
        o.fit = choice.fit;
    } else {
        target = findTarget(o);
        if (!target) return 1;
    }

    for (;;) {
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        const bdex::GameInfo game = bdex::inspectGame(pid);
        const bdex::Config d = bdex::Config::load(game.name);
        const int method = useLauncher ? choice.method : o.method >= 0 ? o.method : d.method;
        const std::string gpu = useLauncher ? choice.gpu : !o.gpuName.empty() ? o.gpuName : d.gpu;
        Plan plan = o.gpu >= 0 ? Plan{Run::Capture, "capture (--gpu index)", {}, false} : planRun(method, gpu, game);
        LOGI("%s (%s): %s", game.name.c_str(), bdex::apiName(game.api), plan.why.c_str());
        auto show = [&](const std::wstring& text, UINT icon) {
            if (useLauncher) MessageBoxW(nullptr, text.c_str(), L"bdex framegen", MB_OK | icon | MB_SETFOREGROUND);
            else LOGW("%s", bdex::narrow(text).c_str());
        };
        if (!plan.notice.empty()) show(plan.notice, MB_ICONINFORMATION);
        // Prevention, the launch going ahead: a game with an anti-cheat.
        const LayerView lv = layerView(pid);
        if (plan.run != Run::Back && (!game.antiCheat.empty() || lv.antiCheat || !lv.antiCheatKept.empty())) {
            bdex::GameInfo shown = game;
            if (shown.antiCheat.empty()) shown.antiCheat = !lv.antiCheatKept.empty() ? lv.antiCheatKept : "anti-cheat";
            show(bdex::antiCheatWarning(shown, plan.run == Run::Injection), MB_ICONWARNING);
        }
        if (plan.run == Run::Back) {
            if (!useLauncher) return 1;
            if (!bdex::runLauncher(choice)) return 0;
            target = choice.target;
            continue;
        }
        const int rc = plan.run == Run::Injection ? runInjection(target) : runCapture(o, target, useLauncher ? &choice : nullptr);
        if (!useLauncher || rc != 0) return rc;
        if (!bdex::runLauncher(choice)) return 0;
        target = choice.target;
    }
}
