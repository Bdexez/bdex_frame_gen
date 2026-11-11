#include "recorder.h"
#include "framegen.h"
#include "log.h"
#include "vk_util.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace bdex {

namespace {
// Six frames in flight: the writer (about 10 ms for a 1080p frame) keeps up
// with 60 fps, and a slow disk write is absorbed.
constexpr int kBuffers = 6;
} // namespace

FrameRecorder::FrameRecorder(DeviceData& dev, std::string dir, int frames)
    : dev_(dev), dir_(std::move(dir)), total_(frames), left_(frames), buffers_(kBuffers),
      scale_(uint32_t(std::max(0, dev.config.recordScale))) {
    writer_ = std::thread([this] { writerLoop(); });
}

FrameRecorder::~FrameRecorder() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    writer_.join();
    for (auto& b : buffers_) {
        if (b.mapped) dev_.vt.UnmapMemory(dev_.device, b.buf.memory);
        dev_.destroyBuffer(b.buf);
        if (b.depthMapped) dev_.vt.UnmapMemory(dev_.device, b.depth.memory);
        dev_.destroyBuffer(b.depth);
    }
}

VkBuffer FrameRecorder::next(VkExtent2D extent, const OutputEncoding& encoding, const SlotDone& done) {
    if (left_ <= 0) return VK_NULL_HANDLE;
    int idx = -1;
    auto takeFree = [&] {  // under mutex_
        for (int i = 0; i < kBuffers && idx < 0; ++i)
            if (!buffers_[size_t(i)].busy) idx = i;
        if (idx >= 0) buffers_[size_t(idx)].busy = true;
        return failed_ || idx >= 0;
    };
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (inFlight_.empty()) cv_.wait(lock, takeFree);  // all with the writer
            else takeFree();
            if (failed_) {
                if (idx >= 0) buffers_[size_t(idx)].busy = false;
                left_ = 0;
                return VK_NULL_HANDLE;
            }
            if (idx >= 0) break;
        }
        // Every buffer waits for the GPU: the oldest frame's submission.
        done(inFlight_.front().slot, inFlight_.front().generation, true);
        pump(done);
    }
    Buffer& b = buffers_[size_t(idx)];
    const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * (encoding.is64 ? 8 : 4);
    if (b.size != bytes) {
        try {
            if (b.mapped) dev_.vt.UnmapMemory(dev_.device, b.buf.memory);
            b.mapped = nullptr;
            dev_.destroyBuffer(b.buf);
            b.buf = dev_.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
            VK_CHECK(dev_.vt.MapMemory(dev_.device, b.buf.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
            b.size = bytes;
        } catch (const std::exception& e) {
            BDEX_WARN("recording: no readback buffer (%s) — stopped", e.what());
            std::lock_guard<std::mutex> lock(mutex_);
            b.busy = false;
            b.size = 0;
            left_ = 0;
            return VK_NULL_HANDLE;
        }
    }
    current_ = idx;
    pendingFrame_ = Frame{};
    pendingFrame_.buffer = idx;
    pendingFrame_.extent = extent;
    pendingFrame_.encoding = encoding;
    return b.buf.buffer;
}

VkBuffer FrameRecorder::depthBuffer(VkExtent2D extent, uint32_t texelBytes) {
    if (current_ < 0 || (texelBytes != 2 && texelBytes != 4)) return VK_NULL_HANDLE;
    Buffer& b = buffers_[size_t(current_)];  // (ours until the writer is done with the frame)
    const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * texelBytes;
    if (b.depthSize != bytes) {
        try {
            if (b.depthMapped) dev_.vt.UnmapMemory(dev_.device, b.depth.memory);
            b.depthMapped = nullptr;
            b.depthSize = 0;
            dev_.destroyBuffer(b.depth);
            b.depth = dev_.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
            VK_CHECK(dev_.vt.MapMemory(dev_.device, b.depth.memory, 0, VK_WHOLE_SIZE, 0, &b.depthMapped));
            b.depthSize = bytes;
        } catch (const std::exception& e) {
            BDEX_WARN("recording: no buffer for the depth (%s) — frames only", e.what());
            return VK_NULL_HANDLE;
        }
    }
    pendingFrame_.depthTexel = texelBytes;
    return b.depth.buffer;
}

void FrameRecorder::queued(uint32_t slot, uint64_t generation, double gameFps, double presentMs) {
    if (current_ < 0) return;
    pendingFrame_.slot = slot;
    pendingFrame_.generation = generation;
    pendingFrame_.index = copied_++;
    pendingFrame_.gameFps = gameFps;
    pendingFrame_.presentMs = presentMs;
    inFlight_.push_back(pendingFrame_);
    current_ = -1;
    --left_;
}

void FrameRecorder::pump(const SlotDone& done) {
    bool any = false;
    while (!inFlight_.empty() && done(inFlight_.front().slot, inFlight_.front().generation, false)) {
        std::lock_guard<std::mutex> lock(mutex_);
        toWrite_.push_back(inFlight_.front());
        inFlight_.pop_front();
        any = true;
    }
    if (any) cv_.notify_all();
}

void FrameRecorder::finish(const SlotDone& done) {
    if (current_ >= 0) {  // handed out, never submitted
        std::lock_guard<std::mutex> lock(mutex_);
        buffers_[size_t(current_)].busy = false;
        current_ = -1;
    }
    while (!inFlight_.empty()) {
        done(inFlight_.front().slot, inFlight_.front().generation, true);
        pump(done);
    }
    const int copied = copied_;
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return failed_ || written_ == copied; });
}

void FrameRecorder::writerLoop() {
    std::ofstream timestamps(dir_ + "/timestamps.csv");
    timestamps << "frame,present_ms,interval_ms\n" << std::fixed << std::setprecision(3);
    double previousMs = 0.0;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [&] { return stop_ || !toWrite_.empty(); });
        if (toWrite_.empty()) return;  // stop_
        const Frame f = toWrite_.front();
        toWrite_.pop_front();
        const void* pixels = buffers_[size_t(f.buffer)].mapped;
        const void* depth = f.depthTexel ? buffers_[size_t(f.buffer)].depthMapped : nullptr;
        const bool failed = failed_;
        lock.unlock();
        // record_scale: 0 = about 960 pixels wide (a 1440p game at a third:
        // its HUD's thin strokes then never stay exactly still), else 1/N.
        const uint32_t ds = scale_ > 0 ? scale_ : std::max(1u, (f.extent.width + 959) / 960);
        char name[32];
        snprintf(name, sizeof(name), "/f%04d.ppm", f.index);
        bool ok = !failed && FrameGen::writePpm(dir_ + name, pixels, f.extent, f.encoding, ds);
        if (ok && depth) {
            // (the directory at the first depth: none in a recording without)
            if (depthWritten_ == 0) {
                std::error_code ec;
                std::filesystem::create_directories(dir_ + "/depth", ec);
            }
            snprintf(name, sizeof(name), "/depth/f%04d.f32", f.index);
            if (FrameGen::writeDepth(dir_ + name, depth, f.extent, f.depthTexel, ds)) ++depthWritten_;
            snprintf(name, sizeof(name), "/f%04d.ppm", f.index);
        }
        if (ok) {
            timestamps << (name + 1) << ',' << f.presentMs << ','
                       << (f.index == 0 ? 0.0 : f.presentMs - previousMs) << '\n';
            previousMs = f.presentMs;
            if (f.index + 1 == total_) timestamps.flush();  // visible before DONE, which may move the directory
            ok = bool(timestamps);
        }
        lock.lock();
        buffers_[size_t(f.buffer)].busy = false;
        if (!ok) {
            if (!failed_) BDEX_WARN("recording: could not write frame/timestamps in %s — stopped", dir_.c_str());
            failed_ = true;
        } else {
            ++written_;
            if (f.index + 1 == total_) {
                std::ofstream done(dir_ + "/DONE");
                done << "frames=" << written_ << "\nwidth=" << f.extent.width / ds << "\nheight=" << f.extent.height / ds
                     << "\ngame_fps=" << f.gameFps << "\n";
                if (depthWritten_) done << "depth=" << depthWritten_ << "\n";
                BDEX_INFO("recording done: %d frames in %s (%d with the game's depth)", written_, dir_.c_str(), depthWritten_);
            }
        }
        cv_.notify_all();
    }
}

} // namespace bdex
