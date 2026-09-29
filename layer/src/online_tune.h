#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace bdex {

// online_tune: per-game automatic choice of the opt-in flow options, from a
// self-evaluation the engine runs while the game plays.
//
// The measure: when a real frame arrives, the previous pair's motion is used
// to predict it (constant velocity), and the prediction error is compared to
// the error of predicting "no motion" (the ratio `mc / zero`, lower is
// better). That is exactly what extrapolation relies on, and a motion field
// that predicts the next frame better also interpolates better. It needs
// nothing from the game and costs one small pass per frame.
//
// The choice: each candidate option is tried in alternating blocks of
// frames, on and off (ABBA order, so a slow change of the scene weighs on
// both), on top of the options already kept. Pairs of neighbouring blocks
// are compared; the option is kept when it lowers the error ratio by at
// least `kMinGain` (one hundredth of the no-motion error) on average and in
// three pairs out of four, without making the flow less stable — or when it
// makes the flow clearly steadier (`kMinStability`: its pair-to-pair change
// relative to the motion, 10 % lower, in three pairs out of four) without
// hurting the prediction. The prediction error alone cannot see flicker: a
// field that switches between equally good vectors predicts as well. Frames without motion
// carry no information and are skipped, so a static menu only delays the
// decision. Decisions are cached per game (serialize / deserialize) so the
// next session starts tuned.
//
// Pure logic, thread-safe (several swapchains may feed it), unit-tested in
// tests/test_online_tune.cpp.
class OnlineTune {
public:
    struct Candidate {
        std::string key, value;  // a Config key and the trial value
    };
    struct Decision {
        std::string key, value;
        double gain = 0.0;       // mean decrease of the error ratio (0.05 = 5 % of the no-motion error)
        double stability = 0.0;  // mean relative decrease of the flow's pair-to-pair change (0.1 = 10 % steadier)
        bool kept = false;
    };

    static constexpr int kBlockFrames = 8;    // flows per block
    static constexpr int kPairs = 8;          // block pairs compared per candidate
    static constexpr int kMaxBlocks = 96;     // give up waiting for motion and restart the count
    static constexpr double kMinGain = 0.01;  // error ratio at least 0.01 lower
    static constexpr double kMinZero = 0.002; // mean |frame difference| below which a frame has no motion
    static constexpr double kMinStability = 0.10;   // a clearly steadier flow
    static constexpr double kMaxStabilityLoss = 0.05; // what a prediction win may cost in stability

    explicit OnlineTune(std::vector<Candidate> candidates);

    // Tag of the next flow computation (to be passed back with its sample),
    // and the Config overrides to apply to it.
    uint32_t nextFlow(std::vector<std::pair<std::string, std::string>>& overrides);
    // A measurement of the flow computed with `tag`: summed errors over
    // `count` sampled pixels (any common scale), and — when the previous
    // pair's flow was at hand — the summed pair-to-pair flow change and
    // motion magnitude over `flowCount` samples (flow pixels).
    void addSample(uint32_t tag, double mcSum, double zeroSum, double count, double flowChange = 0.0,
                   double flowMagnitude = 0.0, double flowCount = 0.0);

    bool done() const;
    std::vector<Decision> decisions() const;
    // Overrides kept so far (all of them once done).
    std::vector<std::pair<std::string, std::string>> kept() const;
    // One line per decided candidate: "key value kept|dropped gain stability".
    std::string serialize() const;
    // Restores the decisions of a previous session: candidates found there
    // are decided, the others are still tried. False if nothing was read.
    bool deserialize(const std::string& text);
    // Summary for the log, e.g. "zero_bias_contrast=0.1 kept (-4.2 %), subpixel=1 dropped (+0.3 %)".
    std::string summary() const;

private:
    struct Block {
        double mc = 0.0, zero = 0.0;
        double change = 0.0, magnitude = 0.0, flowSamples = 0.0;
        int samples = 0;
    };
    static constexpr uint32_t kSkip = 1u << 31;
    bool on(int block) const { return ((block & 1) ^ ((block >> 1) & 1)) != 0; }
    void decideLocked();
    void advanceLocked();
    std::vector<std::pair<std::string, std::string>> keptLocked() const;

    mutable std::mutex mutex_;
    std::vector<Candidate> candidates_;
    std::vector<Decision> decisions_;  // one per candidate, in order
    std::vector<bool> decided_;
    size_t current_ = 0;               // candidate on trial (== size: done)
    int block_ = 0, frame_ = 0;        // position in the trial
    uint32_t round_ = 0;               // bumped when a trial restarts: stale samples are dropped
    std::vector<Block> blocks_;
};

} // namespace bdex
