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
    // `outputs` is the number of generated frames per real frame (multiplier
    // minus one); 0 creates a pass-through instance with only the history
    // images and the copy helpers.
    FrameGen(DeviceData& dev, VkFormat format, VkExtent2D extent, uint32_t outputs);
    ~FrameGen();
    FrameGen(const FrameGen&) = delete;
    FrameGen& operator=(const FrameGen&) = delete;

    // Copies `src` (a swapchain-like image in PRESENT_SRC layout) into the
    // history slot `parity`; returns `src` to PRESENT_SRC layout.
    void recordCopyToHistory(VkCommandBuffer cmd, uint32_t parity, VkImage src);
    // recordCopyToHistory + rebuild of the luma pyramid of that slot.
    void recordAnalysis(VkCommandBuffer cmd, uint32_t parity, VkImage src);
    // Computes the flow fields between history[1-parity] (previous frame) and
    // history[parity] (current frame). Requires both to have been analysed.
    void recordFlow(VkCommandBuffer cmd, uint32_t parity);
    // Writes the frame at time t (between previous and current) into the
    // internal output image `output`.
    void recordInterpolate(VkCommandBuffer cmd, uint32_t parity, float t, uint32_t output);
    // Copies output image `output` into `dst` (UNDEFINED -> PRESENT_SRC).
    void recordCopyOutput(VkCommandBuffer cmd, uint32_t output, VkImage dst);
    // Copies history[parity] into `dst` (UNDEFINED -> PRESENT_SRC).
    void recordCopyHistory(VkCommandBuffer cmd, uint32_t parity, VkImage dst);

    // Debug frame dumping: records a copy of the image that was just written
    // to `dst` (the generated output or the current history) into a staging
    // buffer; writeDump() decodes it to a PPM file once the GPU is done.
    // `which`: 0 = first generated output, 1 = history[parity] (the real frame).
    void recordDump(VkCommandBuffer cmd, uint32_t which, uint32_t parity);
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
    VkExtent2D extent_;
    OutputEncoding enc_;
    int levels_ = 0;
    float flowScale_ = 1.f;
    VkFormat lumaFormat_ = VK_FORMAT_R32_SFLOAT;

    AllocatedImage history_[2];
    std::vector<Level> levels_v_;
    std::vector<AllocatedImage> out_;
    uint32_t outputs_ = 0;
    AllocatedBuffer costBuf_;
    AllocatedBuffer dumpBuf_[2];
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kProfileSlots = 8;
    double profileMs_[StageCount]{};
    uint32_t profileFrames_[StageCount]{};
    bool profileWritten_[kProfileSlots][StageCount + 1]{};
    float timestampPeriodNs_ = 1.f;
    void* dumpMapped_[2]{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;

    Pass downsample_, blockMatch_, smooth_, refine_, reduce_, interp_;

    // Descriptor sets, indexed [parity][level] etc.
    std::vector<VkDescriptorSet> dsDown_[2];          // per level
    std::vector<VkDescriptorSet> dsMatch_[2][2];      // [parity][dir] per level
    std::vector<VkDescriptorSet> dsSmooth_[2];        // [dir] per level
    std::vector<VkDescriptorSet> dsRefine_[2][2];     // [parity][dir] per level
    VkDescriptorSet dsReduce_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> dsInterp_[2];        // [parity] per output
};

} // namespace bdex
