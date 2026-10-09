/*
 * HeavenBench — Versioned whole-system benchmark scoring
 */
#include "benchmark/score_calculator.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace heaven {
namespace {

std::vector<double> positive_finite(std::span<const double> values) {
    std::vector<double> out;
    out.reserve(values.size());
    for (const double value : values) {
        if (value > 0.0 && std::isfinite(value)) out.push_back(value);
    }
    return out;
}

double percentile_nearest_rank(const std::vector<double>& sorted,
                               double percentile) {
    if (sorted.empty()) return 0.0;
    const auto rank = static_cast<std::size_t>(
        std::ceil(percentile * static_cast<double>(sorted.size())));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

double frame_rate(std::span<const float> intervals) {
    double elapsed = 0.0;
    std::size_t frames = 0;
    for (const float dt : intervals) {
        if (dt > 0.0f && std::isfinite(dt)) {
            elapsed += static_cast<double>(dt);
            ++frames;
        }
    }
    return elapsed > 0.0
        ? static_cast<double>(frames) / elapsed
        : 0.0;
}

double one_percent_low(std::span<const float> intervals) {
    std::vector<double> valid;
    valid.reserve(intervals.size());
    for (const float dt : intervals) {
        if (dt > 0.0f && std::isfinite(dt)) {
            valid.push_back(static_cast<double>(dt));
        }
    }
    if (valid.empty()) return 0.0;

    std::sort(valid.begin(), valid.end(), std::greater<>());
    const std::size_t count = std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::ceil(static_cast<double>(valid.size()) * 0.01)));
    const double elapsed =
        std::accumulate(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(count), 0.0);
    return elapsed > 0.0 ? static_cast<double>(count) / elapsed : 0.0;
}

double segment_geomean(
    std::span<const std::vector<float>> segment_intervals) {
    if (segment_intervals.empty()) return 0.0;

    double log_sum = 0.0;
    for (const auto& segment : segment_intervals) {
        const double fps = frame_rate(segment);
        // Skipping a path section must never improve a score. A comparable run
        // requires at least one accepted frame from every authored segment.
        if (fps <= 0.0 || !std::isfinite(fps)) return 0.0;
        log_sum += std::log(fps);
    }
    return std::exp(
        log_sum / static_cast<double>(segment_intervals.size()));
}

} // namespace

double score_from_rate(double rate_per_second) {
    if (!(rate_per_second > 0.0) || !std::isfinite(rate_per_second)) {
        return 0.0;
    }
    return kBaselinePoints * rate_per_second / kBaselineRate;
}

double score_from_cpu_rate(double updates_per_second) {
    if (!(updates_per_second > 0.0) ||
        !std::isfinite(updates_per_second)) {
        return 0.0;
    }
    return kBaselinePoints * updates_per_second / kCpuReferenceRate;
}

TimingDistribution summarize_timings(std::span<const double> samples_ms) {
    TimingDistribution out;
    std::vector<double> valid = positive_finite(samples_ms);
    if (valid.empty()) return out;

    const double total = std::accumulate(valid.begin(), valid.end(), 0.0);
    std::sort(valid.begin(), valid.end());

    out.average_ms = total / static_cast<double>(valid.size());
    out.p95_ms     = percentile_nearest_rank(valid, 0.95);
    out.p99_ms     = percentile_nearest_rank(valid, 0.99);
    out.samples    = valid.size();
    return out;
}

HardwareScoreResult compute_hardware_scores(
    std::span<const float> frame_intervals,
    std::span<const std::vector<float>> segment_frame_intervals,
    const CpuBenchmarkMetrics& cpu,
    std::span<const double> gpu_times_ms) {
    HardwareScoreResult out;

    out.metrics.average_fps         = frame_rate(frame_intervals);
    out.metrics.path_geomean_fps    = segment_geomean(segment_frame_intervals);
    out.metrics.one_percent_low_fps = one_percent_low(frame_intervals);
    out.metrics.cpu                 = cpu;

    if (out.metrics.path_geomean_fps > 0.0 &&
        out.metrics.one_percent_low_fps > 0.0) {
        out.metrics.effective_fps = std::sqrt(
            out.metrics.path_geomean_fps *
            out.metrics.one_percent_low_fps);
    }

    out.metrics.gpu = summarize_timings(gpu_times_ms);
    if (out.metrics.gpu.average_ms > 0.0) {
        out.metrics.gpu_equivalent_fps =
            1000.0 / out.metrics.gpu.average_ms;
    }

    out.scores.overall = score_from_rate(out.metrics.effective_fps);
    out.scores.cpu     = score_from_cpu_rate(cpu.updates_per_second);
    out.scores.gpu     = score_from_rate(out.metrics.gpu_equivalent_fps);
    return out;
}

double round_fps(double fps) {
    return std::round(fps * 10.0) / 10.0;
}

long long round_score(double score) {
    return std::llround(score);
}

} // namespace heaven
