#pragma once
#include "device.h"
#include "format.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bdex {

// A recording for the evaluation corpus (the control panel's « Enregistrer »,
// DeviceData::recordDir): the game's next N real frames as fNNNN.ppm (at most
// 960 pixels wide), timestamps.csv (the game's present times), then DONE.
// The GPU copies each frame into one of a few
// host buffers; a thread of its own converts and writes them. Written in
// present() — a fence wait, then the conversion and the file, on the game's
// thread — a recording held a game at 3 fps, so its frames were far apart
// in game time and the motion between them unlike the game's.
class FrameRecorder {
public:
    // done(slot, generation): whether that submission finished on the GPU
    // (may block when `wait`).
    using SlotDone = std::function<bool(uint32_t slot, uint64_t generation, bool wait)>;

    FrameRecorder(DeviceData& dev, std::string dir, int frames);
    ~FrameRecorder();  // pending frames are dropped: call finish() first

    bool wantsFrame() const { return left_ > 0; }
    // The buffer the next frame (`extent`, `encoding`) is copied into; waits
    // for the writer when every buffer is in use. VK_NULL_HANDLE: stopped.
    VkBuffer next(VkExtent2D extent, const OutputEncoding& encoding, const SlotDone& done);
    // That copy went into submission (slot, generation).
    void queued(uint32_t slot, uint64_t generation, double gameFps, double presentMs);
    // Frames whose submission finished go to the writer.
    void pump(const SlotDone& done);
    // Every queued frame written (waits for the GPU and the writer).
    void finish(const SlotDone& done);

private:
    struct Buffer {
        AllocatedBuffer buf;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
        bool busy = false;  // between next() and the writer
    };
    struct Frame {
        int buffer = -1;
        uint32_t slot = 0;
        uint64_t generation = 0;
        int index = 0;
        VkExtent2D extent{};
        OutputEncoding encoding;
        double gameFps = 0.0;
        double presentMs = 0.0;  // steady clock, sampled on the game thread, not when the file is written
    };
    void writerLoop();

    DeviceData& dev_;
    const std::string dir_;
    const int total_;
    int left_;            // frames still to copy (game thread)
    int copied_ = 0;
    int current_ = -1;    // buffer handed out by next()
    Frame pendingFrame_;  // its frame, until queued()
    std::deque<Frame> inFlight_;  // queued, GPU not known done (game thread)

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Buffer> buffers_;  // `busy` under mutex_
    const uint32_t scale_;         // record_scale: 1/N of the size written, 0 = about 960 pixels wide
    std::deque<Frame> toWrite_;
    int written_ = 0;
    bool failed_ = false, stop_ = false;
    std::thread writer_;
};

} // namespace bdex
