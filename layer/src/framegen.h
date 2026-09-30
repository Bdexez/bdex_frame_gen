#pragma once
#include "descriptor_planner.h"
#include "device.h"
#include "fg_pass.h"
#include "format.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bdex {

// GPU side of the frame generation for one swapchain: history images, luma
// pyramids, flow fields, the compute pipelines that produce them, and the
// command recording that ties everything together.
class FrameGen : private FgContext {
public:
    // `sources` are the images the application renders into (the virtual
    // swapchain images): they double as the frame history, so nothing is
    // copied before the analysis. `outputs` is the number of generated frames
    // per real frame (multiplier minus one); 0 creates a pass-through
    // instance with only the copy helper.
    // `renderExtent` is the resolution the application renders at (the source
    // images); `displayExtent` is the swapchain resolution presented. They
    // differ when spatial upscaling is on, in which case every presented frame
    // is resampled from render to display size.
    // `colorSpace` is the swapchain's: HDR colour spaces change how the
    // sampled values are brought to the matching's perceptual scale.
    // `flowScaleFactor` multiplies the configured flow resolution divisor (up
    // to 4): the gpu_budget governor's coarsest levels.
    FrameGen(DeviceData& dev, VkFormat format, VkExtent2D renderExtent, VkExtent2D displayExtent, uint32_t outputs,
             const std::vector<VkImage>& sources, VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
             int flowScaleFactor = 1, const Config* config = nullptr);
    ~FrameGen();
    FrameGen(const FrameGen&) = delete;
    FrameGen& operator=(const FrameGen&) = delete;

    // A synthesis is bracketed by recordAcquireSources / recordReleaseSources,
    // which move the source images (PRESENT_SRC when the application hands
    // them over) to GENERAL and back. `prev` may be -1 on the first frame.
    void recordAcquireSources(VkCommandBuffer cmd, int prev, int cur);
    void recordReleaseSources(VkCommandBuffer cmd, int prev, int cur);
    // Builds the luma pyramid of slot `parity` from source `cur`.
    void recordAnalysis(VkCommandBuffer cmd, uint32_t parity, uint32_t cur);
    // Computes the flow fields between pyramid slots 1-parity (previous
    // frame) and parity (current frame). Requires both to have been analysed.
    void recordFlow(VkCommandBuffer cmd, uint32_t parity);
    // Writes the frame at time t (0..1: between prev and cur; 1..2: after
    // cur) into the internal output image `output`.
    void recordInterpolate(VkCommandBuffer cmd, uint32_t prev, uint32_t cur, float t, uint32_t output);
    // Timing of the real frame being recorded, for the per-frame state pass.
    struct FrameTiming {
        double sinceLastMs = 0.0;  // time since the previous real frame was presented
        double intervalMs = 0.0;   // smoothed frame interval the pacing spreads the outputs over
        int multiplier = 2;        // frames presented for this real frame
        bool reset = false;        // no usable previous frame (first frame, history lost)
        // Predicted display times (ms after the present call) of the
        // generated outputs [0..2] and of the real frame [3], from the
        // measured refresh grid; without it (haveDisplay false) the pacing's
        // nominal schedule is assumed.
        bool haveDisplay = false;
        float displayMs[4] = {};
    };
    // Per-frame state pass (frame_state.comp): duplicate detection (dedup)
    // and the content-time t of every output (frame_timing, dedup). Call it
    // after recordAnalysis and before recordFlow; a no-op when neither
    // option is set, and the outputs then use the fixed t they are given.
    void recordFrameState(VkCommandBuffer cmd, const FrameTiming& timing);
    // The state computed by the last recordFrameState, once its submission
    // has completed. Returns false when no state pass was recorded.
    struct State {
        uint32_t dup = 0;     // the frame repeated the previous one
        float t[4] = {};      // t of each output slot, [3] = the real frame's slot
        float intervalMs = 0; // content interval used
        uint32_t repeats = 0; // repeated frames detected since the output stage was built
    };
    bool readState(State& out) const;
    // Copies output image `output` into `dst` (UNDEFINED -> PRESENT_SRC).
    void recordCopyOutput(VkCommandBuffer cmd, uint32_t output, VkImage dst);
    // Copies source `src` (PRESENT_SRC) into `dst` (UNDEFINED -> PRESENT_SRC).
    // When upscaling, `dst` receives the upscaled real frame instead (prepared
    // by recordUpscaleReal in the synthesis command buffer).
    void recordCopySource(VkCommandBuffer cmd, uint32_t src, VkImage dst);
    // Upscales source `cur` (in GENERAL, i.e. between acquire/release) to the
    // display resolution, into the internal real-frame image. Only used when
    // upscaling; recorded in the synthesis command buffer. With dedup in
    // extrapolation, a repeated frame is replaced there by a frame
    // extrapolated from `prev` (-1: no previous frame) and the kept flow.
    void recordUpscaleReal(VkCommandBuffer cmd, uint32_t cur, int prev = -1);

    bool upscaling() const override { return upscaling_; }
    // The real frame is produced into an internal packed image (for upscaling
    // or the HUD) rather than copied straight through.
    bool packReal() const { return packReal_; }
    // Current frame-rate figures for the on-screen HUD (fps and the 1% / 0.1%
    // lows).
    // `latencyMs`: measured present-to-screen latency (HUD level 5), -1 unknown.
    void setHud(int gameFps, int outFps, int low1, int low01, int latencyMs = -1) {
        hudGame_ = gameFps; hudOut_ = outFps; hudLow1_ = low1; hudLow01_ = low01; hudLatency_ = latencyMs;
    }

    // Debug frame dumping: records a copy of the image that was just written
    // to `dst` (the generated output or the current history) into a staging
    // buffer; writeDump() decodes it to a PPM file once the GPU is done.
    // `which`: 0..outputs_-1 = generated outputs, outputs_ = real frame.
    void recordDump(VkCommandBuffer cmd, uint32_t which, uint32_t cur);
    bool writeDump(const std::string& path, uint32_t which);
    uint32_t outputCount() const { return outputs_; }

    int levels() const { return levels_; }

    // GPU profiling (config.profile): timestamps are written around each
    // stage of a slot's command buffer and collected once its fence signalled.
    enum Stage { StageAnalysis = 0, StageFlow, StageInterp, StageCopy, StageCount };
    void profileBegin(VkCommandBuffer cmd, uint32_t slot);
    void profileMark(VkCommandBuffer cmd, uint32_t slot, Stage stage);
    void profileCollect(uint32_t slot);
    // Average GPU milliseconds per stage since the last call; resets the accumulators.
    std::string profileReport();
    // gpu_budget: average GPU milliseconds of the generation (analysis, flow,
    // synthesis) per frame since the last call (0 if unmeasured); resets.
    double takeGpuMs();
    // gpu_budget quality level (see GpuBudget): 1 turns the costly opt-in
    // passes off, 2 also narrows the search. Coarser flow levels are a
    // rebuild with a larger flowScaleFactor.
    void setQualityLevel(int level) { qualityLevel_ = level; }
    int flowScale() const { return int(flowScale_); }
    // The optional passes at work with the current settings, comma-separated
    // (status, diagnostics).
    std::string passNames() const {
        const Config cfg = effectiveConfig();
        std::string s;
        for (const auto& p : passes_)
            if (p->active(cfg)) s += (s.empty() ? "" : ",") + std::string(p->name());
        return s;
    }

private:
    struct Level {
        VkExtent2D size{};    // luma resolution
        VkExtent2D blocks{};  // flow resolution
        AllocatedImage pyr[2];
        AllocatedImage chroma[2];  // Cb/Cr pyramid (chroma_weight), per parity; empty when off
        VkExtent2D fine{};    // refined flow resolution
        bool refined = false; // flowFine is a distinct, refined image (else it aliases flow)
        AllocatedImage flowRaw[2];  // [0] forward, [1] backward
        AllocatedImage flow[2];     // smoothed (median)
        AllocatedImage flowFine[2]; // refined, consumed by the next level and the interpolation
        AllocatedImage cost[2];
    };
    using Pass = FgPipeline;

    void recordCas(VkCommandBuffer cmd, VkDescriptorSet set);
    void recordSynthesis(VkCommandBuffer cmd, VkDescriptorSet set, float t, int32_t timing, bool gated);
    void recordSynthesisImpl(VkCommandBuffer cmd, const FgPipeline& pipe, VkDescriptorSet set, float t, int32_t timing,
                             bool gated, const Config& cfg, bool hud);
    void createPipelines();
    void createResources();
    void createDescriptors();
    void destroyAll();
    Pass makePass(const uint32_t* spv, size_t spvSize, const std::vector<VkDescriptorType>& bindings);
    VkDescriptorSet allocSet(VkDescriptorSetLayout layout) override;
    void writeImage(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView view) override;
    void writeBuffer(VkDescriptorSet set, uint32_t binding, VkBuffer buffer) override;
    void memoryBarrier(VkCommandBuffer cmd);
    void copyToPresent(VkCommandBuffer cmd, VkImage src, VkImageLayout srcLayout, VkAccessFlags srcAccess,
                       VkPipelineStageFlags srcStage, VkImage dst);

    DeviceData& dev_;
    // The settings this instance reads (dev_.config by default; a virtual
    // swapchain passes its own snapshot, see DeviceData::snapshotConfig).
    const Config* cfgp_;
    VkFormat format_;
    VkExtent2D extent_;         // render resolution (source images, flow pyramid)
    VkExtent2D displayExtent_;  // presented resolution (== extent_ unless upscaling)
    bool upscaling_ = false;
    bool sharpen_ = false;      // contrast-adaptive sharpening pass after upscaling
    int  hudMode_ = 0;          // on-screen fps HUD detail level (0 = off)
    bool packReal_ = false;     // real frame goes through the internal packed image (upscaling or HUD)
    int hudGame_ = 0, hudOut_ = 0, hudLow1_ = 0, hudLow01_ = 0, hudLatency_ = -1;
    OutputEncoding enc_;
    int levels_ = 0;
    float flowScale_ = 1.f;
    bool flowValid_ = false;    // the flow images hold a computed flow (temporal candidate source)
    VkFormat lumaFormat_ = VK_FORMAT_R32_SFLOAT;

    struct Source {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    std::vector<Source> sources_;
    std::vector<Level> levels_v_;
    std::vector<AllocatedImage> out_;
    AllocatedImage outReal_;  // upscaled real frame (display size, packed); only when upscaling_
    std::vector<AllocatedImage> sharp_;  // linear rgba16f pre-sharpen, per output; only when sharpen_
    AllocatedImage sharpReal_;           // linear rgba16f pre-sharpen real frame; only when sharpen_
    uint32_t outputs_ = 0;
    AllocatedBuffer stateBuf_;  // frame state: scene-cut cost, content clock, t per output, gated dispatches
    std::vector<AllocatedBuffer> dumpBuf_;
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kProfileSlots = 12;  // application slots + worker slots
    double profileMs_[StageCount]{};
    double budgetMs_ = 0.0;
    uint32_t budgetFrames_ = 0;
    int qualityLevel_ = 0;
    int flowScaleFactor_ = 1;
    Config effectiveConfig() const override;  // the settings with the passes' and the quality level's overrides
    uint32_t profileFrames_[StageCount]{};
    bool profileWritten_[kProfileSlots][StageCount + 1]{};
    float timestampPeriodNs_ = 1.f;
    std::vector<void*> dumpMapped_;
    AllocatedBuffer costReadback_;
    void* costMapped_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> pools_;  // opened as allocSet needs them (see DescriptorPlanner)
    DescriptorPlanner planner_;

    Pass downsample_, blockMatch_, smooth_, refine_, reduce_, interp_, upscale_, cas_, splat_, frameState_, downChroma_;

    // Optional passes (fg_pass.h, passes/), in hook order, and what they see
    // of the engine (FgContext).
    std::vector<std::unique_ptr<FgPass>> passes_;
    DeviceData& device() override { return dev_; }
    const Config& baseConfig() const override { return *cfgp_; }
    FgPipeline makePipeline(const uint32_t* spv, size_t spvSize, const std::vector<VkDescriptorType>& bindings) override {
        return makePass(spv, spvSize, bindings);
    }
    void destroyPipeline(FgPipeline& p) override;
    AllocatedImage createImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) override;
    void barrier(VkCommandBuffer cmd) override { memoryBarrier(cmd); }
    FgFineLevel fineLevel() const override;
    VkImageView previousBackwardFlow() const override { return prevBwd_.view; }
    VkBuffer stateBuffer() const override { return stateBuf_.buffer; }
    uint32_t sourceCount() const override { return static_cast<uint32_t>(sources_.size()); }
    VkImageView sourceView(uint32_t index) const override { return sources_[index].view; }
    VkFormat sourceFormat() const override { return format_; }
    VkImage sourceImage(uint32_t index) const override { return sources_[index].image; }
    VkExtent2D renderExtent() const override { return extent_; }
    VkExtent2D displayExtent() const override { return displayExtent_; }
    uint32_t outputs() const override { return outputs_; }
    float flowToRender() const override { return flowScale_; }
    bool flowLastFrame() const override { return flowLastFrame_; }
    bool repeatFlagValid() const override { return timingActive_ && dedup_; }
    VkDescriptorSet synthesisSet(VkImageView prev, VkImageView cur, uint32_t output) override;
    VkDescriptorSet realOutputSet(VkImageView src) override;
    void setStaticMask(VkImageView view) override;
    FgPipeline makeLinearSynthesisPipeline() override;
    VkDescriptorSet synthesisSetFor(VkDescriptorSetLayout layout, VkImageView prev, VkImageView cur, VkImageView fwd,
                                    VkImageView bwd, VkImageView out) override;
    void recordSynthesisWith(VkCommandBuffer cmd, const FgPipeline& pipe, VkDescriptorSet set, float t,
                             const Config& cfg) override;
    AllocatedImage maskDummy_;               // 1x1 zero: the static mask without hud_mask
    VkImageView staticMask_ = VK_NULL_HANDLE;
    bool hasStaticMask_ = false;
    std::vector<VkDescriptorSet> synthSets_;  // every synthesis set (binding 8 follows setStaticMask)
    bool chroma_ = false;                         // chroma pyramid built (chroma_weight > 0 at creation)
    AllocatedImage chromaDummy_;                  // bound to the matcher's chroma slots when chroma_ is off
    std::vector<VkDescriptorSet> dsChromaSrc_[2]; // [parity] per source (level 0)
    std::vector<VkDescriptorSet> dsChroma_[2];    // [parity] per level > 0

    // Per-frame state (frame_state.comp), see recordFrameState.
    void dispatchGated(VkCommandBuffer cmd, uint32_t entry, uint32_t x, uint32_t y);
    void stateBarrier(VkCommandBuffer cmd);
    bool dedup_ = false;          // duplicate detection (config at creation)
    bool dupReal_ = false;        // extrapolated replacement of repeated real frames (dedup + extrapolation)
    bool timingActive_ = false;   // this frame's outputs read their t from the state buffer
    bool gate_ = false;           // this frame's flow dispatches are gated on the duplicate test
    bool flowThisFrame_ = false;  // recordFlow ran for the frame being recorded
    bool flowLastFrame_ = false;  // ... for the previous frame (its flow may be kept)
    std::vector<uint32_t> gateFull_;  // 4 words per gated dispatch (x, y, z, only-on-duplicates)
    uint32_t gateReduce_ = 0, gateDupReal_ = 0;  // entry indices (flow levels use 3l .. 3l+2)
    AllocatedBuffer stateReadback_;
    void* stateMapped_ = nullptr;
    VkDescriptorSet dsState_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> dsInterpReal_;       // [prev * N + cur]: dupReal_ output (the real-frame image)

    // Descriptor sets, indexed [parity][level] etc.
    std::vector<VkDescriptorSet> dsDownSrc_[2];       // [parity] per source (level 0)
    std::vector<VkDescriptorSet> dsDown_[2];          // [parity] per level > 0
    std::vector<VkDescriptorSet> dsMatch_[2][2];      // [parity][dir] per level
    std::vector<VkDescriptorSet> dsSmooth_[2];        // [dir] per level
    std::vector<VkDescriptorSet> dsRefine_[2][2];     // [parity][dir] per level
    VkDescriptorSet dsReduce_ = VK_NULL_HANDLE;
    VkDescriptorSet dsSplat_ = VK_NULL_HANDLE;
    AllocatedImage splatMap_;  // forward-splat targets (display size), or 1x1 when splatting is off
    AllocatedImage prevBwd_;   // extrapolate_accel: the previous pair's finest backward flow
    float accelThisFrame_ = 0.f; // extrapolate_accel weight for this frame's syntheses (0 = off)
    bool splatting_ = false;   // extrapolate_splat in effect
    std::vector<VkDescriptorSet> dsInterp_;           // [(prev * N + cur) * outputs + output]
    std::vector<VkDescriptorSet> dsUpscale_;          // [source]; only when upscaling_
    std::vector<VkDescriptorSet> dsCas_;              // [output] generated frames; only when sharpen_
    VkDescriptorSet dsCasReal_ = VK_NULL_HANDLE;      // real frame; only when sharpen_
};

} // namespace bdex
