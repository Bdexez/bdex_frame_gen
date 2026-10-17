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
    if (!paced || !job.smoothCatchup) planSlip_.reset();

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
        // (Split: from when the first generated frame is ready, FrameSlot::cmd2.)
        if (paced && n > 0) {
            const VkFence first = job.firstReady ? job.firstReady : job.slot >= 0 ? slots_[size_t(job.slot)].fence : out[0].fence;
            // pace_plan's estimate of when a job's first frame is ready, quick
            // to rise, slow to fall — measured only when the worker got there
            // first: a fence already signalled says only "before now" (the
            // worker still presenting the job before), and counting that
            // pushed the plan later and later (measured: 9 ms for 4.6).
            const bool early = dev_.vt.GetFenceStatus(dev_.device, first) == VK_NOT_READY;
            waitGpu(first, false, origin);
            if (job.plan) {
                const double lag = std::chrono::duration<double, std::milli>(origin - job.start).count();
                const double est = readyLagMs_.load(std::memory_order_relaxed);
                if (lag >= 0.0 && lag < 200.0 && (early || lag < est))
                    readyLagMs_.store(est + (lag > est ? 0.5 : 0.05) * (lag - est), std::memory_order_relaxed);
            }
        }
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
            if (lastLandMs_ <= 0.0 || !displayGrid(g.lastMs, g.periodMs)) { planSlip_.reset(); return; }
            g.valid = true;
            const bool slipped = planSlip_.update(job.planned[0], lastLandMs_, g.periodMs, job.smoothCatchup);
            const double readyMs = std::chrono::duration<double, std::milli>(origin - job.start).count();
            if (slipped && readyMs < 0.5 * job.intervalMs) {
                smoothCatchupRequested_.store(true, std::memory_order_relaxed);
                BDEX_DBG("presentation plan slipped: first output %.3f ms late, ready lag %.3f ms",
                         lastLandMs_ - job.planned[0], readyMs);
            }
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
bool VirtualSwapchain::predictedCancelled() {
    VkFence next = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        if (cancelPredicted_ || stop_) return true;
        if (jobs_.empty()) return false;
        next = jobs_.front().ready;
    }
    return next == VK_NULL_HANDLE || dev_.vt.GetFenceStatus(dev_.device, next) == VK_SUCCESS;
}

// Until `target`, or false as soon as predictedCancelled: sleeps until a
// newer present is queued, then watches its ready fence. (Its slot's fence
// is not recycled before the worker reaches that job: the application thread
// recycles a slot only presents later, each waiting for the job before.)
// The worker's sleep until a present's moment, and how late it woke (for
// the statistics; only sleeps whose moment was still ahead). precise_wake
// drops the kernel's timer slack for this thread (workerLoop: 50 us by
// default). Measured: spinning the last half millisecond (sleep, then
// yield) won nothing on an idle machine (141 -> 120 us late) and lost
// frames on a busy one (1 ms late, 114.7 of 119 shown): the yield hands the
// CPU to the load. Not done.
void VirtualSwapchain::sleepUntilPrecise(clock::time_point target) {
    if (target <= clock::now()) return;
    std::this_thread::sleep_until(target);
    const int64_t late = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - target).count();
    statsWakeLateUs_.fetch_add(uint64_t(std::max<int64_t>(late, 0)), std::memory_order_relaxed);
    statsWakes_.fetch_add(1, std::memory_order_relaxed);
}

bool VirtualSwapchain::waitUntilOrCancelled(clock::time_point target) {
    VkFence next = VK_NULL_HANDLE;
    {
        std::unique_lock<std::mutex> wlock(workerMutex_);
        workerCv_.wait_until(wlock, target, [this] { return cancelPredicted_ || stop_ || !jobs_.empty(); });
        if (cancelPredicted_ || stop_) return false;
        if (jobs_.empty()) return true;  // the target, nothing newer
        next = jobs_.front().ready;
    }
    if (next == VK_NULL_HANDLE) return false;
    for (;;) {
        const auto now = clock::now();
        if (now >= target) return dev_.vt.GetFenceStatus(dev_.device, next) != VK_SUCCESS;
        // Short waits, so that cancelPredicted_ is honoured promptly.
        const int64_t left = std::chrono::duration_cast<std::chrono::nanoseconds>(target - now).count();
        const VkResult r = dev_.vt.WaitForFences(dev_.device, 1, &next, VK_TRUE, uint64_t(std::min<int64_t>(left, 1'000'000)));
        if (r != VK_TIMEOUT) return false;  // ready (or lost): obsolete
        std::lock_guard<std::mutex> wlock(workerMutex_);
        if (cancelPredicted_ || stop_) return false;
    }
}

// Bounded: a frame the GPU has not finished within 250 ms is paced from then
// (a hang is not ours to diagnose, and the pacing must not stall on it).
bool VirtualSwapchain::waitGpu(VkFence fence, bool cancellable, clock::time_point& doneAt) {
    const auto limit = clock::now() + std::chrono::milliseconds(250);
    for (;;) {
        // Short waits: vkWaitForFences returns as soon as the fence signals,
        // so doneAt stays within the driver's wake-up time of the GPU's end.
        const VkResult r = dev_.vt.WaitForFences(dev_.device, 1, &fence, VK_TRUE, 500'000);
        if (r != VK_TIMEOUT) break;
        if (cancellable && predictedCancelled()) return false;
        if (clock::now() >= limit) break;
    }
    doneAt = clock::now();
    return true;
}

// Frame k goes out k/m of the frame interval after the origin (when the GPU
// finished the job's first frame), never less than half a slot after the
// previous present (a job whose GPU work ended early must not land on the
// previous frame's refresh), and, with a known refresh grid, well before the
// refresh it should show on (a present landing just after a refresh would
// slip a whole period) and on a refresh of its own.
VirtualSwapchain::clock::time_point VirtualSwapchain::paceTarget(const Job& job, clock::time_point origin, int k) {
    pacing::Grid g;
    g.valid = displayGrid(g.lastMs, g.periodMs, &g.lead);
    const double t = pacing::presentTargetMs(msOf(origin), job.intervalMs, job.frames, job.order, k, lastPresentMs_,
                                             lastLandMs_, g, job.plan && k < 8 ? job.planned[k] : 0.0);
    return clock::time_point(std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::milli>(t)));
}

// ---------------------------------------------------------------------------
// Present feedback
// ---------------------------------------------------------------------------

namespace {
// latency_log: one line per displayed frame, from every swapchain of the
// process into one file (opened once, appended under a lock).
void latencyLogLine(const std::string& path, double presentMs, double displayMs, bool real, double plannedMs) {
    static std::mutex mutex;
    static FILE* file = nullptr;
    static bool failed = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (!file && !failed) {
        file = fopen(path.c_str(), "w");
        failed = !file;
        if (failed) BDEX_WARN("latency_log: cannot write %s", path.c_str());
    }
    if (!file) return;
    fprintf(file, "%.3f %.3f %d %.3f\n", presentMs, displayMs, real ? 1 : 0, plannedMs);
    fflush(file);
}
} // namespace

// Waits for each presented id in turn. vkWaitForPresentKHR returns once that
// frame *or a later one* is displayed, so an id that completes right after
// its predecessor (well within a refresh) means the predecessor was replaced
// before being shown (mailbox), and the display time belongs to this one.
void VirtualSwapchain::feedbackLoop() {
    for (;;) {
        FbEntry entry;
        {
            std::unique_lock<std::mutex> lock(fbMutex_);
            fbCv_.wait(lock, [this] { return fbStop_ || (!fbPaused_ && !fbPending_.empty()); });
            if (fbStop_) return;
            entry = fbPending_.front();
            fbPending_.pop_front();
            fbBusy_ = true;
        }
        VkResult r = VK_TIMEOUT;
        // Short waits so a pause (swapchain re-creation) is honoured quickly;
        // a frame not shown within ~0.5 s (minimised window) is given up.
        for (int k = 0; k < 25 && r == VK_TIMEOUT && !fbPaused_; ++k)
            r = dev_.vt.WaitForPresentKHR(dev_.device, real_, entry.id, 20'000'000ull);
        double now = msOf(clock::now());
        // The driver's own display time (VK_EXT_present_timing): the moment
        // the frame became visible, not this thread's wake-up after it.
        if (r >= 0 && r != VK_TIMEOUT && ptActive_.load()) {
            double shown = 0.0;
            if (presentTimingMs(entry.id, shown) && shown <= now + 1.0 && now - shown < 250.0) now = shown;
        }
        bool quit = false;
        {
            std::lock_guard<std::mutex> lock(fbMutex_);
            fbBusy_ = false;
            fbDoneId_ = std::max(fbDoneId_, entry.id);
            if (r >= 0 && r != VK_TIMEOUT) {
                fbEverDisplayed_ = true;
                // Latency the player sees: the game's present call to the
                // moment its real frame is on screen (all queueing, the
                // layer's hold and the compositor included).
                if (!cfg_.latencyLog.empty() && !latencyFromX11_.load(std::memory_order_relaxed))
                    latencyLogLine(cfg_.latencyLog, entry.presentMs, now, entry.real, entry.plannedMs);
                const double latency = now - entry.presentMs;
                if (entry.real && latency >= 0.0 && latency < 1000.0 && !latencyFromX11_.load(std::memory_order_relaxed)) {
                    fbLatencyMs_ = fbLatencyMs_ < 0.0 ? latency : fbLatencyMs_ * 0.9 + latency * 0.1;
                    fbLatencySum_ += latency;
                    ++fbLatencyCount_;
                    const double frameLatency = now - entry.frameStartMs;
                    if (entry.frameStartMs > 0.0 && frameLatency >= 0.0 && frameLatency < 1000.0) {
                        fbFrameLatencySum_ += frameLatency;
                        ++fbFrameLatencyCount_;
                    }
                }
                if (fbLastMs_ > 0.0 && now - fbLastMs_ < 0.5) {
                    ++fbReplaced_;
                } else {
                    // virtual_grid: two frames shown a third of a period
                    // apart or less fell on one refresh, seen through
                    // display times too noisy for the 0.5 ms test above
                    // (an X11 surface on a Wayland compositor).
                    if (vgPeriodMs_ > 0.0 && fbLastMs_ > 0.0 && now - fbLastMs_ < 0.35 * vgPeriodMs_) ++fbClose_;
                    // (status held_per_s: a frame on screen a refresh more)
                    const double hint = fbPeriodHintMs_.load(std::memory_order_relaxed);
                    if (hint > 0.0 && fbLastMs_ > 0.0 && now - fbLastMs_ >= 1.5 * hint && now - fbLastMs_ < 200.0) ++fbHeld_;
                    ++fbDisplayed_;
                    displayClock_.add(now);
                    fbLastMs_ = now;
                }
            } else if (r == VK_TIMEOUT && !fbPaused_ && !fbEverDisplayed_ && ++fbTimeouts_ >= 30) {
                // Nothing ever reported displayed: the driver or compositor
                // does not implement present waits usefully here.
                BDEX_INFO("present feedback unavailable on this surface; disabled");
                feedback_ = false;
                quit = true;
            }
        }
        fbIdleCv_.notify_all();
        if (quit) return;
    }
}

void VirtualSwapchain::waitDisplayed(uint64_t id, double timeoutMs) {
    std::unique_lock<std::mutex> lock(fbMutex_);
    fbIdleCv_.wait_for(lock, std::chrono::duration<double, std::milli>(timeoutMs),
                       [&] { return fbDoneId_ >= id || fbPaused_ || fbStop_ || !feedback_; });
}

void VirtualSwapchain::pauseFeedback() {
    if (!fbThread_.joinable()) return;
    std::unique_lock<std::mutex> lock(fbMutex_);
    fbPaused_ = true;
    fbPending_.clear();
    fbIdleCv_.wait(lock, [this] { return !fbBusy_; });
}

void VirtualSwapchain::resumeFeedback() {
    if (!fbThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(fbMutex_);
        fbPaused_ = false;
        fbLastMs_ = 0.0;
        presentCounter_ = 0;  // ids restart with the new swapchain (the worker is idle here)
        fbDoneId_ = 0;
    }
    fbCv_.notify_all();
}

double VirtualSwapchain::displayLatencyMs() {
    if (!feedback_ && !latencyFromX11_.load(std::memory_order_relaxed)) return -1.0;
    std::lock_guard<std::mutex> lock(fbMutex_);
    return fbLatencyMs_;
}

double VirtualSwapchain::displayHz() {
    // Under gamescope, the cycle it reports: exact, at once, and with Steam's
    // framerate limit on, the limit — what the screen shows at most. (The
    // measured grid cannot tell a 30 Hz display from a 60 Hz one we present
    // to every other refresh: at x1 it stayed at 30 Hz once the limit was
    // lifted.)
    if (const double g = gsHz_.load(); g > 0.0) return g;
    const double x11 = x11Clock_ ? x11Clock_->refreshHz() : 0.0;
    std::lock_guard<std::mutex> lock(fbMutex_);
    const double measured = x11 > 0.0 ? 0.0 : displayClock_.refreshHz();  // (x11_clock first, as displayGrid)
    // Not measurable (no present feedback, or no refresh grid, e.g. KDE with
    // NVIDIA): the X server's (x11_clock), else the refresh rate the control
    // panel reads from the system.
    return measured > 0.0 ? measured : x11 > 0.0 ? x11 : cfg_.panelDisplayHz;
}

// Worker thread, after a job (real_ stays put until it goes idle): at most
// ten times a second, the refresh cycle gamescope's WSI layer holds for our
// swapchain (it pumps gamescope's events itself) and the limiter state.
void VirtualSwapchain::pollGamescope() {
    if (!dev_.gamescope || gsFailed_) return;
    // At once when the limiter state flips: the present that got the new
    // cycle is often the one the WSI layer failed with VK_ERROR_OUT_OF_DATE_KHR
    // to have the swapchain re-created, and the next swapchain never gets it.
    const bool limited = gamescopeLimiterActive();
    const auto now = clock::now();
    if (limited == gsLimited_.load() && now - gsPolledAt_ < std::chrono::milliseconds(100)) return;
    gsPolledAt_ = now;
    VkRefreshCycleDurationGOOGLE cycle{};
    if (dev_.vt.GetRefreshCycleDurationGOOGLE(dev_.device, real_, &cycle) != VK_SUCCESS || cycle.refreshDuration == 0) {
        gsFailed_ = true;
        gsHz_ = 0.0;
        gsLimited_ = false;
        BDEX_WARN("gamescope's WSI layer gave no refresh cycle for this swapchain: measuring the display instead");
        return;
    }
    uint64_t ns = cycle.refreshDuration;
    if (gsFirstCycle_ == 0) gsFirstCycle_ = ns;
    else if (ns != gsFirstCycle_) gsCycleChanged_ = true;
    {
        // A value of its own once gamescope sent one; until then, the
        // surface's last (DeviceData::gamescopeCycles).
        std::lock_guard<std::mutex> lock(dev_.gamescopeMutex);
        uint64_t& known = dev_.gamescopeCycles[statusKey(realInfo_.surface)];
        if (gsCycleChanged_ || known == 0) known = ns;
        else ns = known;
    }
    gsHz_ = 1e9 / double(ns);
    gsLimited_ = limited;
    if (limited) dev_.gamescopeLimitSeen.store(true, std::memory_order_relaxed);
}

bool VirtualSwapchain::displayGrid(double& lastMs, double& periodMs, double* lead) {
    if (lead) *lead = 0.3;
    // x11_clock first: the moments the X server handed the window's content
    // over (Xwayland: to the compositor), the refreshes a present must beat.
    // Before the present feedback's, which under Xwayland in immediate mode
    // times our own presents — once they are paced on a grid, they fit one
    // and that "grid" took over now and then (at another phase and the
    // display's lead: a job in ten planned a refresh off, all its frames).
    if (x11Clock_ && x11Clock_->grid(lastMs, periodMs)) {
        if (lead) *lead = pacing::kX11Lead;
        return true;
    }
    std::lock_guard<std::mutex> lock(fbMutex_);
    if (feedback_ && displayClock_.valid()) {
        lastMs = displayClock_.lastMs();
        periodMs = displayClock_.periodMs();
        return true;
    }
    if (vgPeriodMs_ <= 0.0) return false;
    lastMs = vgAnchorMs_;
    periodMs = vgPeriodMs_;
    return true;
}

// Submits the synthesis the application thread recorded but left to the
// worker (Job::synthSlot), right after the real frame went out.
VkResult VirtualSwapchain::submitDeferredSynthesis(const Job& job) {
    FrameSlot& s = slots_[static_cast<size_t>(job.synthSlot)];
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    // async_compute: after the game (synthIn) and the copies still reading
    // the outputs; signals the generated frames' presents (synthDone[1..]).
    std::vector<VkSemaphore> waits;
    std::vector<VkPipelineStageFlags> stages;
    VkQueue queue = dev_.queue;
    if (job.asyncSlot >= 0) {
        queue = dev_.computeQueue;
        waits.push_back(s.synthIn);
        waits.insert(waits.end(), job.synthWaits.begin(), job.synthWaits.end());
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);  // this job's real copy
            waits.insert(waits.end(), pendingCopyDone_.begin(), pendingCopyDone_.end());
            pendingCopyDone_.clear();
        }
        stages.assign(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        si.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
        si.pWaitSemaphores = waits.data();
        si.pWaitDstStageMask = stages.data();
        si.signalSemaphoreCount = static_cast<uint32_t>(job.frames - 1);
        si.pSignalSemaphores = s.synthDone + 1;
    }
    std::lock_guard<std::mutex> qlock(dev_.queueMutex(queue));
    VkResult r = dev_.vt.QueueSubmit(queue, 1, &si, s.fence);
    if (r < 0) {
        BDEX_ERR("synthesis submission failed: %s", vkResultName(r));
        // Still signal the fence (and the semaphores the presents wait) so
        // nothing waits forever; the waits above are consumed too.
        VkSubmitInfo empty = si;
        empty.commandBufferCount = 0;
        dev_.vt.QueueSubmit(queue, 1, &empty, s.fence);
    }
    if (job.presentFence) {
        VkSubmitInfo fs{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        dev_.vt.QueueSubmit(queue, 1, &fs, job.presentFence);
    }
    return r;
}

// Acquires a real swapchain image and submits the copy of frame `index` of
// the job into it. Interpolation: generated frames first, the real one last.
// Extrapolation: the real frame first, then the predicted ones.
VkResult VirtualSwapchain::submitCopy(const Job& job, int index, Copied& out, int staged) {
    const bool real = index == job.realIndex;
    const uint32_t output = static_cast<uint32_t>(index < job.realIndex ? index : index - 1);
    out.index = index;

    FrameSlot& slot = nextSlot(workerSlots_, workerCursor_);
    uint32_t ri = 0;
    if (real_ == realOutOfDate_) return VK_ERROR_OUT_OF_DATE_KHR;
    VkResult r = dev_.vt.AcquireNextImageKHR(dev_.device, real_, UINT64_MAX, slot.acquireSem, VK_NULL_HANDLE, &ri);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) realOutOfDate_ = real_;
    if (r < 0) return r;
    // VK_SUBOPTIMAL is deliberately not forwarded: it reports that *our*
    // real swapchain is no longer optimal, but the application's virtual
    // swapchain is unaffected, so we must not push it into recreating (some
    // D3D translation layers hang while recreating). Instead the application
    // thread re-creates the real swapchain if the surface changed size
    // (followSurface). A genuine change fails the acquire/present with
    // OUT_OF_DATE, which we do propagate.
    if (r == VK_SUBOPTIMAL_KHR) surfaceChanged_.store(true, std::memory_order_relaxed);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev_.vt.BeginCommandBuffer(slot.cmd, &bi);
    const uint32_t profSlot = static_cast<uint32_t>(slots_.size() + (&slot - workerSlots_.data()));
    // Without the passes' slot hooks: they belong to the application
    // thread's synthesis slots (online_tune reads back its sums there).
    fg_->profileBegin(slot.cmd, profSlot, false);
    if (staged >= 0) fg_->recordCopyStaged(slot.cmd, static_cast<uint32_t>(staged), realImages_[ri].image);
    else if (real) fg_->recordCopySource(slot.cmd, job.source, realImages_[ri].image);
    else fg_->recordCopyOutput(slot.cmd, output, realImages_[ri].image);
    fg_->profileMark(slot.cmd, profSlot, FrameGen::StageCopy);
    dev_.vt.EndCommandBuffer(slot.cmd);

    // The copy reads images written by the synthesis submission, which
    // precedes it on the same queue: queue order is the only dependency.
    // (async_compute: two queues, so the synthesis — or, for a deferred
    // job's real frame, the game — is waited through a semaphore, and the
    // copy signals when it is done reading for the next synthesis.)
    VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSemaphore waits[2] = {slot.acquireSem, VK_NULL_HANDLE};
    VkSemaphore signals[2] = {realImages_[ri].readySem, slot.copyDone};
    // (A staged copy reads what the staging copy wrote, earlier on this queue.)
    const bool async = job.asyncSlot >= 0 && staged < 0;
    if (async) {
        const FrameSlot& synth = slots_[static_cast<size_t>(job.asyncSlot)];
        waits[1] = (job.synthSlot >= 0 && real) ? synth.realIn : synth.synthDone[index];
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = async ? 2 : 1;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    si.signalSemaphoreCount = async ? 2 : 1;
    si.pSignalSemaphores = signals;
    {
        std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
        r = dev_.vt.QueueSubmit(dev_.queue, 1, &si, slot.fence);
        if (r < 0) return r;
        // The application's present fence (VK_EXT_swapchain_maintenance1)
        // signals when its swapchain image is free to reuse. Its image is our
        // *virtual* image, freed once our copy has read it — not when the
        // real frame finishes displaying. So we signal that fence ourselves
        // after the copy (queue-ordered) instead of tying it to the real
        // present; otherwise the game blocks forever waiting on a display
        // completion that our async worker owns. A deferred synthesis still
        // reads it: submitDeferredSynthesis signals then.
        if (real && job.presentFence && job.synthSlot < 0 && staged < 0) {
            VkSubmitInfo fs{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            dev_.vt.QueueSubmit(dev_.queue, 1, &fs, job.presentFence);
        }
    }
    if (async) {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        pendingCopyDone_.push_back(slot.copyDone);
    }
    slot.submitted = true;
    out.image = ri;
    out.real = real;
    out.fence = slot.fence;
    return VK_SUCCESS;
}

VkResult VirtualSwapchain::submitStage(const Job& job, int index, uint32_t stageSlot) {
    const bool real = index == job.realIndex;
    const uint32_t output = static_cast<uint32_t>(index < job.realIndex ? index : index - 1);
    FrameSlot& slot = nextSlot(workerSlots_, workerCursor_);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev_.vt.BeginCommandBuffer(slot.cmd, &bi);
    fg_->recordStage(slot.cmd, real, output, job.source, stageSlot);
    dev_.vt.EndCommandBuffer(slot.cmd);
    // async_compute: waits the synthesis like the copy it stands for, and
    // signals when it is done reading for the next synthesis.
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSemaphore wait = VK_NULL_HANDLE;
    const bool async = job.asyncSlot >= 0;
    if (async) wait = slots_[static_cast<size_t>(job.asyncSlot)].synthDone[index];
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = async ? 1 : 0;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    si.signalSemaphoreCount = async ? 1 : 0;
    si.pSignalSemaphores = &slot.copyDone;
    VkResult r;
    {
        std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
        r = dev_.vt.QueueSubmit(dev_.queue, 1, &si, slot.fence);
        if (r >= 0 && real && job.presentFence && job.synthSlot < 0) {  // the application's image is read
            VkSubmitInfo fs{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            dev_.vt.QueueSubmit(dev_.queue, 1, &fs, job.presentFence);
        }
    }
    if (r < 0) return r;
    if (async) {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        pendingCopyDone_.push_back(slot.copyDone);
    }
    slot.submitted = true;
    return VK_SUCCESS;
}

// A present's frames past what the real swapchain lets the worker hold
// acquired at once are staged (runJob): one internal image each. Hybrid and
// extrapolation stage their predicted frames too (all but the real one).
void VirtualSwapchain::prepareStaging() {
    if (!fg_) return;
    const int frames = generating_ ? std::max(1, cfg_.frameCeiling()) : 1;
    const bool predicted = cfg_.extrapolate || cfg_.hybrid;
    fg_->ensureStaging(static_cast<uint32_t>(std::max(0, predicted ? frames - 1 : frames - acquireAhead_)));
}

VkResult VirtualSwapchain::presentCopied(const Job& job, const Copied& c) {
    uint32_t ri = c.image;
    VkPresentIdKHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    VkSwapchainPresentModeInfoEXT presentMode{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT};
    const VkPresentModeKHR presentModeValue = static_cast<VkPresentModeKHR>(job.presentMode);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &realImages_[ri].readySem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &real_;
    pi.pImageIndices = &ri;
    const void** tail = bdexNext(pi);
    uint64_t feedbackId = 0;
    if (c.real && job.presentId) {
        presentId.swapchainCount = 1;
        presentId.pPresentIds = &job.presentId;
        *tail = &presentId;
        tail = bdexNext(presentId);
    } else if (feedback_ && !job.presentId) {
        // Our own ids (the game cannot use present ids: they are hidden).
        feedbackId = ++presentCounter_;
        presentId.swapchainCount = 1;
        presentId.pPresentIds = &feedbackId;
        *tail = &presentId;
        tail = bdexNext(presentId);
    }
    if (job.presentMode >= 0) {
        presentMode.swapchainCount = 1;
        presentMode.pPresentModes = &presentModeValue;
        *tail = &presentMode;
        tail = bdexNext(presentMode);
    }
#ifdef VK_EXT_present_timing
    // The driver times this frame's display (the feedback thread reads it).
    VkPresentTimingInfoEXT timingInfo{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT};
    VkPresentTimingsInfoEXT timings{VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT};
    const bool timed = ptActive_.load() && feedbackId;
    const void** timingsLink = nullptr;  // (the last link: cut on a full timing queue)
    if (timed) {
        timingInfo.timeDomainId = ptDomainId_;
        timingInfo.presentStageQueries = ptStage_;
        timings.swapchainCount = 1;
        timings.pTimingInfos = &timingInfo;
        timingsLink = tail;
        *tail = &timings;
        tail = bdexNext(timings);
    }
#endif
    // Steam's framerate limit (gamescope): gamescope shows one frame per
    // cycle of the limit and queues the others, first in first out — a
    // frame goes out once the previous one is on screen, so one frame waits
    // there instead of the whole swapchain (measured at 30 fps: 92 ms from
    // present to screen with three queued). Bounded, should the feedback stall.
    if (feedbackId > 1 && gsLimited_.load()) {
        const double hz = gsHz_.load();
        waitDisplayed(feedbackId - 1, hz > 0.0 ? 3000.0 / hz : 100.0);
    }
    const auto now = clock::now();
    if (c.real)
        statsRealDelayUs_.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - job.start).count()),
            std::memory_order_relaxed);
    VkResult r;
    {
        std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
        r = dev_.vt.QueuePresentKHR(dev_.queue, &pi);
#ifdef VK_EXT_present_timing
        if (r == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT && timed) {
            // The results were not read back in time (the feedback thread
            // paused): present without them from now on.
            ptActive_ = false;
            *timingsLink = nullptr;
            BDEX_WARN("present timing queue full: display times from present waits only");
            r = dev_.vt.QueuePresentKHR(dev_.queue, &pi);
        }
#endif
    }
    // For the spacing of the next present (paceTarget): when this one went
    // out, and the refresh it was due to show on.
    lastPresentMs_ = msOf(now);
    double lastMs, periodMs;
    lastLandMs_ = displayGrid(lastMs, periodMs)
                      ? lastMs + std::ceil((lastPresentMs_ - lastMs) / periodMs) * periodMs
                      : 0.0;
    // x11_clock in place of the present feedback (which, under Xwayland in
    // immediate mode, reports each present at once): the real frame reaches
    // the compositor at the refresh after its present, and the screen a
    // refresh later at the soonest — the latency from there.
    if (lastLandMs_ > 0.0 && x11Grid()) {
        std::lock_guard<std::mutex> lock(fbMutex_);
        const bool x11 = true;  // (x11_clock first, as displayGrid)
        latencyFromX11_.store(x11, std::memory_order_relaxed);
        // latency_log too: the refresh the frame reaches the compositor at
        // (on screen a refresh later; the plan is on these refreshes), where
        // the present feedback's times are this present's.
        if (x11 && !latencyLogPath_.empty())
            latencyLogLine(latencyLogPath_, msOf(job.start), lastLandMs_, c.real,
                           c.index >= 0 && c.index < 8 ? job.planned[c.index] : 0.0);
    }
    if (c.real && lastLandMs_ > 0.0 && latencyFromX11_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(fbMutex_);
        const bool x11 = true;
        const double latency = lastLandMs_ + periodMs - msOf(job.start);
        if (x11 && latency >= 0.0 && latency < 1000.0) {
            fbLatencyMs_ = fbLatencyMs_ < 0.0 ? latency : fbLatencyMs_ * 0.9 + latency * 0.1;
            fbLatencySum_ += latency;
            ++fbLatencyCount_;
            const double frameLatency = lastLandMs_ + periodMs - job.frameStartMs;
            if (job.frameStartMs > 0.0 && frameLatency >= 0.0 && frameLatency < 1000.0) {
                fbFrameLatencySum_ += frameLatency;
                ++fbFrameLatencyCount_;
            }
        }
    }
    if (feedbackId && r >= 0) {
        {
            std::lock_guard<std::mutex> lock(fbMutex_);
            if (!fbPaused_)
                fbPending_.push_back({feedbackId, msOf(job.start), job.frameStartMs, c.real,
                                      c.index >= 0 && c.index < 8 ? job.planned[c.index] : 0.0});
        }
        fbCv_.notify_one();
    }
    if (r == VK_SUBOPTIMAL_KHR) surfaceChanged_.store(true, std::memory_order_relaxed);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) realOutOfDate_ = real_;
    if (r < 0) return r;
    statsOutFrames_.fetch_add(1, std::memory_order_relaxed);
    return VK_SUCCESS;
}

} // namespace bdex
