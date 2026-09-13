#pragma once
#include "device.h"
#include "format.h"

#include <string>
#include <vector>

namespace bdex {

// GPU side of the frame generation for one swapchain: history images, luma
// pyramids, flow fields, the compute pipelines that produce them, and the
// command recording that ties everything together.
class FrameGen {
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
    FrameGen(DeviceData& dev, VkFormat format, VkExtent2D renderExtent, VkExtent2D displayExtent, uint32_t outputs,
             const std::vector<VkImage>& sources);
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
    // Copies output image `output` into `dst` (UNDEFINED -> PRESENT_SRC).
    void recordCopyOutput(VkCommandBuffer cmd, uint32_t output, VkImage dst);
    // Copies source `src` (PRESENT_SRC) into `dst` (UNDEFINED -> PRESENT_SRC).
    // When upscaling, `dst` receives the upscaled real frame instead (prepared
    // by recordUpscaleReal in the synthesis command buffer).
    void recordCopySource(VkCommandBuffer cmd, uint32_t src, VkImage dst);
    // Upscales source `cur` (in GENERAL, i.e. between acquire/release) to the
    // display resolution, into the internal real-frame image. Only used when
    // upscaling; recorded in the synthesis command buffer.
    void recordUpscaleReal(VkCommandBuffer cmd, uint32_t cur);

    bool upscaling() const { return upscaling_; }
    // The real frame is produced into an internal packed image (for upscaling
    // or the HUD) rather than copied straight through.
    bool packReal() const { return packReal_; }
    // Current frame-rate figures for the on-screen HUD.
    void setHud(int gameFps, int outFps) { hudGame_ = gameFps; hudOut_ = outFps; }

    // Debug frame dumping: records a copy of the image that was just written
    // to `dst` (the generated output or the current history) into a staging
    // buffer; writeDump() decodes it to a PPM file once the GPU is done.
    // `which`: 0 = first generated output, 1 = source `cur` (the real frame,
    // which must be between recordAcquireSources and recordReleaseSources).
    void recordDump(VkCommandBuffer cmd, uint32_t which, uint32_t cur);
    bool writeDump(const std::string& path, uint32_t which);

    int levels() const { return levels_; }

    // GPU profiling (config.profile): timestamps are written around each
    // stage of a slot's command buffer and collected once its fence signalled.
    enum Stage { StageAnalysis = 0, StageFlow, StageInterp, StageCopy, StageCount };
    void profileBegin(VkCommandBuffer cmd, uint32_t slot);
    void profileMark(VkCommandBuffer cmd, uint32_t slot, Stage stage);
    void profileCollect(uint32_t slot);
    // Average GPU milliseconds per stage since the last call; resets the accumulators.
    std::string profileReport();

private:
    struct Level {
        VkExtent2D size{};    // luma resolution
        VkExtent2D blocks{};  // flow resolution
        AllocatedImage pyr[2];
        VkExtent2D fine{};    // refined flow resolution
        bool refined = false; // flowFine is a distinct, refined image (else it aliases flow)
        AllocatedImage flowRaw[2];  // [0] forward, [1] backward
        AllocatedImage flow[2];     // smoothed (median)
        AllocatedImage flowFine[2]; // refined, consumed by the next level and the interpolation
        AllocatedImage cost[2];
    };
    struct Pass {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    void recordCas(VkCommandBuffer cmd, VkDescriptorSet set);
    void createPipelines();
    void createResources();
    void createDescriptors();
    void destroyAll();
    Pass makePass(const uint32_t* spv, size_t spvSize, const std::vector<VkDescriptorType>& bindings);
    VkDescriptorSet allocSet(VkDescriptorSetLayout layout);
    void writeImage(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView view);
    void writeBuffer(VkDescriptorSet set, uint32_t binding, VkBuffer buffer);
    void memoryBarrier(VkCommandBuffer cmd);
    void copyToPresent(VkCommandBuffer cmd, VkImage src, VkImageLayout srcLayout, VkAccessFlags srcAccess,
                       VkPipelineStageFlags srcStage, VkImage dst);

    DeviceData& dev_;
    VkFormat format_;
    VkExtent2D extent_;         // render resolution (source images, flow pyramid)
    VkExtent2D displayExtent_;  // presented resolution (== extent_ unless upscaling)
    bool upscaling_ = false;
    bool sharpen_ = false;      // contrast-adaptive sharpening pass after upscaling
    bool overlay_ = false;      // draw the on-screen fps HUD
    bool packReal_ = false;     // real frame goes through the internal packed image (upscaling or HUD)
    int hudGame_ = 0, hudOut_ = 0;
    OutputEncoding enc_;
    int levels_ = 0;
    float flowScale_ = 1.f;
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
    AllocatedBuffer costBuf_;
    AllocatedBuffer dumpBuf_[2];
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kProfileSlots = 12;  // application slots + worker slots
    double profileMs_[StageCount]{};
    uint32_t profileFrames_[StageCount]{};
    bool profileWritten_[kProfileSlots][StageCount + 1]{};
    float timestampPeriodNs_ = 1.f;
    void* dumpMapped_[2]{};
    AllocatedBuffer costReadback_;
    void* costMapped_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;

    Pass downsample_, blockMatch_, smooth_, refine_, reduce_, interp_, upscale_, cas_;

    // Descriptor sets, indexed [parity][level] etc.
    std::vector<VkDescriptorSet> dsDownSrc_[2];       // [parity] per source (level 0)
    std::vector<VkDescriptorSet> dsDown_[2];          // [parity] per level > 0
    std::vector<VkDescriptorSet> dsMatch_[2][2];      // [parity][dir] per level
    std::vector<VkDescriptorSet> dsSmooth_[2];        // [dir] per level
    std::vector<VkDescriptorSet> dsRefine_[2][2];     // [parity][dir] per level
    VkDescriptorSet dsReduce_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> dsInterp_;           // [(prev * N + cur) * outputs + output]
    std::vector<VkDescriptorSet> dsUpscale_;          // [source]; only when upscaling_
    std::vector<VkDescriptorSet> dsCas_;              // [output] generated frames; only when sharpen_
    VkDescriptorSet dsCasReal_ = VK_NULL_HANDLE;      // real frame; only when sharpen_
};

} // namespace bdex
