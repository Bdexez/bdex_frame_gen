#include "online_tune.h"
#include "test.h"

#include <cmath>
#include <cstdint>

using namespace bdex;

namespace {

bool has(const std::vector<std::pair<std::string, std::string>>& o, const std::string& key) {
    for (const auto& kv : o)
        if (kv.first == key) return true;
    return false;
}

// Plays `frames` flows: the error ratio is 0.5 without options, times the
// factor of each active option; `motion` = false gives static frames. The
// flow's pair-to-pair change is 0.4 of the motion, times `steadyA` with "a".
// Samples come back two flows late, as from a GPU queue.
void play(OnlineTune& t, int frames, double factorA, double factorB, bool motion = true, double steadyA = 1.0) {
    uint32_t seed = 12345;
    std::vector<std::pair<uint32_t, std::pair<double, double>>> pending;
    for (int i = 0; i < frames; ++i) {
        std::vector<std::pair<std::string, std::string>> o;
        const uint32_t tag = t.nextFlow(o);
        double ratio = 0.5;
        if (has(o, "a")) ratio *= factorA;
        if (has(o, "b")) ratio *= factorB;
        seed = seed * 1664525u + 1013904223u;
        const double noise = 1.0 + 0.04 * (double(seed >> 8) / double(1u << 24) - 0.5);  // +-2 %
        // The scene's amount of motion drifts slowly: the ratio does not.
        const double zero = motion ? 0.02 * (1.0 + 0.5 * std::sin(i * 0.01)) : 0.0005;
        const double change = 0.4 * 5.0 * (has(o, "a") ? steadyA : 1.0) * noise;  // per sample, motion 4 px (+1)
        pending.emplace_back(tag, std::make_pair(ratio * noise * zero, change));
        if (pending.size() > 2) {
            const auto s = pending.front();
            pending.erase(pending.begin());
            t.addSample(s.first, s.second.first * 1000.0, zero * 1000.0, 1000.0, s.second.second * 1000.0, 4000.0, 1000.0);
        }
    }
}

} // namespace

TEST(online_tune_keeps_a_helpful_option_and_drops_a_neutral_one) {
    OnlineTune t({{"a", "1"}, {"b", "1"}});
    play(t, 2000, 0.9, 1.0);
    CHECK(t.done());
    const auto d = t.decisions();
    CHECK(d.size() == 2);
    CHECK(d[0].key == "a" && d[0].kept && d[0].gain > 0.03);
    CHECK(d[1].key == "b" && !d[1].kept);
    const auto k = t.kept();
    CHECK(k.size() == 1 && k[0].first == "a");
}

TEST(online_tune_drops_a_harmful_option) {
    OnlineTune t({{"a", "1"}});
    play(t, 2000, 1.1, 1.0);
    CHECK(t.done());
    CHECK(!t.decisions()[0].kept);
    CHECK(t.decisions()[0].gain < 0.0);
}

TEST(online_tune_waits_for_motion) {
    OnlineTune t({{"a", "1"}});
    play(t, 3000, 0.8, 1.0, false);
    CHECK(!t.done());
    CHECK(t.decisions().empty());
}

TEST(online_tune_decides_within_a_few_seconds) {
    // 8 pairs of 8-flow blocks: 128 flows, about 2 s at 60 fps.
    OnlineTune t({{"a", "1"}});
    play(t, 160, 0.9, 1.0);
    CHECK(t.done());
}

TEST(online_tune_trials_on_top_of_kept_options) {
    // "b" only helps together with "a": tried after "a" was kept, it is too.
    OnlineTune t({{"a", "1"}, {"b", "1"}});
    play(t, 2000, 0.9, 0.9);
    CHECK(t.kept().size() == 2);
}

TEST(online_tune_cache_round_trip) {
    OnlineTune t({{"a", "1"}, {"b", "1"}});
    play(t, 2000, 0.9, 1.0);
    const std::string text = t.serialize();
    OnlineTune u({{"a", "1"}, {"b", "1"}, {"c", "2"}});
    CHECK(u.deserialize(text));
    CHECK(!u.done());  // "c" is new: still tried
    CHECK(u.kept().size() == 1 && u.kept()[0].first == "a");
    std::vector<std::pair<std::string, std::string>> o;
    u.nextFlow(o);
    CHECK(has(o, "a"));
    // A different trial value is a different candidate.
    OnlineTune v({{"a", "2"}});
    CHECK(!v.deserialize(text));
    CHECK(!v.done());
    CHECK(!OnlineTune({{"a", "1"}}).deserialize("garbage\n"));
}

TEST(online_tune_summary) {
    OnlineTune t({{"a", "1"}});
    CHECK(t.summary().empty());
    t.deserialize("a 1 kept 0.042\n");
    CHECK(t.summary() == "a=1 kept (-4.2 %)");
    OnlineTune u({{"a", "1"}});
    u.deserialize("a 1 kept 0 0.25\n");
    CHECK(u.summary() == "a=1 kept (-0.0 %, flow change -25 %)");
}

TEST(online_tune_keeps_a_steadier_flow_at_equal_prediction) {
    // flow_temporal's case: same prediction error, a third less flow change.
    OnlineTune t({{"a", "1"}});
    play(t, 2000, 1.0, 1.0, true, 0.67);
    CHECK(t.done());
    CHECK(t.decisions()[0].kept && t.decisions()[0].stability > 0.2);
}

TEST(online_tune_drops_a_small_prediction_win_that_destabilises) {
    OnlineTune t({{"a", "1"}});
    play(t, 2000, 0.96, 1.0, true, 1.5);  // 2 % better, 50 % less stable
    CHECK(t.done());
    CHECK(!t.decisions()[0].kept);
}

TEST(online_tune_keeps_a_strong_prediction_win_whatever_the_stability) {
    OnlineTune t({{"a", "1"}});
    play(t, 2000, 0.8, 1.0, true, 3.0);  // 10 % better (of the no-motion error)
    CHECK(t.done());
    CHECK(t.decisions()[0].kept);
}

TEST(online_tune_reads_caches_without_stability) {
    OnlineTune t({{"a", "1"}});
    CHECK(t.deserialize("a 1 kept 0.05\n"));
    CHECK(t.kept().size() == 1 && t.decisions()[0].stability == 0.0);
}

TEST(online_tune_restarts_when_the_content_changes) {
    OnlineTune t({{"a", "1"}});
    play(t, 2000, 0.9, 1.0);
    CHECK(t.done());
    // Same kind of content: no restart, whatever the time.
    bool restarted = false;
    for (int i = 0; i < 1000; ++i) restarted |= t.observe(0.02, 0.4);
    CHECK(!restarted && t.done());
    // A still menu: no restart either.
    for (int i = 0; i < 1000; ++i) restarted |= t.observe(0.0002, 0.8);
    CHECK(!restarted && t.done());
    // Much faster content (and darker): restart after a sustained change.
    int at = -1;
    for (int i = 0; i < 1000 && at < 0; ++i)
        if (t.observe(0.12, 0.15)) at = i;
    CHECK(at >= OnlineTune::kChangeSamples - 1 && !t.done() && t.restarts() == 1);
    CHECK(t.kept().empty());  // back to the defaults while trying again
}

TEST(online_tune_restarts_at_most_once_per_cooldown) {
    OnlineTune t({{"a", "1"}});
    play(t, 2000, 0.9, 1.0);
    for (int i = 0; i < 1000; ++i) t.observe(0.02, 0.4);
    int n = 0;
    for (int i = 0; i < 200; ++i) n += t.observe(i % 60 < 30 ? 0.12 : 0.02, 0.4);
    CHECK(n <= 1);
}
