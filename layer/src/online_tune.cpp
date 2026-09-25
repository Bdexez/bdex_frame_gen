#include "online_tune.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace bdex {

// Tag layout: bit 31 skip (first flow of a block: its temporal inputs came
// from the other setting), bits 24..30 candidate, 16..23 round, 0..15 block.
static uint32_t makeTag(size_t candidate, uint32_t round, int block) {
    return (uint32_t(candidate & 0x7f) << 24) | ((round & 0xff) << 16) | (uint32_t(block) & 0xffff);
}

OnlineTune::OnlineTune(std::vector<Candidate> candidates)
    : candidates_(std::move(candidates)), decisions_(candidates_.size()), decided_(candidates_.size(), false) {
    for (size_t i = 0; i < candidates_.size(); ++i) {
        decisions_[i].key = candidates_[i].key;
        decisions_[i].value = candidates_[i].value;
    }
    current_ = 0;
    while (current_ < candidates_.size() && decided_[current_]) ++current_;
}

std::vector<std::pair<std::string, std::string>> OnlineTune::keptLocked() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (size_t i = 0; i < decisions_.size(); ++i)
        if (decided_[i] && decisions_[i].kept) out.emplace_back(decisions_[i].key, decisions_[i].value);
    return out;
}

std::vector<std::pair<std::string, std::string>> OnlineTune::kept() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return keptLocked();
}

bool OnlineTune::trialIsSynthesis() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_ < candidates_.size() && candidates_[current_].synthesis;
}

bool OnlineTune::done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_ >= candidates_.size();
}

std::vector<OnlineTune::Decision> OnlineTune::decisions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Decision> out;
    for (size_t i = 0; i < decisions_.size(); ++i)
        if (decided_[i]) out.push_back(decisions_[i]);
    return out;
}

uint32_t OnlineTune::nextFlow(std::vector<std::pair<std::string, std::string>>& overrides) {
    std::lock_guard<std::mutex> lock(mutex_);
    overrides = keptLocked();
    if (current_ >= candidates_.size()) return kSkip;
    if (on(block_)) overrides.emplace_back(candidates_[current_].key, candidates_[current_].value);
    uint32_t tag = makeTag(current_, round_, block_);
    if (frame_ == 0) tag |= kSkip;
    if (++frame_ >= kBlockFrames) {
        frame_ = 0;
        ++block_;
        if (block_ >= kMaxBlocks) {
            // Not enough motion to decide in this many blocks: start the
            // count again (the samples so far are kept only if recent).
            block_ = 0;
            ++round_;
            blocks_.clear();
        }
    }
    return tag;
}

void OnlineTune::addSample(uint32_t tag, double mcSum, double zeroSum, double count, double flowChange,
                           double flowMagnitude, double flowCount) {
    if (tag & kSkip || !(count > 0.0)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t cand = (tag >> 24) & 0x7f;
    const uint32_t round = (tag >> 16) & 0xff;
    const int block = int(tag & 0xffff);
    if (cand != current_ || round != (round_ & 0xff) || current_ >= candidates_.size()) return;
    if (zeroSum / count < kMinZero) return;  // no motion: no information
    if (blocks_.size() <= size_t(block)) blocks_.resize(block + 1);
    Block& b = blocks_[block];
    b.mc += mcSum;
    b.zero += zeroSum;
    ++b.samples;
    if (flowCount > 0.0) {
        b.change += flowChange;
        b.magnitude += flowMagnitude;
        b.flowSamples += flowCount;
    }
    decideLocked();
}

void OnlineTune::decideLocked() {
    // Complete pairs: both blocks over (the trial has moved past them) and
    // with enough frames in motion.
    const int minSamples = kBlockFrames / 2;
    std::vector<double> gains, steadier;
    for (size_t p = 0; p + 1 < blocks_.size(); p += 2) {
        const Block& a = blocks_[p];
        const Block& b = blocks_[p + 1];
        if (int(p + 1) >= block_ || a.samples < minSamples || b.samples < minSamples) continue;
        const Block& off = on(int(p)) ? b : a;
        const Block& onb = on(int(p)) ? a : b;
        const double rOff = off.mc / off.zero, rOn = onb.mc / onb.zero;
        if (!(rOff > 0.0) || !std::isfinite(rOn)) continue;
        // An absolute difference of ratios (a share of the no-motion error):
        // relative changes of a near-perfect prediction are noise that
        // looks large (0.003 -> 0.006 is +100 % and invisible).
        gains.push_back(rOff - rOn);
        // Stability: the mean change relative to the motion (+1 flow pixel,
        // so still content does not divide by zero).
        if (off.flowSamples > 0.0 && onb.flowSamples > 0.0) {
            const double sOff = off.change / (off.magnitude + off.flowSamples);
            const double sOn = onb.change / (onb.magnitude + onb.flowSamples);
            // Relative, but a flow already this steady (changes below 5 % of
            // the motion) has nothing to gain or lose: without the floor a
            // near-still field turned noise into +1800 %.
            steadier.push_back((sOff - sOn) / std::max(sOff, 0.05));
        }
    }
    if (int(gains.size()) < kPairs) return;
    double mean = 0.0;
    int positive = 0;
    for (double g : gains) { mean += g; positive += g > 0.0; }
    mean /= double(gains.size());
    double stab = 0.0;
    int steadierPairs = 0;
    for (double g : steadier) { stab += g; steadierPairs += g > 0.0; }
    const bool measuredStability = int(steadier.size()) * 2 >= kPairs;
    if (!steadier.empty()) stab /= double(steadier.size());
    Decision& d = decisions_[current_];
    d.gain = mean;
    d.stability = measuredStability ? stab : 0.0;
    // A strong prediction gain stands even if the flow changes more: that is
    // what tracking an accelerating camera looks like next to a field stuck
    // at zero (zero_bias_contrast on a dark swinging scene: +3.4 dB, flow
    // change +1800 %). The stability veto only guards small gains.
    const bool predicts = mean >= kMinGain && positive * 4 >= int(gains.size()) * 3 &&
                          (!measuredStability || stab >= -kMaxStabilityLoss || mean >= kStrongGain);
    const bool steadies = measuredStability && stab >= kMinStability &&
                          steadierPairs * 4 >= int(steadier.size()) * 3 && mean >= -kMinGain / 2;
    d.kept = predicts || steadies;
    decided_[current_] = true;
    advanceLocked();
}

void OnlineTune::advanceLocked() {
    while (current_ < candidates_.size() && decided_[current_]) ++current_;
    block_ = frame_ = 0;
    ++round_;
    blocks_.clear();
}

bool OnlineTune::observe(double motion, double brightness) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ema = [](double& v, double x, double a) { v = v < 0.0 ? x : v + a * (x - v); };
    ema(fastMotion_, motion, 0.2);
    if (fastLuma_ == 0.0) fastLuma_ = brightness; else fastLuma_ += 0.2 * (brightness - fastLuma_);
    ++sinceRestart_;
    const bool trying = current_ < candidates_.size();
    if (trying || slowMotion_ < 0.0) {
        // While deciding, the reference follows the content the decisions see.
        ema(slowMotion_, motion, 0.05);
        if (slowLuma_ == 0.0) slowLuma_ = brightness; else slowLuma_ += 0.05 * (brightness - slowLuma_);
        deviant_ = 0;
        return false;
    }
    const double eps = kMinZero;
    const bool moving = fastMotion_ >= kMinZero;  // a still menu is no reason to re-tune
    const bool motionShift = std::fabs(std::log((fastMotion_ + eps) / (slowMotion_ + eps))) > std::log(3.0);
    const bool lumaShift = std::fabs(fastLuma_ - slowLuma_) > 0.2;
    deviant_ = (moving && (motionShift || lumaShift)) ? deviant_ + 1 : 0;
    if (deviant_ < kChangeSamples || sinceRestart_ < kRestartCooldown || candidates_.empty()) return false;
    for (size_t i = 0; i < decided_.size(); ++i) decided_[i] = false;
    current_ = 0;
    block_ = frame_ = 0;
    ++round_;
    blocks_.clear();
    slowMotion_ = fastMotion_;
    slowLuma_ = fastLuma_;
    deviant_ = 0;
    sinceRestart_ = 0;
    ++restarts_;
    return true;
}

int OnlineTune::restarts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return restarts_;
}

std::string OnlineTune::serialize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream o;
    for (size_t i = 0; i < decisions_.size(); ++i)
        if (decided_[i])
            o << decisions_[i].key << ' ' << decisions_[i].value << ' ' << (decisions_[i].kept ? "kept" : "dropped")
              << ' ' << decisions_[i].gain << ' ' << decisions_[i].stability << '\n';
    return o.str();
}

bool OnlineTune::deserialize(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::istringstream in(text);
    std::string line;
    bool any = false;
    while (std::getline(in, line)) {
        std::istringstream l(line);
        std::string key, value, verdict;
        double gain = 0.0;
        if (!(l >> key >> value >> verdict >> gain)) continue;
        double stability = 0.0;
        l >> stability;  // absent in caches from before the stability measure
        if (verdict != "kept" && verdict != "dropped") continue;
        for (size_t i = 0; i < candidates_.size(); ++i) {
            // A cached verdict for another trial value does not apply.
            if (candidates_[i].key != key || candidates_[i].value != value) continue;
            decisions_[i].kept = verdict == "kept";
            decisions_[i].gain = gain;
            decisions_[i].stability = stability;
            decided_[i] = true;
            any = true;
        }
    }
    if (any) {
        current_ = 0;
        advanceLocked();
    }
    return any;
}

std::string OnlineTune::summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    char buf[160];
    for (size_t i = 0; i < decisions_.size(); ++i) {
        if (!decided_[i]) continue;
        // The gain is a decrease of the error: shown as the error change.
        snprintf(buf, sizeof(buf), "%s%s=%s %s (%+.1f %%", out.empty() ? "" : ", ", decisions_[i].key.c_str(),
                 decisions_[i].value.c_str(), decisions_[i].kept ? "kept" : "dropped", -100.0 * decisions_[i].gain);
        out += buf;
        if (decisions_[i].stability != 0.0) {
            snprintf(buf, sizeof(buf), ", flow change %+.0f %%", -100.0 * decisions_[i].stability);
            out += buf;
        }
        out += ")";
    }
    return out;
}

} // namespace bdex
