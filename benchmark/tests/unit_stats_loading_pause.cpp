/* HeavenBench — modal loading retires timing without discarding samples. */
#include "benchmark/stats.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}

namespace {
bool require(bool value, const char* message) {
    if (!value) std::fprintf(stderr, "loading clock test: %s\n", message);
    return value;
}
bool modal_loading_does_not_enter_presented_intervals() {
    heaven::BenchmarkStats stats;
    auto now = heaven::BenchmarkStats::Clock::time_point{} + std::chrono::seconds(1);
    stats.reset_frame_clock(now);
    if (!require(stats.record_presented_frame(now) == 0, "first frame must establish baseline")) return false;
    for (int frame = 0; frame < 2; ++frame) {
        now += std::chrono::milliseconds(16);
        static_cast<void>(stats.begin_frame(now));
        if (!require(std::abs(stats.record_presented_frame(now) - 0.016f) < 1e-7f,
                     "accepted interval before loading changed")) return false;
    }
    const auto prior_samples = stats.frame_times();
    now += std::chrono::seconds(30);
    stats.reset_frame_clock(now);
    if (!require(stats.frame_times() == prior_samples && stats.raw_frame_delta() == 0,
                 "loading reset must retain accepted samples and clear raw delta")) return false;
    now += std::chrono::milliseconds(10);
    if (!require(std::abs(stats.begin_frame(now) - 0.010f) < 1e-7f &&
                 std::abs(stats.raw_frame_delta() - 0.010f) < 1e-7f,
                 "simulation clock inherited loading time") ||
            !require(stats.record_presented_frame(now) == 0 && stats.frame_times() == prior_samples,
                     "first resumed presentation must exclude the loading interval")) return false;
    now += std::chrono::milliseconds(16);
    const float interval = stats.record_presented_frame(now);
    return require(std::abs(interval - 0.016f) < 1e-7f &&
                   stats.frame_count() == prior_samples.size() + 1 &&
                   std::abs(stats.frame_times().back() - 0.016f) < 1e-7f,
                   "subsequent resumed frame must append one normal accepted interval");
}
const heaven::testing::Register kRegister(modal_loading_does_not_enter_presented_intervals);
} // namespace
