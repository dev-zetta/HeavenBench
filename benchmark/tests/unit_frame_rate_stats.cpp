/*
 * HeavenBench — live frame-rate summary unit tests
 */
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <vector>

#include "benchmark/stats.h"
#include "benchmark/stats_collector.h"

namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

bool approx(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

bool unit_frame_rate_stats() {
    bool ok = true;

    {
        const std::vector<float> intervals(60, 1.0f / 60.0f);
        const heaven::FrameRateSample sample =
            heaven::summarize_frame_rate(intervals);
        if (!approx(sample.fps, 60.0f, 0.01f) ||
            !approx(sample.average_frame_ms, 1000.0f / 60.0f, 0.01f) ||
            !approx(sample.one_percent_low_fps, 60.0f, 0.01f)) {
            std::printf(
                "[unit_frame_rate_stats] FAILED: constant 60 FPS summary "
                "was fps=%.3f frame=%.3fms low=%.3f\n",
                static_cast<double>(sample.fps),
                static_cast<double>(sample.average_frame_ms),
                static_cast<double>(sample.one_percent_low_fps));
            ok = false;
        }
    }

    {
        // 99 fast frames plus one 100 ms hitch: mean throughput uses total
        // elapsed time, while the slowest 1% consists of the hitch.
        std::vector<float> intervals(99, 0.01f);
        intervals.push_back(0.1f);
        const heaven::FrameRateSample sample =
            heaven::summarize_frame_rate(intervals);
        if (!approx(sample.fps, 100.0f / 1.09f, 0.01f) ||
            !approx(sample.one_percent_low_fps, 10.0f, 0.01f)) {
            std::printf(
                "[unit_frame_rate_stats] FAILED: hitch summary was "
                "fps=%.3f low=%.3f\n",
                static_cast<double>(sample.fps),
                static_cast<double>(sample.one_percent_low_fps));
            ok = false;
        }
    }

    {
        const std::vector<float> invalid{
            0.0f, -1.0f, std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN()};
        const heaven::FrameRateSample sample =
            heaven::summarize_frame_rate(invalid);
        if (sample.fps != 0.0f || sample.average_frame_ms != 0.0f ||
            sample.one_percent_low_fps != 0.0f) {
            std::printf(
                "[unit_frame_rate_stats] FAILED: invalid-only input "
                "did not produce a zero summary\n");
            ok = false;
        }
    }

    {
        // Measurement must retain the whole hitch, whereas simulation time is
        // independently capped. Invalid intervals must not poison totals or
        // increase frames beyond the valid sample count used for scoring.
        heaven::StatsCollector collector;
        collector.add_frame(0.25f);
        collector.add_frame(1.0f);
        collector.add_frame(0.0f);
        collector.add_frame(-1.0f);
        collector.add_frame(std::numeric_limits<float>::infinity());
        collector.add_frame(std::numeric_limits<float>::quiet_NaN());
        if (collector.frames() != 2 || collector.total_time() != 1.25 ||
            collector.min_fps() != 1.0f || collector.max_fps() != 4.0f) {
            std::printf(
                "[unit_frame_rate_stats] FAILED: measured hitches were "
                "clamped or invalid intervals affected the statistics\n");
            ok = false;
        }
    }

    return ok;
}

const heaven::testing::Register kRegister(unit_frame_rate_stats);

} // namespace
