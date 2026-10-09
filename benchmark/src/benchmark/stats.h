/*
 * HeavenBench — Frame timing and benchmark statistics
 */
#ifndef HEAVENBENCH_SRC_BENCHMARK_STATS_H_
#define HEAVENBENCH_SRC_BENCHMARK_STATS_H_

#include "engine/types.h"

#include <chrono>
#include <cstddef>
#include <span>
#include <vector>

namespace heaven {

// Performance-window measurement. These durations define a consistent sample;
// FPS itself has no universal passing value across hardware and settings.
inline constexpr float kPerfWarmupSeconds = 2.0f;
inline constexpr float kPerfMinWindowSeconds = 60.0f;

// Outcome of measuring mean FPS over the post-warm-up window.
struct PerfWindow {
    // True only when the post-warm-up window is at least the required minimum
    // length. Validity describes measurement coverage, not performance quality.
    bool   valid            = false;
    // Mean frame rate over the window: frames_in_window / window_seconds.
    float  mean_fps         = 0.0f;
    // Wall-clock seconds counted in the window (AFTER the warm-up exclusion).
    float  window_seconds   = 0.0f;
    // Wall-clock seconds actually excluded at the front as warm-up.
    float  warmup_seconds   = 0.0f;
    // Number of frames counted in the window.
    std::size_t frames_in_window = 0;
};

// Compute the mean FPS over the measurement window from a series of per-frame
// delta times (in seconds, in render order). The first `warmup_seconds` worth
// of frames are excluded from the front — specifically the smallest prefix
// whose cumulative time reaches `warmup_seconds` — then the mean frame rate is
// taken over the remaining frames as (frames / elapsed time). `valid` is set
// only when the remaining window is at least `min_window_seconds` long.

// Pure and free of any GPU/Filament/SDL dependency, so the windowing and
// first-2s-exclusion logic is exercised headlessly.
[[nodiscard]] PerfWindow measure_perf_window(
    const std::vector<float>& frame_times,
    float warmup_seconds     = kPerfWarmupSeconds,
    float min_window_seconds = kPerfMinWindowSeconds);

struct FrameRateSample {
    float fps                 = 0.0f;
    float average_frame_ms    = 0.0f;
    float one_percent_low_fps = 0.0f;
};

// Summarize intervals between frames accepted by the renderer. Invalid
// intervals are ignored. The 1% low is the reciprocal of the mean duration of
// the slowest 1% of frames, with at least one frame included.
[[nodiscard]] FrameRateSample summarize_frame_rate(
    std::span<const float> frame_intervals);

class BenchmarkStats {
public:
    using Clock = std::chrono::steady_clock;

    BenchmarkStats();

    // Call at the start of each frame. Simulation retains its 100 ms cap;
    // benchmark playback must instead consume raw_frame_delta(). An explicit
    // timestamp supports deterministic timing tests without sleeping.
    float begin_frame(Clock::time_point now = Clock::now());

    [[nodiscard]] float raw_frame_delta() const { return raw_frame_delta_; }

    // A new run starts after loading / CPU measurement / a cancelled pause.
    // Retire that elapsed time without changing accepted-frame statistics.
    // The next accepted frame establishes a fresh presentation baseline.
    void reset_frame_clock(Clock::time_point now = Clock::now());

    //Call after a frame is accepted by the renderer and reaches endFrame().
    //Rejected main-loop iterations must not call this.
    [[nodiscard]] float record_presented_frame(Clock::time_point now = Clock::now());

    //Print benchmark results to stdout.
    void print_results() const;

    [[nodiscard]] float current_fps() const { return current_fps_; }
    [[nodiscard]] float current_frame_time_ms() const {
        return current_frame_time_ms_;
    }
    [[nodiscard]] float current_one_percent_low_fps() const {
        return current_one_percent_low_fps_;
    }
    [[nodiscard]] size_t frame_count() const { return frame_times_.size(); }

    //Intervals between accepted frames, in render order (seconds).
    [[nodiscard]] const std::vector<float>& frame_times() const { return frame_times_; }

    //Returns true once per second (for title bar updates, etc.)
    [[nodiscard]] bool fps_updated() const { return fps_updated_; }

private:
    Clock::time_point last_time_;
    Clock::time_point last_present_time_;
    float raw_frame_delta_ = 0.0f;
    std::vector<float> frame_times_;
    std::vector<float> sample_frame_times_;

    float fps_timer_                   = 0.0f;
    float current_fps_                 = 0.0f;
    float current_frame_time_ms_       = 0.0f;
    float current_one_percent_low_fps_ = 0.0f;
    bool  fps_updated_                 = false;

    static constexpr float kMaxDeltaTime = 0.1f;
    static constexpr int   kWarmupFrames = 10;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_STATS_H_
