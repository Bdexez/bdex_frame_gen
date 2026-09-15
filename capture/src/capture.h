// Windows.Graphics.Capture feed for the capture PoC: a dedicated thread pumps
// the frame pool and copies each frame into a ring of D3D11 textures shared
// with the Vulkan side through NT handles + keyed mutexes.
//
// Keyed-mutex protocol, per ring slot: the capture thread copies under
// AcquireSync(k)/ReleaseSync(1) and the Vulkan submit acquires key 1 and
// releases key 0. `k` is normally 0; it is 1 when the previous frame in the
// slot was dropped before Vulkan ever acquired it (then the key is still the 1
// we released it with). Capture tracks `k` per slot from recycle() so the two
// sides can never dead-lock on a stale key.
//
// The NT handles stay owned by this class: importing one into Vulkan adds a
// reference to the payload (VK_KHR_external_memory_win32), it does not take
// ownership. They are closed when the ring that created them is dropped.
#pragma once

#include <windows.h>

#include <cstdint>

namespace bdex {

class Capture {
public:
    struct Frame {
        HANDLE handle = nullptr;    // NT handle of the ring texture (not owned)
        uint32_t width = 0, height = 0;
        uint32_t slot = 0;          // ring index; recycle() hands it back
        uint32_t generation = 0;    // bumped when the ring is recreated (resize)
    };

    static constexpr uint32_t kRingSize = 3;

    Capture() = default;
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Starts capturing `target` on a dedicated thread. False (with a logged
    // reason) if WGC refused: unsupported window, capture denied, no D3D11.
    bool start(HWND target);
    void stop();

    // Main thread: waits up to `timeoutMs` and returns the newest captured
    // frame; frames that arrived behind it are dropped (their slots go back
    // to the ring — the Vulkan side never saw them).
    bool pop(Frame& frame, uint32_t timeoutMs);

    // Hands a popped slot back to the capture thread. `consumed` says whether
    // a Vulkan submit acquired (key 1) and released (key 0) the slot's mutex.
    void recycle(const Frame& frame, bool consumed);

    // Drops the textures of a ring generation whose imports the Vulkan side
    // has freed (called once per freed generation).
    void releaseGraveyard(uint32_t generation);

    // Captured frames per second, measured over the last statistics window.
    float captureFps() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace bdex
