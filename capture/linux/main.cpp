// bdex-capture (Linux): frame generation for a game window by capture, the
// Linux counterpart of the Windows bdex_capture — nothing is injected into
// the game, so it also serves the games the Vulkan layer stays out of
// (anti-cheat), non-Vulkan programs, and two-GPU setups: the game renders on
// one GPU, the capture generates on another (--gpu), the one driving the
// screen, like Lossless Scaling's second-GPU mode.
//
//   bdex-capture [--name TEXT | --pid PID | --window 0xID | --pick] [--gpu N|NAME]
//                [-m N] [--mode interpolate|extrapolate|hybrid] [--preset P] [--hud N]
//                [--fifo] [--hotkey ctrl+alt+g] [--duration S] [--list] [--list-gpus]
//
// X11 sessions and Xwayland windows (XComposite + XDamage + MIT-SHM); the
// overlay is an override-redirect window without input region above the
// game, following it. Settings: the layer's configuration file, with the
// game's [section] (the process of the window), then BDEX_FG_* variables,
// then the command line. Ctrl+Alt+G toggles the generation, Ctrl+Alt+Q quits
// (--hotkey or BDEX_FG_HOTKEY rebinds the toggle, e.g. ctrl+shift+f1).
#include "config.h"
#include "framegen.h"
#include "log.h"
#include "platform.h"

#include "presenter.h"
#include "x11_capture.h"
#ifdef BDEX_PIPEWIRE
#include "pw_capture.h"
#endif

#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

int g_xerrors = 0;
int onXError(Display*, XErrorEvent* e) {
    // A window destroyed or resized under us (BadDrawable / BadMatch on the
    // old pixmap): counted, the loop notices from the events.
    ++g_xerrors;
    if (g_xerrors <= 5) BDEX_WARN("X error %d (request %d)", e->error_code, e->request_code);
    return 0;
}

struct Options {
    std::string name, gpu, mode, preset, hotkey;
    long pid = 0;
    Window window = 0;
    bool pick = false, list = false, listGpus = false, fifo = false;
    bool wayland = false, waylandScreen = false;  // --wayland: the screen-cast portal (a native Wayland window)
    int rect[4] = {0, 0, 0, 0};                    // --rect: where the overlay goes (Wayland)
    int multiplier = 0, hud = -1;
    double duration = 0.0;
};

void usage() {
    std::printf(
        "usage: bdex-capture [--name TEXT | --pid PID | --window 0xID | --pick] [--gpu N|NAME]\n"
        "                    [-m N] [--mode interpolate|extrapolate|hybrid] [--preset P] [--hud N]\n"
        "                    [--fifo] [--hotkey ctrl+alt+g] [--duration S] [--list] [--list-gpus]\n"
        "                    [--wayland [--screen] [--rect X,Y,W,H]]\n"
        "Frame generation for a game window by capture (X11 / Xwayland; --wayland: a native\n"
        "Wayland window through the desktop's screen-cast portal, chosen in its dialog, the\n"
        "overlay over --rect, by default the whole screen — a fullscreen game). Without a window\n"
        "choice: click the game's window. --gpu: generate on that GPU (number or part of\n"
        "its name; --list-gpus) — another one than the game's: two-GPU frame generation.\n"
        "Ctrl+Alt+G: generation on/off, Ctrl+Alt+Q: quit.\n");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--name" || a == "--title") o.name = value();
        else if (a == "--pid") o.pid = std::atol(value().c_str());
        else if (a == "--window") o.window = static_cast<Window>(std::strtoul(value().c_str(), nullptr, 0));
        else if (a == "--pick") o.pick = true;
        else if (a == "--gpu") o.gpu = value();
        else if (a == "-m" || a == "--multiplier") o.multiplier = std::atoi(value().c_str());
        else if (a == "--mode") o.mode = value();
        else if (a == "--preset") o.preset = value();
        else if (a == "--hud") o.hud = std::atoi(value().c_str());
        else if (a == "--fifo") o.fifo = true;
        else if (a == "--hotkey") o.hotkey = value();
        else if (a == "--duration") o.duration = std::atof(value().c_str());
        else if (a == "--list") o.list = true;
        else if (a == "--list-gpus") o.listGpus = true;
        else if (a == "--wayland") o.wayland = true;
        else if (a == "--screen") o.waylandScreen = true;
        else if (a == "--rect") {
            if (std::sscanf(value().c_str(), "%d,%d,%d,%d", &o.rect[0], &o.rect[1], &o.rect[2], &o.rect[3]) != 4) {
                std::fprintf(stderr, "--rect X,Y,W,H\n");
                return false;
            }
        }
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return false;
        }
    }
    return true;
}

// The overlay: override-redirect (no window manager decoration nor
// placement), no input region (clicks and keys reach the game below).
Window makeOverlay(Display* dpy, const bdex::X11Window& at) {
    XSetWindowAttributes swa{};
    swa.override_redirect = True;
    swa.background_pixel = BlackPixel(dpy, DefaultScreen(dpy));
    swa.border_pixel = 0;
    const Window w = XCreateWindow(dpy, DefaultRootWindow(dpy), at.x, at.y, static_cast<unsigned>(at.width),
                                   static_cast<unsigned>(at.height), 0, CopyFromParent, InputOutput, CopyFromParent,
                                   CWOverrideRedirect | CWBackPixel | CWBorderPixel, &swa);
    XStoreName(dpy, w, "bdex-capture overlay");
    XClassHint ch{const_cast<char*>("bdex-capture"), const_cast<char*>("bdex-capture")};
    XSetClassHint(dpy, w, &ch);
    int ev, err;
    if (XFixesQueryExtension(dpy, &ev, &err)) {
        const XserverRegion none = XFixesCreateRegion(dpy, nullptr, 0);
        XFixesSetWindowShapeRegion(dpy, w, ShapeInput, 0, 0, none);
        XFixesDestroyRegion(dpy, none);
    } else {
        BDEX_WARN("no XFIXES: the overlay takes the mouse clicks");
    }
    XMapRaised(dpy, w);
    XSync(dpy, False);
    return w;
}

// Global hotkeys on the root window (with and without Num Lock / Caps Lock).
struct Hotkey {
    KeyCode code = 0;
    unsigned mods = 0;
};
void grab(Display* dpy, const Hotkey& h) {
    if (!h.code) return;
    for (unsigned extra : {0u, static_cast<unsigned>(Mod2Mask), static_cast<unsigned>(LockMask),
                           static_cast<unsigned>(Mod2Mask | LockMask)})
        XGrabKey(dpy, h.code, h.mods | extra, DefaultRootWindow(dpy), True, GrabModeAsync, GrabModeAsync);
}
Hotkey parseHotkey(Display* dpy, const std::string& spec, KeySym fallback) {
    Hotkey h;
    KeySym sym = fallback;
    if (!spec.empty()) {
        h.mods = 0;
        std::string s = spec, part;
        for (size_t p = 0; p <= s.size(); ++p) {
            if (p == s.size() || s[p] == '+') {
                std::string l;
                for (char c : part) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (l == "ctrl" || l == "control") h.mods |= ControlMask;
                else if (l == "alt") h.mods |= Mod1Mask;
                else if (l == "shift") h.mods |= ShiftMask;
                else if (l == "win" || l == "super") h.mods |= Mod4Mask;
                else if (!l.empty()) sym = XStringToKeysym(l.size() == 1 ? l.c_str() : part.c_str());
                part.clear();
            } else {
                part += s[p];
            }
        }
    } else {
        h.mods = ControlMask | Mod1Mask;
    }
    if (sym != NoSymbol) h.code = XKeysymToKeycode(dpy, sym);
    return h;
}

void sleepUntil(Clock::time_point t) {
    const auto now = Clock::now();
    if (t > now + std::chrono::microseconds(800)) std::this_thread::sleep_until(t - std::chrono::microseconds(500));
    while (Clock::now() < t) std::this_thread::yield();
}

// The capture loop, for either source (X11Capture, PwCapture): each new
// frame synthesised and its frames presented, paced like the Windows capture
// (capture/src/main.cpp) and the layer's worker.
template <class Cap>
int generate(Cap& cap, Display* dpy, Window overlay, bdex::Presenter& vk, const bdex::Config& cfg, const Options& o,
             const bdex::X11Window& target) {
    using namespace bdex;
    auto resize = [&]() {
        const X11Window& w = cap.window();
        XMoveResizeWindow(dpy, overlay, w.x, w.y, static_cast<unsigned>(std::max(1, w.width)),
                          static_cast<unsigned>(std::max(1, w.height)));
        XRaiseWindow(dpy, overlay);
        XSync(dpy, False);
        vk.setSize(static_cast<uint32_t>(w.width), static_cast<uint32_t>(w.height));
    };
    resize();

    const Hotkey toggle = parseHotkey(dpy, o.hotkey, XK_g), quit = parseHotkey(dpy, "", XK_q);
    grab(dpy, toggle);
    grab(dpy, quit);
    XSelectInput(dpy, DefaultRootWindow(dpy), KeyPressMask);

    BDEX_INFO("bdex-capture: %s on %s; Ctrl+Alt+G toggles, Ctrl+Alt+Q quits", target.process.c_str(),
              vk.gpuName().c_str());
    const auto startT = Clock::now();
    auto statsT = startT, raiseT = startT;
    int presented = 0, presentedReal = 0;
    double latencySum = 0.0;
    int latencyN = 0, slot = 0;
    bool hidden = false;
    struct Pending {
        Cap* cap;
        Display* dpy;
    } pending{&cap, dpy};
    auto framePending = [](void* c) {
        auto* p = static_cast<Pending*>(c);
        p->cap->pump(0);
        return p->cap->damaged();
    };

    while (!g_stop) {
        if (o.duration > 0.0 && std::chrono::duration<double>(Clock::now() - startT).count() >= o.duration) break;
        // Hotkeys (root KeyPress events reach us through XGrabKey).
        while (XPending(dpy)) {
            XEvent ev;
            XPeekEvent(dpy, &ev);
            if (ev.type != KeyPress) break;
            XNextEvent(dpy, &ev);
            const unsigned mods = ev.xkey.state & (ControlMask | Mod1Mask | ShiftMask | Mod4Mask);
            if (ev.xkey.keycode == quit.code && mods == quit.mods) g_stop = 1;
            else if (ev.xkey.keycode == toggle.code && mods == toggle.mods)
                BDEX_INFO("frame generation %s", vk.toggleGeneration() ? "on" : "off");
        }
        cap.pump(20);
        if (cap.gone()) {
            BDEX_INFO("game window closed");
            break;
        }
        const auto now = Clock::now();
        if (cap.visible() == hidden) {
            hidden = !cap.visible();
            if (hidden) XUnmapWindow(dpy, overlay);
            else XMapRaised(dpy, overlay);
        }
        if (cap.takeMoved()) resize();
        if (now - raiseT > std::chrono::milliseconds(250)) {  // above the game after it was raised
            raiseT = now;
            XRaiseWindow(dpy, overlay);
            if (vk.takeSuboptimal() && vk.sizeChanged()) resize();
        }
        if (hidden || !cap.damaged()) continue;

        vk.waitSlot(slot);
        X11Capture::Frame frame;
        if (!cap.grab(slot, frame)) continue;
        slot = (slot + 1) % Cap::kSlots;
        int r = vk.synthesize(frame);
        if (r == 1) {
            resize();
            r = vk.synthesize(frame);
        }
        if (r == -1) {
            BDEX_ERR("fatal error, exiting");
            break;
        }
        if (r != 0) continue;

        // The job's frames, paced like the Windows capture (capture/src/main.cpp)
        // and the layer's worker: from when the GPU finished them.
        const Presenter::Job job = vk.job();
        const bool paced = cfg.pacing && !vk.fifo() && job.frames > 1;
        Clock::time_point origin = job.start;
        if (paced && job.realIndex > 0) origin = vk.waitSynthesis();
        for (int i = 0; i < job.frames && !g_stop; ++i) {
            if (i > 0) {
                const bool predicted = i > job.realIndex;
                if (paced) {
                    if (job.realIndex == 0 && i == 1) {
                        origin = vk.waitLastCopy(framePending, &pending);
                        if (cap.damaged()) break;
                    }
                    const auto t = origin + std::chrono::duration_cast<Clock::duration>(
                                                std::chrono::duration<double, std::milli>(job.intervalMs * i / job.frames));
                    if (predicted) {
                        bool next = false;
                        while (Clock::now() < t && !(next = framePending(&pending)))
                            std::this_thread::sleep_for(std::chrono::microseconds(500));
                        if (next) break;
                    } else {
                        sleepUntil(t);
                    }
                } else if (predicted && framePending(&pending)) {
                    break;
                }
            }
            r = vk.present(i);
            if (r == 1) {
                resize();
                break;
            }
            if (r == -1) {
                BDEX_ERR("fatal error, exiting");
                g_stop = 1;
                break;
            }
            ++presented;
            if (i == job.realIndex) {
                ++presentedReal;
                latencySum += std::chrono::duration<double, std::milli>(Clock::now() - frame.damagedAt).count();
                ++latencyN;
            }
        }

        const double secs = std::chrono::duration<double>(Clock::now() - statsT).count();
        if (secs >= 2.0) {
            const double outFps = presented / secs, realFps = presentedReal / secs;
            const double lat = latencyN ? latencySum / latencyN : -1.0;
            BDEX_INFO("game %.1f fps -> output %.1f fps (x%.2f), damage -> present %.1f ms, %s%s", realFps, outFps,
                      realFps > 0 ? outFps / realFps : 0.0, lat, vk.hostImport() ? "in place" : "copied",
                      vk.generation() ? "" : ", generation off");
            vk.setHud(static_cast<int>(realFps + 0.5), static_cast<int>(outFps + 0.5));
            vk.publishStatus(target.process, realFps, outFps, lat);
            presented = presentedReal = latencyN = 0;
            latencySum = 0.0;
            statsT = Clock::now();
        }
    }

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    using namespace bdex;
    Options o;
    if (!parse(argc, argv, o)) return 2;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "bdex-capture: cannot open the X display (DISPLAY=%s)\n",
                     std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "");
        return 1;
    }
    XSetErrorHandler(onXError);
    X11Capture cap(dpy);
    if (o.list) {
        for (const auto& w : cap.list())
            std::printf("0x%08lx  pid %-7ld %-20s %4dx%-4d  %s\n", w.id, w.pid, w.process.c_str(), w.width, w.height,
                        w.title.c_str());
        return 0;
    }

    X11Window target;
    bool found = false;
    if (o.wayland) {
#ifndef BDEX_PIPEWIRE
        std::fprintf(stderr, "bdex-capture: built without PipeWire (libpipewire-0.3, gio-2.0): no --wayland\n");
        return 1;
#endif
        // The window is chosen in the portal's dialog; the overlay goes
        // over --rect, else the whole screen. --name names the game's
        // [section] of the configuration.
        target.process = o.name;
        target.title = o.name;
        target.x = o.rect[0];
        target.y = o.rect[1];
        target.width = o.rect[2] > 0 ? o.rect[2] : DisplayWidth(dpy, DefaultScreen(dpy));
        target.height = o.rect[3] > 0 ? o.rect[3] : DisplayHeight(dpy, DefaultScreen(dpy));
        found = true;
    } else if (o.window || o.pid || !o.name.empty()) {
        // (a game just started may not have mapped its window yet: a few tries)
        for (int i = 0; i < 50 && !found && !g_stop; ++i) {
            found = cap.find(target, o.window, o.pid, o.name);
            if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    } else if (!o.listGpus) {
        std::fprintf(stderr, "bdex-capture: click the game's window\n");
        found = cap.pick(target);
    }
    if (!found && !o.listGpus) {
        std::fprintf(stderr, "bdex-capture: no such window (--list shows them)\n");
        return 1;
    }

    // Settings: the configuration file with the game's section, then the
    // environment (Config::load), then the command line.
    // Read as explicitly enabled (BDEX_FG=1): generating is this program's
    // purpose, and require_app (the panel's arming of games for the layer)
    // would otherwise pause it and turn its HUD off.
    setenv("BDEX_FG", "1", 1);
    platformEnvForget();  // (the engine reads a copy of the environment)
    Config cfg = Config::load(target.process);
    log_set_level(cfg.logLevel);
    cfg.enabled = true;
    if (!o.preset.empty() && !cfg.applyPreset(o.preset)) BDEX_WARN("unknown preset %s", o.preset.c_str());
    if (o.multiplier > 0) cfg.apply("multiplier", std::to_string(o.multiplier));
    if (!o.mode.empty() && !cfg.apply("mode", o.mode)) BDEX_WARN("unknown mode %s", o.mode.c_str());
    if (o.hud >= 0) cfg.apply("overlay", std::to_string(o.hud));
    if (cfg.multiplier < 2 && o.multiplier == 0) cfg.multiplier = 2;  // (the panel's x1 means "off" for the layer)
    if (o.gpu.empty()) o.gpu = cfg.gpu;          // the configuration's GPU (panel, launchers)
    if (o.hotkey.empty()) o.hotkey = cfg.hotkey;

    // Our own Vulkan instance loads the layer too (implicit): it must stay a
    // pass-through there, whatever the configuration or BDEX_FG says.
    setenv("BDEX_FG", "0", 1);
    platformEnvForget();

    const Window overlay = makeOverlay(dpy, found ? target : X11Window{0, 0, "", "", 0, 0, 64, 64});
    if (o.listGpus) {
        int i = 0;
        for (const auto& g : Presenter::listGpus(dpy, overlay)) std::printf("%d: %s\n", i++, g.c_str());
        return 0;
    }
    Presenter vk;
    if (!vk.init(dpy, overlay, o.gpu, o.fifo, cfg)) {
        vk.shutdown();
        return 1;
    }
    int rc = 1;
#ifdef BDEX_PIPEWIRE
    if (o.wayland) {
        PwCapture pw;
        if (pw.start(o.waylandScreen, target)) rc = generate(pw, dpy, overlay, vk, cfg, o, target);
        vk.shutdown();
        pw.stop();
        XDestroyWindow(dpy, overlay);
        XCloseDisplay(dpy);
        return rc;
    }
#endif
    if (!cap.start(target)) {
        vk.shutdown();
        return 1;
    }
    rc = generate(cap, dpy, overlay, vk, cfg, o, target);
    vk.shutdown();
    cap.stop();
    XDestroyWindow(dpy, overlay);
    XCloseDisplay(dpy);
    return rc;
}
