#pragma once

// Source present times from FrameRecorder. Kept independent of Vulkan so
// malformed recordings and late replay frames can be checked without a GPU.
#include <cmath>
#include <istream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bdex::replay {

inline std::vector<double> readTimes(std::istream& in, const std::vector<std::string>& names) {
    auto fail = [] { throw std::runtime_error("--replay-timing: invalid or incomplete timestamps.csv"); };
    auto number = [&](const std::string& s) {
        std::istringstream value(s);
        value.imbue(std::locale::classic());
        double result = 0.0;
        if (!(value >> result) || !std::isfinite(result)) fail();
        value >> std::ws;
        if (!value.eof()) fail();
        return result;
    };
    std::string line;
    auto getLine = [&] {
        if (!std::getline(in, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    if (!getLine() || line != "frame,present_ms,interval_ms") fail();
    std::vector<double> times;
    double first = 0.0, previous = 0.0;
    while (getLine()) {
        const size_t a = line.find(','), b = a == std::string::npos ? a : line.find(',', a + 1);
        if (a == std::string::npos || b == std::string::npos || line.find(',', b + 1) != std::string::npos ||
            times.size() >= names.size() || line.substr(0, a) != names[times.size()]) fail();
        const double present = number(line.substr(a + 1, b - a - 1));
        const double interval = number(line.substr(b + 1));
        if (times.empty()) {
            if (interval != 0.0) fail();
            first = present;
        } else if (present <= previous || std::abs(present - previous - interval) > 0.0021) fail();
        const double offset = present - first;
        if (!std::isfinite(offset)) fail();
        times.push_back(offset);
        previous = present;
    }
    if (in.bad() || times.empty() || times.size() != names.size()) fail();
    return times;
}

// Keep the recorded intervals. If preparing/presenting a frame takes too
// long, shift the following deadlines instead of emitting a catch-up burst.
// Small wake-up jitter does not move the clock. commit() returns the shift
// so a benchmark can distinguish the requested cadence from replay overload.
struct Clock {
    double originMs = std::numeric_limits<double>::quiet_NaN();
    double target(double sourceOffsetMs, double nowMs) {
        if (!std::isfinite(originMs)) originMs = nowMs - sourceOffsetMs;
        return originMs + sourceOffsetMs;
    }
    double commit(double sourceOffsetMs, double actualMs) {
        const double late = actualMs - (originMs + sourceOffsetMs);
        if (late <= 0.5) return 0.0;
        originMs += late;
        return late;
    }
};

} // namespace bdex::replay
