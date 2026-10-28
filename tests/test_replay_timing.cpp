#include "../demo/replay_timing.h"
#include "test.h"

#include <sstream>
#include <utility>

namespace {
const std::vector<std::string> names{"f0000.ppm", "f0001.ppm", "f0002.ppm", "f0003.ppm"};
const char* const header = "frame,present_ms,interval_ms\n";
bool rejects(const std::string& csv) {
    std::istringstream input(csv);
    try { bdex::replay::readTimes(input, names); }
    catch (const std::runtime_error&) { return true; }
    return false;
}
}

TEST(replay_timing_preserves_irregular_source_clock) {
    // Rounded recorder values, CRLF, and a large absolute monotone clock.
    std::istringstream input(std::string(header) +
        "f0000.ppm,1450000000.000,0\r\n"
        "f0001.ppm,1450000033.333,33.333\r\n"
        "f0002.ppm,1450000113.334,80.000\r\n"
        "f0003.ppm,1450000133.334,20.000\r\n");
    const auto times = bdex::replay::readTimes(input, names);
    CHECK(times.size() == 4);
    CHECK(std::abs(times[1] - 33.333) < 0.0001);
    CHECK(std::abs(times[2] - 113.334) < 0.0001);
    CHECK(std::abs(times[3] - times[1] - 100.001) < 0.0001);  // start 1, step 2
}

TEST(replay_timing_rejects_unusable_or_mismatched_recordings) {
    const std::string good = std::string(header) +
        "f0000.ppm,100,0\nf0001.ppm,130,30\nf0002.ppm,210,80\nf0003.ppm,230,20\n";
    CHECK(rejects(""));
    CHECK(rejects(header));
    CHECK(rejects("frame,time\nf0000.ppm,100\n"));
    for (const auto& bad : std::vector<std::pair<std::string, std::string>>{
             {"f0000.ppm,100,0", "f0000.ppm,100,1"},
             {"f0001.ppm,130,30", "f0000.ppm,130,30"},
             {"f0001.ppm,130,30", "f0002.ppm,130,30"},
             {"f0001.ppm,130,30", "f0001.ppm,100,0"},
             {"f0001.ppm,130,30", "f0001.ppm,130,20"},
             {"f0001.ppm,130,30", "f0001.ppm,nan,30"},
             {"f0001.ppm,130,30", "f0001.ppm,inf,30"},
             {"f0001.ppm,130,30", "f0001.ppm,130ms,30"},
             {"f0001.ppm,130,30", "f0001.ppm,130,30,7"},
             {"f0003.ppm,230,20\n", ""}}) {
        auto csv = good;
        csv.replace(csv.find(bad.first), bad.first.size(), bad.second);
        CHECK(rejects(csv));
    }
    CHECK(rejects(good + "f0004.ppm,250,20\n"));
}

TEST(replay_timing_does_not_catch_up_in_a_burst_after_overload) {
    bdex::replay::Clock clock;
    CHECK(clock.target(0, 1000) == 1000);
    CHECK(clock.commit(0, 1000) == 0);
    CHECK(clock.target(30, 1010) == 1030);
    CHECK(clock.commit(30, 1030.1) == 0);  // small wake jitter, no accumulating drift
    CHECK(clock.target(110, 1040) == 1110);
    CHECK(clock.commit(110, 1160) == 50);  // disk/GPU/OS delay remains visible in the log
    CHECK(clock.target(130, 1161) == 1180);  // keep the following 20 ms source interval
    CHECK(clock.commit(130, 1180) == 0);
    CHECK(clock.target(160, 1181) == 1210);
}
