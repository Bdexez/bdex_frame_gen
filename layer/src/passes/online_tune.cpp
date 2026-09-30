// online_tune: the self-evaluation pass (shaders/self_eval.comp) and the
// per-game trial of the flow options (online_tune.h). The previous flow is
// rated on the frame that just arrived, the sums are written to the command
// slot's counters and read back when the slot is reused (its submission has
// completed then: no stall), and the options under trial are applied to
// each frame's settings through overrideConfig.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "log.h"
#include "online_tune.h"
#include "platform.h"
#include "shaders.h"
#include "synthesis_probe.h"
#include "vk_util.h"

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace bdex {
namespace {

struct SelfEvalPC { int32_t size[2]; float flowExtent[2]; int32_t slot; int32_t step; int32_t hasPrev; };

// Per game and mode (the decisions differ between interpolation and
// extrapolation); "" when there is no cache.
static std::string onlineTuneCachePath(const Config& cfg) {
    const std::string dir = platformCacheDir();
    if (dir.empty() || cfg.tuneName.empty()) return "";
    std::string name;
    for (char c : cfg.tuneName) name += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.') ? c : '_';
    return dir + "/online-tune-" + name + (cfg.extrapolate ? "-extrapolate" : "-interpolate") + ".txt";
}

// online_tune candidates come from the option registry (config.cpp, the
// Tune column): flow options measured non-negative on average that the
// self-evaluation can rate (they change the motion field; synthesis and
// resource-sizing options cannot be tried this way). The self-evaluation
// predicts the next frame, i.e. rates the flow the way extrapolation uses
// it; sub-pixel vectors are only tried there: on a 3.25 px/frame pan they
// win 2 dB in extrapolation, as the measure says, but lose 0.9 dB
// interpolated (the half-way bilinear resampling blurs). An option the user
// set is left alone: it is not a candidate.
static std::shared_ptr<OnlineTune> createOnlineTune(const Config& cfg) {
    std::vector<OnlineTune::Candidate> candidates;
    const std::string mine = cfg.describe();
    for (const auto& tc : cfg.tuneCandidates()) {
        const OnlineTune::Candidate c{tc.key, tc.value, tc.synthesis};
        // Not a candidate if the user set any of its options.
        std::vector<std::pair<std::string, std::string>> parts;
        OnlineTune::expand(c.key, c.value, parts);
        Config off = cfg;
        for (const auto& kv : parts) off.apply(kv.first, "0");
        if (off.describe() == mine) candidates.push_back(c);
    }
    auto tune = std::make_shared<OnlineTune>(candidates);
    if (const std::string path = onlineTuneCachePath(cfg); !path.empty()) {
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        if (in && tune->deserialize(text.str()))
            BDEX_INFO("online_tune: cached decisions for %s: %s%s", cfg.tuneName.c_str(), tune->summary().c_str(),
                      tune->done() ? "" : " (trying the others)");
    }
    return tune;
}

class OnlineTunePass : public FgPass {
public:
    static constexpr uint32_t kSlots = 16;  // command slots tracked (the engine's profile slots)
    static constexpr uint32_t kSlotBytes = 32;  // 8 counters (see self_eval.comp)

    const char* name() const override { return "online_tune"; }

    void create(FgContext& ctx) override {
        DeviceData& dev = ctx.device();
        {
            std::lock_guard<std::mutex> lock(dev.onlineTuneMutex);
            if (!dev.onlineTune) {
                dev.onlineTune = createOnlineTune(ctx.baseConfig());
                dev.onlineTuneReported = dev.onlineTune->decisions().size();  // cached ones were logged
            }
            tune_ = dev.onlineTune;
        }
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pipe_ = ctx.makePipeline(self_eval_comp, self_eval_comp_size, {CIS, CIS, CIS, SB, CIS});
        buf_ = dev.createBuffer(kSlots * kSlotBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VK_CHECK(dev.vt.MapMemory(dev.device, buf_.memory, 0, VK_WHOLE_SIZE, 0, &mapped_));
        const FgFineLevel l0 = ctx.fineLevel();
        for (uint32_t parity = 0; parity < 2; ++parity) {
            VkDescriptorSet& e = sets_[parity];
            e = ctx.allocSet(pipe_.setLayout);
            ctx.writeImage(e, 0, CIS, l0.pyr[1 - parity]);
            ctx.writeImage(e, 1, CIS, l0.pyr[parity]);
            ctx.writeImage(e, 2, CIS, l0.flow[1]);
            ctx.writeBuffer(e, 3, buf_.buffer);
            ctx.writeImage(e, 4, CIS, ctx.previousBackwardFlow());
        }
        // Synthesis options are rated by the probe (synthesis_probe.h),
        // built only if one of them is a candidate.
        for (const auto& tc : ctx.baseConfig().tuneCandidates())
            if (tc.synthesis) hasSynthesis_ = true;
        if (hasSynthesis_) probe_.create(ctx, ctx.baseConfig().extrapolate, buf_.buffer);
    }
    void destroy(FgContext& ctx) override {
        DeviceData& dev = ctx.device();
        if (mapped_) dev.vt.UnmapMemory(dev.device, buf_.memory);
        mapped_ = nullptr;
        if (hasSynthesis_) probe_.destroy(ctx);
        dev.destroyBuffer(buf_);
        ctx.destroyPipeline(pipe_);
    }

    void frameBegin(FgContext&) override { probe_.frameBegin(); }

    void slotBegin(FgContext& ctx, uint32_t slot) override {
        collect(ctx, slot);
        slot_ = slot;
    }

    void flowBegin(FgContext& ctx, VkCommandBuffer cmd, uint32_t parity) override {
        // Rate the flow still in the images (the previous pair's) on the
        // frame that just arrived, then pick the settings of this one.
        // (Nothing left to measure once every candidate is decided.)
        // The previous pair's flow is at hand (stability) when it was kept
        // last frame: the copy happens in recordFlow after this hook, on a
        // frame whose previous one computed a flow.
        const bool hasPrev = copiedLastFrame_ && ctx.flowLastFrame();
        copiedLastFrame_ = ctx.flowLastFrame() && !tune_->done();
        // A synthesis option on trial is rated by the probe instead.
        synthesisTrial_ = hasSynthesis_ && tune_->trialIsSynthesis();
        // Once decided, a lighter watch (every 8th frame) of the content for a change.
        const bool watch = tune_->done() && (++watchCount_ % 8) == 0;
        if (ctx.flowLastFrame() && tagValid_ && (watch || (!tune_->done() && !synthesisTrial_)))
            record(ctx, cmd, parity, hasPrev);
        tag_ = tune_->nextFlow(overrides_);
        tagValid_ = true;
        if (hasSynthesis_) probe_.flowBegin(ctx, cmd, synthesisTrial_);  // with this frame's settings
    }

    VkDescriptorSet synthesisInputs(FgContext& ctx, VkCommandBuffer cmd, uint32_t prev, uint32_t cur, uint32_t) override {
        if (!hasSynthesis_) return VK_NULL_HANDLE;
        const uint32_t slot = slot_;
        const bool free = slot < kSlots && !pending_[slot];
        if (free && synthesisTrial_) {
            auto& vt = ctx.device().vt;
            vt.CmdFillBuffer(cmd, buf_.buffer, VkDeviceSize(slot) * kSlotBytes, kSlotBytes, 0);
            ctx.barrier(cmd);
        }
        if (probe_.measure(ctx, cmd, prev, cur, free ? slot : kSlots, synthesisTrial_) && free) {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            ctx.device().vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                               &mb, 0, nullptr, 0, nullptr);
            pending_[slot] = true;
            probeSample_[slot] = true;
            tags_[slot] = tag_;
        }
        return VK_NULL_HANDLE;
    }

    // While trying, keep the previous pair's flow for the stability measure.
    bool needsPreviousFlow(const Config&) const override { return !tune_->done(); }

    void overrideConfig(Config& c) const override {
        for (const auto& kv : overrides_) c.apply(kv.first, kv.second);
    }

private:
    void record(FgContext& ctx, VkCommandBuffer cmd, uint32_t parity, bool hasPrev) {
        const uint32_t slot = slot_;
        if (slot >= kSlots || pending_[slot]) return;
        auto& vt = ctx.device().vt;
        const FgFineLevel l0 = ctx.fineLevel();
        // At most 2^18 samples, so the fixed-point sums cannot overflow.
        uint32_t step = 2;
        while (uint64_t(fg::divUp(l0.luma.width, step)) * fg::divUp(l0.luma.height, step) > (1u << 18)) ++step;
        vt.CmdFillBuffer(cmd, buf_.buffer, VkDeviceSize(slot) * kSlotBytes, kSlotBytes, 0);
        ctx.barrier(cmd);
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        SelfEvalPC pc{{(int32_t)l0.luma.width, (int32_t)l0.luma.height},
                      {float(l0.fine.width * l0.block), float(l0.fine.height * l0.block)}, (int32_t)slot, (int32_t)step,
                      hasPrev ? 1 : 0};
        vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &sets_[parity], 0, nullptr);
        vt.CmdDispatch(cmd, fg::divUp(fg::divUp(l0.luma.width, step), 16), fg::divUp(fg::divUp(l0.luma.height, step), 16), 1);
        // The matching then rewrites the flow it read; the host reads the sums.
        ctx.barrier(cmd);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        pending_[slot] = true;
        probeSample_[slot] = false;
        tags_[slot] = tag_;
    }

    void collect(FgContext& ctx, uint32_t slot) {
        if (slot >= kSlots || !pending_[slot]) return;
        pending_[slot] = false;
        const auto* w = static_cast<const uint32_t*>(mapped_) + slot * (kSlotBytes / 4);
        if (w[2] > 0 && !probeSample_[slot] &&
            tune_->observe(w[1] / 4096.0 / double(w[2]), w[6] / 4096.0 / double(w[2]))) {
            BDEX_INFO("online_tune: the content changed (motion or brightness) — trying the options again");
            std::lock_guard<std::mutex> lock(ctx.device().onlineTuneMutex);
            ctx.device().onlineTuneReported = 0;  // the new decisions are logged again
        }
        if (w[2] > 0 && probeSample_[slot]) {
            // Probe: squared errors, compared as RMS (the scale of the
            // self-evaluation's mean errors, weighted like PSNR).
            const double n = double(w[2]);
            tune_->addSample(tags_[slot], std::sqrt(w[0] / 4096.0 / n) * n, std::sqrt(w[1] / 4096.0 / n) * n, n);
        } else if (w[2] > 0) {
            tune_->addSample(tags_[slot], w[0] / 4096.0, w[1] / 4096.0, double(w[2]), w[3] / 16.0, w[4] / 16.0, double(w[5]));
        }
        report(ctx);
    }

    // Logs each new decision and caches the decisions made so far for the game.
    void report(FgContext& ctx) {
        DeviceData& dev = ctx.device();
        const Config& cfg = ctx.baseConfig();
        std::lock_guard<std::mutex> lock(dev.onlineTuneMutex);
        const auto decisions = tune_->decisions();
        if (decisions.size() == dev.onlineTuneReported) return;
        for (size_t i = dev.onlineTuneReported; i < decisions.size(); ++i)
            BDEX_INFO("online_tune: %s=%s %s (prediction error %+.1f %%, flow change %+.0f %%)", decisions[i].key.c_str(),
                      decisions[i].value.c_str(), decisions[i].kept ? "kept" : "dropped", -100.0 * decisions[i].gain,
                      -100.0 * decisions[i].stability);
        dev.onlineTuneReported = decisions.size();
        if (tune_->done()) BDEX_INFO("online_tune: done for %s: %s", cfg.tuneName.c_str(), tune_->summary().c_str());
        // Cached after each decision: a short session still spares the next one.
        const std::string path = onlineTuneCachePath(cfg);
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path, std::ios::trunc);
        out << tune_->serialize();
        if (out && tune_->done()) BDEX_INFO("online_tune: decisions cached in %s", path.c_str());
    }

    std::shared_ptr<OnlineTune> tune_;  // DeviceData::onlineTune: shared by the device's swapchains
    FgPipeline pipe_;
    VkDescriptorSet sets_[2] = {};      // per parity: lumas of the pair, finest backward flow, counters
    AllocatedBuffer buf_;               // host-visible: 4 words per slot
    void* mapped_ = nullptr;
    uint32_t slot_ = kSlots;            // slot of the command buffer being recorded
    bool pending_[kSlots]{};
    bool probeSample_[kSlots]{};        // the slot holds a synthesis-probe measurement
    uint32_t tags_[kSlots]{};
    uint32_t tag_ = 0;                  // tag of the flow in the flow images
    bool tagValid_ = false;
    bool copiedLastFrame_ = false;      // the previous frame kept its predecessor's flow
    uint32_t watchCount_ = 0;
    bool hasSynthesis_ = false;         // synthesis options among the candidates: the probe exists
    bool synthesisTrial_ = false;       // this frame, a synthesis option is on trial
    SynthesisProbe probe_;
    std::vector<std::pair<std::string, std::string>> overrides_;  // this frame's trial settings
};

} // namespace

std::unique_ptr<FgPass> makeOnlineTunePass() { return std::make_unique<OnlineTunePass>(); }

} // namespace bdex
