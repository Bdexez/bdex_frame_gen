// bdex_capture — phase-1 proof of concept of the Windows capture product
// (docs/windows-capture.md §3): capture one game window with
// Windows.Graphics.Capture, share the frames into Vulkan, run the layer's
// upscale shader and present the result in a borderless always-on-top overlay
// placed over (or instead of) the game window. Console app: logs go to stderr.
//
//   bdex_capture [--title <substr> | --pid <n>] [--scale <f> | --fit]
//                [--filter 0|1|2] [--hud 0..2] [--fifo] [--list]
//
// Without --title/--pid the foreground window 5 s after launch is captured
// (alt-tab into the game). Ctrl+Alt+Q or closing the game window quits.
#include "capture.h"
#include "log.h"
#include "vkctx.h"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace {

struct Options {
    std::wstring title;
    DWORD pid = 0;
    float scale = 1.0f;
    bool fit = false;
    int filter = 1;
    int hud = 0;
    bool fifo = false;
    bool list = false;
};

void usage() {
    std::puts(
        "bdex_capture — capture a game window, upscale it in Vulkan, show it in an overlay\n"
        "\n"
        "  --title <substr>   target window whose title contains <substr> (case-insensitive)\n"
        "  --pid <n>          target the main window of process <n>\n"
        "  (neither)          the foreground window 5 seconds after launch\n"
        "  --scale <f>        overlay size = capture size x f (default 1.0)\n"
        "  --fit              overlay fills the game's monitor, aspect preserved\n"
        "  --filter <0|1|2>   bilinear | bicubic (default) | Lanczos-2\n"
        "  --hud <0|1|2>      fps HUD: off | output fps | capture > output\n"
        "  --fifo             vsync (FIFO) presentation instead of mailbox\n"
        "  --list             list capturable windows and exit\n"
        "\n"
        "Ctrl+Alt+Q quits; so does closing the game window.\n"
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
            o.hud = std::clamp(std::atoi(v), 0, 2);
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

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// UTF-8 for the byte-oriented log/console (SetConsoleOutputCP(CP_UTF8) in main).
std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring processName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"?";
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring name = L"?";
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        name = buf;
        size_t p = name.find_last_of(L"\\/");
        if (p != std::wstring::npos) name = name.substr(p + 1);
    }
    CloseHandle(h);
    return name;
}

struct WindowInfo {
    HWND hwnd;
    DWORD pid;
    std::wstring title;
};

// Visible, titled, un-cloaked top-level windows — what WGC can capture and
// what a user would recognise in --list.
std::vector<WindowInfo> capturableWindows() {
    std::vector<WindowInfo> out;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* v = reinterpret_cast<std::vector<WindowInfo>*>(lp);
            if (!IsWindowVisible(h) || GetWindowTextLengthW(h) == 0) return TRUE;
            if (GetAncestor(h, GA_ROOT) != h) return TRUE;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
            if (cloaked) return TRUE;
            wchar_t title[256];
            GetWindowTextW(h, title, 256);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) return TRUE;
            v->push_back({h, pid, title});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

HWND findTarget(const Options& o) {
    if (!o.title.empty() || o.pid) {
        std::wstring want = lower(o.title);
        for (auto& w : capturableWindows()) {
            if (o.pid && w.pid != o.pid) continue;
            if (!want.empty() && lower(w.title).find(want) == std::wstring::npos) continue;
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
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
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

BOOL WINAPI ctrlHandler(DWORD) {
    g_running = false;
    return TRUE;
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

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return 2;
    SetConsoleOutputCP(CP_UTF8);
    setDpiAware();

    if (o.list) {
        for (auto& w : capturableWindows())
            std::printf("%6lu  %-24s  %s\n", static_cast<unsigned long>(w.pid),
                        narrow(processName(w.pid)).c_str(), narrow(w.title).c_str());
        return 0;
    }

    HWND target = findTarget(o);
    if (!target) return 1;
    {
        wchar_t title[256];
        GetWindowTextW(target, title, 256);
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        LOGI("target: \"%s\" (%s, pid %lu)", narrow(title).c_str(),
             narrow(processName(pid)).c_str(), static_cast<unsigned long>(pid));
    }

    RECT ov = overlayRect(o, target, windowRect(target));
    HWND overlay = createOverlay(ov);
    if (!overlay) {
        LOGE("overlay window creation failed (%lu)", static_cast<unsigned long>(GetLastError()));
        return 1;
    }

    bdex::VkCtx vk;
    bdex::Capture cap;
    if (!vk.init(overlay, o.fifo)) return 1;
    if (!cap.start(target)) return 1;
    vk.setCapture(&cap);

    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    if (!vk.setSize(static_cast<uint32_t>(ov.right - ov.left),
                    static_cast<uint32_t>(ov.bottom - ov.top)))
        return 1;

    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    if (!RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q'))
        LOGW("Ctrl+Alt+Q hotkey unavailable; close the game window or Ctrl+C to quit");
    LOGI("overlay %ldx%ld at %ld,%ld (%s, filter %d) — Ctrl+Alt+Q to quit",
         ov.right - ov.left, ov.bottom - ov.top, ov.left, ov.top,
         o.fit ? "fit" : "scaled", o.filter);

    bdex::DrawParams params;
    params.filterMode = o.filter;
    params.hudMode = o.hud;

    ULONGLONG lastRectCheck = 0, statsT0 = GetTickCount64();
    uint32_t presented = 0;
    bool hidden = false;
    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY || msg.message == WM_QUIT) g_running = false;
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
                    vk.setSize(static_cast<uint32_t>(r.right - r.left),
                               static_cast<uint32_t>(r.bottom - r.top));
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

        bool consumed = false;
        int r = vk.draw(frame, params, consumed);
        if (r == 1) {
            // Swapchain out of date (overlay resized): rebuild at the current
            // overlay size and retry once if this frame was not consumed.
            vk.setSize(static_cast<uint32_t>(ov.right - ov.left),
                       static_cast<uint32_t>(ov.bottom - ov.top));
            if (!consumed && vk.hasSwapchain()) r = vk.draw(frame, params, consumed);
        }
        cap.recycle(frame, consumed);
        if (r == -1) {
            LOGE("fatal draw error, exiting");
            break;
        }
        if (consumed) ++presented;

        if (now - statsT0 >= 2000) {
            float outFps = presented * 1000.0f / static_cast<float>(now - statsT0);
            LOGI("capture %.1f fps -> output %.1f fps", cap.captureFps(), outFps);
            params.hudGame = static_cast<int>(cap.captureFps() + 0.5f);
            params.hudOut = static_cast<int>(outFps + 0.5f);
            presented = 0;
            statsT0 = now;
        }
    }

    UnregisterHotKey(nullptr, 1);
    // Order matters: Vulkan drops its references to the shared textures
    // before Capture destroys them.
    vk.shutdown();
    cap.stop();
    DestroyWindow(overlay);
    return 0;
}
