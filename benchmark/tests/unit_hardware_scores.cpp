/*
 * HeavenBench — score-v3 complete hardware result tests
 */
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "benchmark/score_calculator.h"

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool close_to(double actual, double expected, double tolerance = 1e-6) {
    return std::abs(actual - expected) <= tolerance;
}

bool unit_hardware_scores() {
    constexpr float kSixtyFpsInterval = 1.0f / 60.0f;
    constexpr double kSixtyFpsGpuMs = 1000.0 / 60.0;

    std::vector<float> frames(100, kSixtyFpsInterval);
    std::vector<std::vector<float>> segments{
        std::vector<float>(50, kSixtyFpsInterval),
        std::vector<float>(50, kSixtyFpsInterval),
    };
    std::vector<double> gpu_times(100, kSixtyFpsGpuMs);

    heaven::CpuBenchmarkMetrics cpu;
    cpu.updates_per_second = heaven::kCpuReferenceRate;
    cpu.timing = heaven::summarize_timings(gpu_times);

    const heaven::HardwareScoreResult stable =
        heaven::compute_hardware_scores(frames, segments, cpu, gpu_times);

    bool ok =
        stable.score_version == heaven::kScoreVersion &&
        close_to(stable.metrics.average_fps, 60.0, 1e-4) &&
        close_to(stable.metrics.path_geomean_fps, 60.0, 1e-4) &&
        close_to(stable.metrics.one_percent_low_fps, 60.0, 1e-4) &&
        close_to(stable.metrics.effective_fps, 60.0, 1e-4) &&
        close_to(stable.metrics.gpu_equivalent_fps, 60.0, 1e-9) &&
        close_to(stable.scores.overall, 1000.0, 1e-3) &&
        close_to(stable.scores.cpu, 1000.0) &&
        close_to(stable.scores.gpu, 1000.0);

    // One long frame is exactly the slowest 1% of this sample. It must reduce
    // the experience-based overall score without changing the independent CPU
    // or GPU component scores.
    frames.back() = 0.1f;
    segments.back().back() = 0.1f;
    const heaven::HardwareScoreResult hitched =
        heaven::compute_hardware_scores(frames, segments, cpu, gpu_times);
    ok = ok &&
        close_to(hitched.metrics.one_percent_low_fps, 10.0, 1e-6) &&
        hitched.scores.overall < stable.scores.overall &&
        close_to(hitched.scores.cpu, stable.scores.cpu) &&
        close_to(hitched.scores.gpu, stable.scores.gpu);

    std::vector<std::vector<float>> incomplete_segments = segments;
    incomplete_segments.front().clear();
    const heaven::HardwareScoreResult incomplete =
        heaven::compute_hardware_scores(
            frames, incomplete_segments, cpu, gpu_times);
    ok = ok &&
        close_to(incomplete.metrics.path_geomean_fps, 0.0) &&
        close_to(incomplete.scores.overall, 0.0);

    if (!ok) {
        std::printf(
            "[unit_hardware_scores] FAILED: stable scores %.6f/%.6f/%.6f, "
            "rates %.6f/%.6f/%.6f/%.6f; hitched overall %.6f, low %.6f\n",
            stable.scores.overall, stable.scores.cpu, stable.scores.gpu,
            stable.metrics.average_fps, stable.metrics.path_geomean_fps,
            stable.metrics.one_percent_low_fps,
            stable.metrics.gpu_equivalent_fps,
            hitched.scores.overall,
            hitched.metrics.one_percent_low_fps);
    }
    return ok;
}

const heaven::testing::Register reg_unit_hardware_scores{
    unit_hardware_scores};

} // namespace
