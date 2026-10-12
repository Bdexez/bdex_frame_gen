// The game window's frames on X11 (an X11 session, or an Xwayland window —
// Proton games are, unless they use Wine's Wayland driver): the window is
// redirected off-screen (XComposite, so the overlay over it never feeds
// back), each change of its content arrives as an XDamage event, and the
// frame is read from its pixmap with MIT-SHM into a ring of shared-memory
// segments (page-aligned, sized to whole pages: the presenter imports them as
// host memory, VK_EXT_external_memory_host, rather than copying them).
#pragma once

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace bdex {

struct X11Window {
    Window id = 0;
    long pid = 0;
    std::string title, process;
    int x = 0, y = 0, width = 0, height = 0;  // root coordinates
};

class X11Capture {
public:
    using clock = std::chrono::steady_clock;
    static constexpr int kSlots = 3;

    struct Frame {
        int slot = -1;
        const uint8_t* data = nullptr;
        uint32_t width = 0, height = 0, stride = 0;  // stride in bytes (BGRX, 4 bytes a pixel)
        uint32_t generation = 0;   // the segments' generation (changes on resize)
        clock::time_point damagedAt;
    };

    explicit X11Capture(Display* dpy) : dpy_(dpy) {}
    ~X11Capture() { stop(); }
    X11Capture(const X11Capture&) = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    // Top-level application windows (those with WM_STATE, or mapped children
    // of the root without a window manager), with their pid and title.
    std::vector<X11Window> list();
    // The window named by id, pid or a part of its title (case-insensitive);
    // id 0 / pid 0 / empty title are not criteria.
    bool find(X11Window& out, Window id, long pid, const std::string& title);
    // Interactive: the next window clicked (crosshair cursor).
    bool pick(X11Window& out);

    bool start(const X11Window& w);
    void stop();

    // Waits up to `timeoutMs` for X events and handles them. Returns true
    // when the window's content changed since the last grab().
    bool pump(int timeoutMs);
    bool damaged() const { return damaged_; }
    bool gone() const { return gone_; }
    bool visible() const { return mapped_; }
    // The window's root rectangle changed (move or resize) since the last call.
    bool takeMoved() { const bool m = moved_; moved_ = false; return m; }
    const X11Window& window() const { return win_; }

    // Reads the window's current content into ring slot `slot`.
    bool grab(int slot, Frame& out);
    // The segments of the current generation (for the presenter's import).
    void* segment(int slot) const { return slots_[slot].info.shmaddr; }
    size_t segmentSize(int slot) const { return slots_[slot].size; }
    uint32_t generation() const { return generation_; }

    // Capture rate (damage events), an EMA.
    double fps() const { return fps_; }

private:
    struct Slot {
        XShmSegmentInfo info{};
        XImage* image = nullptr;
        size_t size = 0;
    };
    bool createSlots();
    void destroySlots();
    bool namePixmap();
    void updateGeometry();

    Display* dpy_;
    X11Window win_;
    Pixmap pixmap_ = 0;
    unsigned long damage_ = 0;
    int damageEvent_ = 0, damageError_ = 0;
    Slot slots_[kSlots];
    uint32_t generation_ = 0;
    uint32_t pixW_ = 0, pixH_ = 0;
    bool damaged_ = false, gone_ = false, mapped_ = true, moved_ = false, started_ = false;
    clock::time_point damagedAt_{}, lastDamage_{};
    double fps_ = 0.0;
};

// A process's name as the layer's [sections] match it: the .exe of a Wine /
// Proton command line, else the executable's base name.
std::string processNameOf(long pid);

} // namespace bdex
