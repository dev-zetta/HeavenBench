/*
 * HeavenBench — Versioned whole-system benchmark scoring
 *
 * Score version 3 uses explicit per-workload reference rates:
 *
 *     Overall / GPU: 1000 points == 60 frames / second
 *     CPU:           1000 points == 10,000 world updates / second
 *
 * A CPU update advances the deterministic simulation by 1/60 second, so the
 * CPU reference is 166.67x real-time. Separate references keep independently
 * meaningful workloads in a comparable score range.
 */
#ifndef HEAVENBENCH_SRC_BENCHMARK_SCORE_CALCULATOR_H_
#define HEAVENBENCH_SRC_BENCHMARK_SCORE_CALCULATOR_H_

#include <cstddef>
#include <span>
#include <vector>

namespace heaven {

inline constexpr int    kScoreVersion           = 3;
inline constexpr double kBaselinePoints         = 1000.0;
inline constexpr double kBaselineRate           = 60.0;
inline constexpr double kCpuReferenceRate       = 10'000.0;
inline constexpr double kCpuSimulationRate      = 60.0;

struct TimingDistribution {
    double average_ms = 0.0;
    double p95_ms     = 0.0;
    double p99_ms     = 0.0;
    std::size_t samples = 0;
};

struct CpuBenchmarkMetrics {
    double updates_per_second = 0.0;
    TimingDistribution timing;
    double checksum = 0.0;
    std::size_t scene_meshes = 0;
    std::size_t scene_instances = 0;
};

struct CpuStressMetrics {
    bool enabled = false;
    int workers = 0;
    double duration_seconds = 0.0;
    double aggregate_updates_per_second = 0.0;
    std::vector<double> worker_updates_per_second;
    std::vector<double> checksums;
    // Counts for one worker's scene, not totals multiplied by workers.
    std::size_t scene_meshes = 0;
    std::size_t scene_instances = 0;
};

struct BenchmarkScores {
    double overall = 0.0;
    double cpu     = 0.0;
    double gpu     = 0.0;
};

struct BenchmarkMetrics {
    double average_fps          = 0.0;
    double path_geomean_fps     = 0.0;
    double one_percent_low_fps  = 0.0;
    double effective_fps        = 0.0;

    CpuBenchmarkMetrics cpu;

    double gpu_equivalent_fps = 0.0;
    TimingDistribution gpu;
};

struct HardwareScoreResult {
    int score_version = kScoreVersion;
    BenchmarkScores  scores;
    BenchmarkMetrics metrics;
};

// Convert any positive rate into the shared 1000-points-at-60 scale.
[[nodiscard]] double score_from_rate(double rate_per_second);

// Convert deterministic CPU world-update throughput to the score-v3 CPU
// reference (1000 points at 10,000 updates/s).
[[nodiscard]] double score_from_cpu_rate(double updates_per_second);

// Summarize positive finite millisecond samples. Percentiles use nearest-rank
// selection after sorting ascending.
[[nodiscard]] TimingDistribution summarize_timings(
    std::span<const double> samples_ms);

// Calculate the complete result. `frame_intervals` contains accepted-frame
// intervals in seconds. `segment_frame_intervals` partitions those intervals by
// authored camera-path segment. `gpu_times_ms` contains completed whole-frame
// GPU timestamps; invalid/unavailable values are ignored.
[[nodiscard]] HardwareScoreResult compute_hardware_scores(
    std::span<const float> frame_intervals,
    std::span<const std::vector<float>> segment_frame_intervals,
    const CpuBenchmarkMetrics& cpu,
    std::span<const double> gpu_times_ms);

[[nodiscard]] double    round_fps(double fps);
[[nodiscard]] long long round_score(double score);

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_SCORE_CALCULATOR_H_
