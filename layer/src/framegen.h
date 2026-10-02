#pragma once
#include "descriptor_planner.h"
#include "device.h"
#include "fg_pass.h"
#include "format.h"

#include <memory>
#include <mutex>
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
    void recordFlow(VkCommandBuffer cmd, uint32_t parity, int costSlot = -1);
    // Writes the frame at time t (0..1: between prev and cur; 1..2: after
    // cur) into the internal output image `output`.
    void recordInterpolate(VkCommandBuffer cmd, uint32_t prev, uint32_t cur, float t, uint32_t output);
    // Timing of the real frame being recorded, for the per-frame state pass.
    struct FrameTiming {
        double sinceLastMs = 0.0;  // time since the previous real frame was presented
        double intervalMs = 0.0;   // smoothed frame interval the pacing spreads the outputs over
        int multiplier = 2;        // frames presented for this real frame
        int realPos = -1;          // display position of the real frame (frame_order.h); -1 = from the mode
        bool reset = false;        // no usable previous frame (first frame, history lost)
        // Predicted display times (ms after the present call) of the
        // generated outputs [0..6] and of the real frame [7], from the
        // measured refresh grid; without it (haveDisplay false) the pacing's
        // nominal schedule is assumed.
        bool haveDisplay = false;
        float displayMs[8] = {};
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
        float t[8] = {};      // t of each output slot, [7] = the real frame's slot
        float intervalMs = 0; // content interval used
        uint32_t repeats = 0; // repeated frames detected since the output stage was built
    };
    bool readState(State& out) const;
    // Mean matching cost of the last flow read back (the scene-cut input), a
    // few frames old; negative when none yet. collectMeanCost(slot) takes the
    // value recordFlow(..., slot) copied, once that slot's fence has signalled.
    float lastMeanCost() const { return lastCost_; }
    // The share of the last generated frames the global scene-cut fallback
    // replaced by the real frame (0..1, with scene_cut_hist's gate), -1 if
    // unknown: 1 = a duplicate of the real frame, between = faded to it.
    float lastCutWeight() const;
    void collectMeanCost(uint32_t slot);
    // Copies output image `output` into `dst` (UNDEFINED -> PRESENT_SRC).
    void recordCopyOutput(VkCommandBuffer cmd, uint32_t output, VkImage dst);
    // Staging for the worker (VirtualSwapchain::runJob): a frame of a present
    // copied into an internal image right away, when the real swapchain
    // cannot hand out an image for every frame at once, then from there to
    // its swapchain image when it is presented. `real`: the real frame (the
    // packed image, or source `source`), else generated output `output`.
    // ensureStaging (application thread, between frames) allocates `count`.
    void ensureStaging(uint32_t count);
    uint32_t stagingCount() const { return static_cast<uint32_t>(stage_.size()); }
    void recordStage(VkCommandBuffer cmd, bool real, uint32_t output, uint32_t source, uint32_t slot);
    void recordCopyStaged(VkCommandBuffer cmd, uint32_t slot, VkImage dst);
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
    // flow_confidence: share of uncertain flow texels since the last call (-1: not measured).
    double takeUncertainShare() {
        std::lock_guard<std::mutex> lock(confMutex_);
        const double r = confTotal_ ? double(confUncertain_) / double(confTotal_) : -1.0;
        confUncertain_ = confTotal_ = 0;
        return r;
    }
    // HUD level 6: the pacing graph (FgHud::graph), oldest sample first.
    void setHudGraph(const uint8_t* samples, int count, int refHalfMs) {
        for (auto& w : hudGraph_) w = 0;
        for (int i = 0; i < count && i < FgHud::kHudGraph; ++i)
            hudGraph_[i / 4] |= uint32_t(samples[i]) << (8 * (i % 4));
        hudGraphRef_ = refHalfMs;
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
    // `passHooks`: the optional passes' slotBegin runs too — only for the
    // application thread's synthesis slots (the worker's copy slots call
    // with false: a pass's slot bookkeeping belongs to one thread).
    void profileBegin(VkCommandBuffer cmd, uint32_t slot, bool passHooks = true);
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
                             bool gated, const Config& cfg);
    void createPipelines();
    void createResources();
    void createDescriptors();
    void destroyAll();
    Pass makePass(const uint32_t* spv, size_t spvSize, const std::vector<VkDescriptorType>& bindings, bool lean = false,
                  bool leanOnly = false, bool rot = false);
    bool matcherFull_ = true;  // the GPU has the shared memory for the full block matcher (createPipelines)
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
    uint32_t hudGraph_[FgHud::kHudGraph / 4] = {};
    int hudGraphRef_ = 0;
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
    std::vector<AllocatedImage> stage_;  // worker staging (ensureStaging), output storage format, display size
    std::vector<AllocatedImage> sharp_;  // linear rgba16f pre-sharpen, per output; only when sharpen_
    AllocatedImage sharpReal_;           // linear rgba16f pre-sharpen real frame; only when sharpen_
    uint32_t outputs_ = 0;
    AllocatedBuffer stateBuf_;  // frame state: scene-cut cost, content clock, t per output, gated dispatches
    std::vector<AllocatedBuffer> dumpBuf_;
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kProfileSlots = 20;  // application slots + worker slots
    // The worker thread collects its copy slots' timestamps while the
    // application thread collects and reports the others.
    std::mutex profileMutex_;
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

    Pass downsample_, blockMatch_, smooth_, refine_, reduce_, interp_, upscale_, cas_, frameState_, downChroma_, sceneHist_;
    VkDescriptorSet dsSceneHist_ = VK_NULL_HANDLE;  // scene_cut_hist: both pyramid slots at histLevel_
    int histLevel_ = 0;

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
    void setConfidenceMap(VkImageView view) override;
    VkDescriptorSet synthesisSetWithFlow(VkImageView prev, VkImageView cur, VkImageView fwd, VkImageView bwd,
                                         uint32_t output) override {
        return synthesisSetFor(interp_.setLayout, prev, cur, fwd, bwd, (sharpen_ ? sharp_[output] : out_[output]).view);
    }
    int pyramidLevels() const override { return levels_; }
    VkExtent2D pyramidExtent(int level) const override { return levels_v_[size_t(level)].size; }
    VkImageView pyramidView(int level, uint32_t parity) const override { return levels_v_[size_t(level)].pyr[parity & 1].view; }
    VkFormat lumaFormat() const override { return lumaFormat_; }
    void reportConfidence(uint32_t uncertain, uint32_t total) override {
        std::lock_guard<std::mutex> lock(confMutex_);
        confUncertain_ += uncertain;
        confTotal_ += total;
    }
    void setSplatMap(VkImageView view) override;
    void dispatchSynthesisSize(VkCommandBuffer cmd, bool gated) override;
    VkImageView outputView(int output) const override { return output < 0 ? outReal_.view : out_[output].view; }
    OutputEncoding outputEncoding() const override { return enc_; }
    FgHud hud() const override {
        FgHud h{hudMode_, hudGame_, hudOut_, hudLow1_, hudLow01_, hudLatency_, {}, hudGraphRef_};
        for (int i = 0; i < FgHud::kHudGraph / 4; ++i) h.graph[i] = hudGraph_[i];
        return h;
    }
    FgPipeline makeLinearSynthesisPipeline() override;
    VkDescriptorSet synthesisSetFor(VkDescriptorSetLayout layout, VkImageView prev, VkImageView cur, VkImageView fwd,
                                    VkImageView bwd, VkImageView out) override;
    void recordSynthesisWith(VkCommandBuffer cmd, const FgPipeline& pipe, VkDescriptorSet set, float t,
                             const Config& cfg) override;
    AllocatedImage maskDummy_;               // 1x1 zero: the static mask without hud_mask
    VkImageView staticMask_ = VK_NULL_HANDLE;
    AllocatedBuffer altFlow_[2];             // alt_vectors: the matcher's second hypothesis per block (vec2), [direction]
    AllocatedImage confDummy_;               // 1x1 one: the confidence map without flow_confidence
    VkImageView confMap_ = VK_NULL_HANDLE;
    bool hasConfMap_ = false;
    std::mutex confMutex_;
    uint64_t confUncertain_ = 0, confTotal_ = 0;
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
    AllocatedBuffer meanCostBuf_;         // one float per profiling slot: mean flow cost
    const float* meanCostMapped_ = nullptr;
    bool costWritten_[kProfileSlots] = {};
    float lastCost_ = -1.f;
    float lastHistDiff_ = 0.f;
    VkDescriptorSet dsState_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> dsInterpReal_;       // [prev * N + cur]: dupReal_ output (the real-frame image)

    // Descriptor sets, indexed [parity][level] etc.
    std::vector<VkDescriptorSet> dsDownSrc_[2];       // [parity] per source (level 0)
    std::vector<VkDescriptorSet> dsDown_[2];          // [parity] per level > 0
    std::vector<VkDescriptorSet> dsMatch_[2][2];      // [parity][dir] per level
    std::vector<VkDescriptorSet> dsSmooth_[2];        // [dir] per level
    std::vector<VkDescriptorSet> dsRefine_[2][2];     // [parity][dir] per level
    VkDescriptorSet dsReduce_ = VK_NULL_HANDLE;
    AllocatedImage splatDummy_;              // 1x1: the synthesis's splat map without extrapolate_splat
    VkImageView splatMap_ = VK_NULL_HANDLE;  // bound at binding 6 of every synthesis set
    AllocatedImage prevBwd_;   // extrapolate_accel: the previous pair's finest backward flow
    std::vector<VkDescriptorSet> dsInterp_;           // [(prev * N + cur) * outputs + output]
    std::vector<VkDescriptorSet> dsUpscale_;          // [source]; only when upscaling_
    std::vector<VkDescriptorSet> dsCas_;              // [output] generated frames; only when sharpen_
    VkDescriptorSet dsCasReal_ = VK_NULL_HANDLE;      // real frame; only when sharpen_
};

} // namespace bdex
