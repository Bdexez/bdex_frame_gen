#pragma once
// The refresh grid of an X11 window, from the X server itself (x11_clock).
//
// An X11 surface on a Wayland compositor (Xwayland: every Proton game under
// Hyprland, KDE, GNOME...) presented in immediate mode tells the present
// feedback nothing about the display: Xwayland copies each present into the
// window at once and reports it complete at once, so vkWaitForPresentKHR
// returns at the present itself (measured under KWin's virtual output, 60 Hz:
// "displayed 120 fps"). The layer then paced on a grid of its own at an
// arbitrary phase, and frames presented near the moment the compositor takes
// the window's content met there two to a refresh, then none.
//
// The X Present extension's completion events of that window carry the
// window's MSC and UST at each present's execution: under Xwayland the
// moment its last frame callback was processed — when Xwayland hands the
// window's content to the compositor, i.e. the instant a present must beat —
// and on a real X server the last vblank. Any client may select them on any
// window: the layer opens its own connection (libxcb, loaded at run time),
// leaving the game's alone, and learns the grid from them (DisplayClock),
// with what really reached the screen: a present whose successor executed
// before the next MSC was replaced.
#include "display_clock.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bdex {

// Pure logic (tests/test_x11_present_clock.cpp): the completion events of
// one window's presents, in order, each with the MSC and UST (ms) it was
// executed at.
class PresentTimeline {
public:
    PresentTimeline() { clock_.setJitter(1.0, 0.15); }  // (compositor frame callbacks: DisplayClock::setJitter)
    void setHint(double hz) { clock_.setHint(hz); hintMs_ = hz > 0.0 ? 1000.0 / hz : 0.0; }
    void add(uint64_t msc, double ustMs);
    bool valid() const { return clock_.valid(); }
    double lastMs() const { return clock_.lastMs(); }
    double periodMs() const { return clock_.periodMs(); }
    double refreshHz() const { return clock_.refreshHz(); }
    uint64_t presents = 0;  // completion events seen
    uint64_t shown = 0;     // presents that reached the window's content handed to the compositor
    uint64_t replaced = 0;  // presents overwritten before that (their successor in the same MSC)
    uint64_t held = 0;      // refreshes that brought nothing new (the content handed over a refresh late or more)

private:
    DisplayClock clock_;
    double hintMs_ = 0.0;
    bool have_ = false;
    uint64_t lastMsc_ = 0;
    double lastTickMs_ = 0.0;
};

class X11PresentClock {
public:
    // Watches `window` (an XID) on $DISPLAY; nullptr when libxcb, the X
    // server or its Present extension is unavailable (or not on Linux).
    static std::unique_ptr<X11PresentClock> start(uint32_t window, double hintHz);
    ~X11PresentClock();
    X11PresentClock(const X11PresentClock&) = delete;
    X11PresentClock& operator=(const X11PresentClock&) = delete;

    void setHint(double hz);
    bool grid(double& lastMs, double& periodMs);  // the refresh grid, once learned
    double refreshHz();                           // 0 until learned
    struct Counts { uint64_t presents = 0, shown = 0, replaced = 0, held = 0; };
    Counts counts();

private:
    X11PresentClock() = default;
    void run();
    struct Xcb;
    std::unique_ptr<Xcb> xcb_;
    std::thread thread_;
    int stopPipe_[2] = {-1, -1};
    std::mutex mutex_;
    PresentTimeline timeline_;  // mutex_
};

} // namespace bdex
