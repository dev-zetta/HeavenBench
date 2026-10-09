/*
 * HeavenBench — Per-run frame statistics collector
 *
 * StatsCollector accumulates the statistics for a single benchmark run: the
 * total frame count, the total elapsed time, and the minimum and maximum
 * instantaneous frame rate. Accepted-frame wall-clock intervals are kept
 * unclamped, matching the samples used by the completed result and score.
 *
 * Pure CPU logic — no Filament/SDL/ImGui dependency — so it is host-testable.
 */
#ifndef HEAVENBENCH_SRC_BENCHMARK_STATS_COLLECTOR_H_
#define HEAVENBENCH_SRC_BENCHMARK_STATS_COLLECTOR_H_

#include <cstdint>
#include <limits>

namespace heaven {

class StatsCollector {
public:
    // Zero the counters and the maximum frame rate, set the minimum frame rate
    // to +inf so the first valid frame replaces it, and clear the valid-frame
    // flag.
    void reset();

    // Feed one accepted frame's wall-clock interval in seconds. Positive finite
    // intervals update all statistics, including hitches longer than the
    // simulation delta cap. Non-positive and non-finite samples are ignored.
    void add_frame(float raw_dt_seconds);

    [[nodiscard]] uint64_t frames() const { return frames_; }
    [[nodiscard]] double   total_time() const { return total_time_; }

    // Returns 0 until the first valid frame has been recorded, after which it
    // returns the smallest instantaneous frame rate seen.
    [[nodiscard]] float min_fps() const;

    [[nodiscard]] float max_fps() const { return max_fps_; }

private:
    uint64_t frames_           = 0;
    double   total_time_       = 0.0;
    float    min_fps_          = std::numeric_limits<float>::max();
    float    max_fps_          = 0.0f;
    bool     have_valid_frame_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_STATS_COLLECTOR_H_
