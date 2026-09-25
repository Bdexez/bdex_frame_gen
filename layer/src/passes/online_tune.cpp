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
#include "vk_util.h"

#include <cctype>
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
    for (const auto& [key, trial] : cfg.tuneCandidates()) {
        const OnlineTune::Candidate c{key, trial};
        Config off = cfg;
        off.apply(c.key, "0");
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
    }
    void destroy(FgContext& ctx) override {
        DeviceData& dev = ctx.device();
        if (mapped_) dev.vt.UnmapMemory(dev.device, buf_.memory);
        mapped_ = nullptr;
        dev.destroyBuffer(buf_);
        ctx.destroyPipeline(pipe_);
    }

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
        if (ctx.flowLastFrame() && tagValid_ && !tune_->done()) record(ctx, cmd, parity, hasPrev);
        tag_ = tune_->nextFlow(overrides_);
        tagValid_ = true;
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
        tags_[slot] = tag_;
    }

    void collect(FgContext& ctx, uint32_t slot) {
        if (slot >= kSlots || !pending_[slot]) return;
        pending_[slot] = false;
        const auto* w = static_cast<const uint32_t*>(mapped_) + slot * (kSlotBytes / 4);
        if (w[2] > 0)
            tune_->addSample(tags_[slot], w[0] / 4096.0, w[1] / 4096.0, double(w[2]), w[3] / 16.0, w[4] / 16.0, double(w[5]));
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
    uint32_t tags_[kSlots]{};
    uint32_t tag_ = 0;                  // tag of the flow in the flow images
    bool tagValid_ = false;
    bool copiedLastFrame_ = false;      // the previous frame kept its predecessor's flow
    std::vector<std::pair<std::string, std::string>> overrides_;  // this frame's trial settings
};

} // namespace

std::unique_ptr<FgPass> makeOnlineTunePass() { return std::make_unique<OnlineTunePass>(); }

} // namespace bdex
