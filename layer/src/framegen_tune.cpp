// FrameGen: online_tune — the self-evaluation pass and the per-game trial
// (see online_tune.h).
#include "framegen.h"
#include "framegen_internal.h"
#include "log.h"
#include "online_tune.h"
#include "platform.h"
#include "shaders.h"
#include "vk_util.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bdex {
namespace fg {

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
std::shared_ptr<OnlineTune> createOnlineTune(const Config& cfg) {
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

} // namespace fg

using namespace fg;

void FrameGen::recordSelfEval(VkCommandBuffer cmd, uint32_t parity) {
    auto& vt = dev_.vt;
    const uint32_t slot = evalSlot_;
    if (slot >= kProfileSlots || evalPending_[slot]) return;
    const Level& l0 = levels_v_[0];
    const uint32_t block = l0.refined ? kFineBlock : kBlockSize;
    // At most 2^18 samples, so the fixed-point sums cannot overflow.
    uint32_t step = 2;
    while (uint64_t(divUp(l0.size.width, step)) * divUp(l0.size.height, step) > (1u << 18)) ++step;
    vt.CmdFillBuffer(cmd, evalBuf_.buffer, VkDeviceSize(slot) * 16, 16, 0);
    memoryBarrier(cmd);
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selfEval_.pipeline);
    SelfEvalPC pc{{(int32_t)l0.size.width, (int32_t)l0.size.height},
                  {float(l0.fine.width * block), float(l0.fine.height * block)}, (int32_t)slot, (int32_t)step};
    vt.CmdPushConstants(cmd, selfEval_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selfEval_.layout, 0, 1, &dsEval_[parity], 0, nullptr);
    vt.CmdDispatch(cmd, divUp(divUp(l0.size.width, step), 16), divUp(divUp(l0.size.height, step), 16), 1);
    // The matching then rewrites the flow it read; the host reads the sums.
    memoryBarrier(cmd);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    evalPending_[slot] = true;
    evalTag_[slot] = tuneTag_;
}

void FrameGen::collectSelfEval(uint32_t slot) {
    if (slot >= kProfileSlots || !evalPending_[slot]) return;
    evalPending_[slot] = false;
    const auto* w = static_cast<const uint32_t*>(evalMapped_) + slot * 4;
    if (w[2] > 0) tune_->addSample(evalTag_[slot], w[0] / 4096.0, w[1] / 4096.0, double(w[2]));
    reportOnlineTune();
}

// Logs each new decision and caches the decisions made so far for the game.
void FrameGen::reportOnlineTune() {
    std::lock_guard<std::mutex> lock(dev_.onlineTuneMutex);
    const auto decisions = tune_->decisions();
    if (decisions.size() == dev_.onlineTuneReported) return;
    for (size_t i = dev_.onlineTuneReported; i < decisions.size(); ++i)
        BDEX_INFO("online_tune: %s=%s %s (prediction error %+.1f %%)", decisions[i].key.c_str(),
                  decisions[i].value.c_str(), decisions[i].kept ? "kept" : "dropped", -100.0 * decisions[i].gain);
    dev_.onlineTuneReported = decisions.size();
    if (tune_->done()) BDEX_INFO("online_tune: done for %s: %s", (*cfgp_).tuneName.c_str(), tune_->summary().c_str());
    // Cached after each decision: a short session still spares the next one.
    const std::string path = onlineTuneCachePath(*cfgp_);
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    out << tune_->serialize();
    if (out && tune_->done()) BDEX_INFO("online_tune: decisions cached in %s", path.c_str());
}

} // namespace bdex
