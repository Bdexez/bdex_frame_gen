// The worker thread of a virtual swapchain (swapchain.h): runs the jobs the
// game's present() hands over — synthesis submission, copies, staging and
// the paced presents — plus the display-feedback thread and its clock. The
// game-thread side (creation, acquire, present) is swapchain.cpp.
#include "swapchain.h"
#include "frame_order.h"
#include "pacing.h"
#include "platform.h"
#include "thread_priority.h"
#include "gamescope.h"
#include "online_tune.h"
#include "status.h"
#include "log.h"
#include "vk_util.h"

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace bdex {

// ---------------------------------------------------------------------------
// Worker side
// ---------------------------------------------------------------------------

void VirtualSwapchain::workerLoop() {
#ifdef __linux__
    // (precise_wake: no 50 us of timer slack on this thread's sleeps)
    if (cfg_.preciseWake) prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
#endif
    // precise_wake on Windows: the paced sleeps and waits at 1 ms rather than
    // the default 15.6 ms timer (an interpolated frame 4 ms ahead of its slot
    // would otherwise be shown up to 15 ms late), for as long as this worker runs.
    struct FineTimer {
        bool on;
        explicit FineTimer(bool o) : on(o) { if (on) platformFineTimer(true); }
        ~FineTimer() { if (on) platformFineTimer(false); }
    } fineTimer(cfg_.preciseWake);
    // thread_priority: the presents' moments matter more than any other
    // thread's work of the game (thread_priority.h).
    if (cfg_.threadPriority) {
        const std::string got = raiseThreadPriority();
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            BDEX_INFO("presentation thread priority: %s", got.empty() ? "normal (not permitted)" : got.c_str());
    }
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> wlock(workerMutex_);
            workerCv_.wait(wlock, [this] { return !jobs_.empty() || stop_; });
            if (jobs_.empty()) return;  // stop requested and nothing left
            job = jobs_.front();
            jobs_.pop_front();
            running_ = true;
        }
        jobStartDelayMs_ = std::chrono::duration<double, std::milli>(clock::now() - job.queuedAt).count();
        runJob(job);
        pollGamescope();  // before going idle: followSurface replaces real_ only then
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            running_ = false;
        }
        idleCv_.notify_all();
    }
}

void VirtualSwapchain::runJob(const Job& job) {
    bool submitted = false;
    auto markSubmitted = [&] {
        if (submitted) return;
        submitted = true;
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            --unsubmitted_;
        }
        idleCv_.notify_all();
    };
    // async_compute: every semaphore a copy would have waited must be waited
    // even when that frame is dropped or fails before its copy, or it stays
    // signalled into its next use.
    std::vector<bool> copied(static_cast<size_t>(job.frames), false);
    auto consumeUnused = [&] {
        if (job.asyncSlot < 0) return;
        const FrameSlot& synth = slots_[static_cast<size_t>(job.asyncSlot)];
        std::vector<VkSemaphore> left;
        for (int k = 0; k < job.frames; ++k) {
            if (copied[size_t(k)]) continue;
            copied[size_t(k)] = true;
            const bool realK = k == job.realIndex;
            left.push_back((job.synthSlot >= 0 && realK) ? synth.realIn : synth.synthDone[k]);
        }
        if (!left.empty()) consumeSemaphores(left.data(), static_cast<uint32_t>(left.size()));
    };
    auto fail = [&](VkResult r) {
        consumeUnused();
        markSubmitted();
        // A genuine out-of-date on the real swapchain means the window
        // changed (e.g. a resize). On Wayland, drop the reduced-extent flag
        // so the app's recreation re-learns the new true window size before
        // we reduce again (see WaylandSurfaceState).
        if (r == VK_ERROR_OUT_OF_DATE_KHR && surface_ != VK_NULL_HANDLE) {
            std::lock_guard<std::mutex> lk(dev_.inst->surfMutex);
            auto it = dev_.inst->waylandSurfaces.find(surface_);
            if (it != dev_.inst->waylandSurfaces.end()) it->second.reduce = false;
        }
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            pendingResult_ = r;
            lazyOutputs_ = false;
            acquireCv_.notify_all();
        }
        idleCv_.notify_all();
    };
    // With FIFO the presentation engine paces the frames; otherwise they are
    // spread over the game's frame interval.
    const bool paced = job.pacing && !job.fifo && job.frames > 1;

    if (job.realIndex > 0 || job.frames == 1) {
        // Interpolation, hybrid (and a lone real frame): every frame is
        // copied at once, in display order, right behind the synthesis on the
        // queue — into its swapchain image for as many as the surface lets us
        // hold acquired at once (acquireAhead_), into a staging image for the
        // rest (a surface allowing few images, high multipliers; hybrid's
        // predicted frames). The application thread only waits for these
        // copies to be submitted before it records the next synthesis (which
        // rewrites what they read), so the pacing below never holds the game
        // back; a staged frame goes to its swapchain image when presented.
        // Hybrid: the predicted frames after the real one are presented at
        // their times unless the next present's first frame is ready first.
        // (Past the staging images there are — a live change not yet
        // applied — frames are copied at their times, the game waiting.)
        const int upToReal = job.frames == 1 ? 1 : job.realIndex + 1;
        const int eager = std::min(upToReal, acquireAhead_);
        const int staged = std::min(job.frames, eager + job.staging);  // frames [eager, staged) go through staging
        std::vector<Copied> out(static_cast<size_t>(eager));
        int n = 0;
        VkResult err = VK_SUCCESS;
        for (int i = 0; i < eager; ++i) {
            const VkResult r = submitCopy(job, i, out[size_t(i)]);
            if (r < 0) { err = r; break; }
            copied[size_t(i)] = true;
            ++n;
        }
        for (int i = eager; i < staged && err >= 0; ++i) {
            const VkResult r = submitStage(job, i, static_cast<uint32_t>(i - eager));
            if (r < 0) err = r;
            else copied[size_t(i)] = true;
        }
        if (err < 0 || staged == job.frames) {
            consumeUnused();
            markSubmitted();
        }
        auto staging = [&](int i) { return i < staged ? i - eager : -1; };
        // The frames are timed from when the GPU finished the synthesis (the
        // copies follow within a fraction of a millisecond), not from the
        // game's present call: the game's rendering and the synthesis may end
        // long after that call (a GPU-bound game runs a frame ahead of the
        // GPU), and frames timed from it became ready together — the
        // generated one replaced before being displayed, x2 announced and x1
        // shown. Evenly spaced too when the GPU keeps up: the generated frame
        // went out when ready, the real one half a frame after the call.
        clock::time_point origin = clock::now();
        if (paced && n > 0) waitGpu(job.slot >= 0 ? slots_[size_t(job.slot)].fence : out[0].fence, false, origin);
        // The job's first frame went out a refresh or more after the first
        // one it could make once ready (paceTarget aims 0.3 refresh ahead):
        // the jobs before still held the refreshes — a lag that producing at
        // the display's rate never undoes (measured: a refresh more from
        // present to screen, for good, after a gamescope frame limit). Eight
        // jobs in a row (a lag that stays), and a present shows its real
        // frame alone (present()), which gives the refresh back.
        auto noteLateness = [&](int i) {
            if (!paced || i != 0) return;
            pacing::Grid g;
            if (lastLandMs_ <= 0.0 || !displayGrid(g.lastMs, g.periodMs)) return;
            g.valid = true;
            // (Or the job itself began a refresh late: the worker was still
            // presenting the job before.)
            if (pacing::jobLate(msOf(origin), lastLandMs_, jobStartDelayMs_, g))
                lateJobs_.fetch_add(1, std::memory_order_relaxed);
            else
                lateJobs_.store(0, std::memory_order_relaxed);
        };
        for (int i = 0; i < n; ++i) {
            if (paced) sleepUntilPrecise(paceTarget(job, origin, i));
            const VkResult r = presentCopied(job, out[size_t(i)]);
            if (r < 0 && err == VK_SUCCESS) err = r;
            noteLateness(i);
        }
        if (err < 0) {
            fail(err);
            return;
        }
        for (int i = eager; i < upToReal; ++i) {
            if (paced) sleepUntilPrecise(paceTarget(job, origin, i));
            Copied c;
            VkResult r = submitCopy(job, i, c, staging(i));
            if (r < 0) {
                fail(r);
                return;
            }
            if (i >= staged) {
                copied[size_t(i)] = true;
                if (i == job.frames - 1) markSubmitted();
            }
            r = presentCopied(job, c);
            if (r < 0) {
                fail(r);
                return;
            }
            noteLateness(i);
        }
        uint64_t dropped = 0;
        bool cancelled = false;
        for (int i = upToReal; i < job.frames; ++i) {
            if (!cancelled) cancelled = paced ? !waitUntilOrCancelled(paceTarget(job, origin, i)) : predictedCancelled();
            if (cancelled) {
                ++dropped;
                continue;
            }
            Copied c;
            VkResult r = submitCopy(job, i, c, staging(i));
            if (r < 0) {
                fail(r);
                return;
            }
            if (i >= staged) {
                copied[size_t(i)] = true;
                if (i == job.frames - 1) markSubmitted();
            }
            r = presentCopied(job, c);
            if (r < 0) {
                fail(r);
                return;
            }
        }
        if (dropped) statsSkipped_.fetch_add(dropped, std::memory_order_relaxed);
        consumeUnused();
        markSubmitted();
        return;
    }

    // Extrapolation: the real frame goes out at once...
    Copied real;
    VkResult r = submitCopy(job, 0, real);
    if (r >= 0) {
        copied[0] = true;
        if (job.frames == 1) markSubmitted();
        r = presentCopied(job, real);
    }
    // ...then the synthesis the application thread left to us (Job::synthSlot):
    // after the real frame's copy on the queue, whatever the present's
    // outcome, since the slot's fence must signal for the application thread.
    if (job.synthSlot >= 0) {
        const VkResult sr = submitDeferredSynthesis(job);
        if (r >= 0 && sr < 0) r = sr;
    }
    // The predicted frames. Synthesised by the application thread (the real
    // frame is packed: upscaling, HUD...), the next present rewrites them
    // before their time: staged now. Deferred, the next synthesis is this
    // thread's own, submitted after them: they are copied straight from the
    // outputs at their times (lazyOutputs_ tells the application thread,
    // should its next synthesis not be deferred).
    const bool lazy = job.synthSlot >= 0;
    const int staged = lazy ? 1 : std::min(job.frames, 1 + job.staging);  // frames [1, staged) go through staging
    for (int i = 1; i < staged && r >= 0; ++i) {
        r = submitStage(job, i, static_cast<uint32_t>(i - 1));
        if (r >= 0) copied[size_t(i)] = true;
    }
    if (r < 0) {
        fail(r);
        return;
    }
    if (lazy && job.frames > 1) {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        lazyOutputs_ = true;
    }
    if (lazy || staged == job.frames) markSubmitted();
    // At their times after the real frame is ready on the GPU — deferred,
    // not before the synthesis is (two late frames presented together would
    // show only the second). They are obsolete once the next present's
    // real frame is ready: from then on, the ones still waiting are dropped.
    clock::time_point origin = clock::now();
    bool cancelled = job.frames > 1 && (paced ? !waitGpu(real.fence, true, origin) : predictedCancelled());
    if (!cancelled && lazy && job.frames > 1 && paced) {
        clock::time_point synthesised;
        cancelled = !waitGpu(slots_[size_t(job.synthSlot)].fence, true, synthesised);
    }
    uint64_t dropped = 0;
    for (int i = 1; i < job.frames; ++i) {
        if (!cancelled) cancelled = paced ? !waitUntilOrCancelled(paceTarget(job, origin, i)) : predictedCancelled();
        if (cancelled) {
            ++dropped;
            continue;
        }
        Copied c;
        r = submitCopy(job, i, c, i < staged ? i - 1 : -1);
        if (r < 0) {
            fail(r);
            return;
        }
        if (i >= staged) {
            copied[size_t(i)] = true;
            if (i == job.frames - 1) markSubmitted();
        }
        r = presentCopied(job, c);
        if (r < 0) {
            fail(r);
            return;
        }
    }
    if (dropped) statsSkipped_.fetch_add(dropped, std::memory_order_relaxed);
    consumeUnused();
    markSubmitted();
    if (lazy) {
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            lazyOutputs_ = false;
        }
        idleCv_.notify_all();
    }
}

// The predicted frames still waiting are obsolete once the next present's
// first frame is ready on the GPU (it goes out then), when the application
// thread needs the images they read (cancelPredicted_), or on stop. Not as
// soon as the next present is *called*: its rendering may end a frame later
// (a GPU-bound game), and the predicted frames are ready before it — dropping
// them then showed a GPU-bound game at x1 in extrapolation and hybrid.
} // namespace bdex
