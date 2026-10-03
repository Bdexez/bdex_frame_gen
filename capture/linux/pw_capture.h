// The game's frames on Wayland (a native Wayland window — Wine's Wayland
// driver, a native game — which X11 cannot read): the desktop's screen-cast
// portal (org.freedesktop.portal.ScreenCast: the compositor asks the user
// which window to share, once — the choice is kept, restore_token) hands over
// a PipeWire stream of that window; its buffers are copied into a ring of
// page-aligned frames, the same as X11Capture's (the presenter imports them
// as host memory). The overlay cannot be placed over a Wayland window (its
// position is the compositor's): it covers the screen area given (--rect),
// by default the whole X screen — a fullscreen game's.
#pragma once

#include "x11_capture.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct spa_pod;
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;

namespace bdex {

class PwCapture {
public:
    using clock = std::chrono::steady_clock;
    using Frame = X11Capture::Frame;
    static constexpr int kSlots = X11Capture::kSlots;

    PwCapture() = default;
    ~PwCapture() { stop(); }
    PwCapture(const PwCapture&) = delete;
    PwCapture& operator=(const PwCapture&) = delete;

    // The portal session (the compositor's picker, unless a kept choice
    // restores it), then the stream. `monitor`: share a whole screen instead
    // of a window (then show the frames on another screen: the overlay would
    // be captured too). `area`: where the overlay goes.
    bool start(bool monitor, const X11Window& area);
    void stop();

    bool pump(int timeoutMs);
    bool damaged() const { return damaged_; }
    bool gone() const { return gone_.load(); }
    bool visible() const { return true; }
    bool takeMoved() { const bool m = moved_; moved_ = false; return m; }
    const X11Window& window() const { return area_; }

    bool grab(int slot, Frame& out);
    uint32_t generation() const { return generation_; }
    double fps() const { return fps_; }
    // What was shared, as the portal said ("" unknown).
    const std::string& sourceName() const { return source_; }

private:
    bool portal(bool monitor, int& fd, uint32_t& node);
    bool connect(int fd, uint32_t node);
    static void onProcess(void* self);
    static void onParamChanged(void* self, uint32_t id, const struct spa_pod* param);
    static void onStateChanged(void* self, int old, int state, const char* error);

    X11Window area_;
    std::string source_;
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    void* listener_ = nullptr;  // spa_hook (heap: its layout stays in the .cpp)
    // The stream's format and its latest frame (the PipeWire thread writes it).
    std::mutex mutex_;
    std::vector<uint8_t> latest_;
    uint32_t width_ = 0, height_ = 0, stride_ = 0;  // the stream's size; the latest frame's (cropped) size:
    uint32_t frameW_ = 0, frameH_ = 0;
    bool swapRB_ = false;  // RGBx / RGBA: converted to the BGRX the presenter takes
    uint64_t seq_ = 0, seqGrabbed_ = 0;
    clock::time_point latestAt_{};
    // The ring the presenter reads (page-aligned, whole pages).
    struct Slot {
        uint8_t* data = nullptr;
        size_t size = 0;
    };
    Slot slots_[kSlots];
    uint32_t generation_ = 0, slotW_ = 0, slotH_ = 0;
    bool damaged_ = false, moved_ = false;
    std::atomic<bool> gone_{false}, stopping_{false};
    clock::time_point lastFrame_{};
    double fps_ = 0.0;
};

} // namespace bdex
