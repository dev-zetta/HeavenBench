/*
 * HeavenBench — Frame timing and benchmark statistics
 */
#include "benchmark/stats.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <numeric>

namespace heaven {

PerfWindow measure_perf_window(const std::vector<float>& frame_times,
                               float warmup_seconds,
                               float min_window_seconds) {
    PerfWindow out;

    // Walk the front of the series accumulating wall-clock time until we have
    // excluded at least `warmup_seconds` ("the first 2 seconds after the first
    // rendered frame"). The frame that crosses the threshold is the
    // last excluded frame; counting starts at the next one.

    // Accumulate in double: a real >= 60 s run at thousands of FPS sums
    // hundreds of thousands to millions of tiny per-frame floats, and summing
    // that many float32 values drifts enough to skew both the window length and
    // the mean. double keeps the window/mean accurate.
    std::size_t i = 0;
    double warmup_elapsed = 0.0;
    const double warmup_target = static_cast<double>(warmup_seconds);
    for (; i < frame_times.size() && warmup_elapsed < warmup_target; ++i) {
        warmup_elapsed += static_cast<double>(frame_times[i]);
    }
    out.warmup_seconds = static_cast<float>(warmup_elapsed);

    // Everything from `i` onward is the measurement window.
    double window_elapsed = 0.0;
    std::size_t window_frames = 0;
    for (; i < frame_times.size(); ++i) {
        window_elapsed += static_cast<double>(frame_times[i]);
        ++window_frames;
    }

    out.window_seconds   = static_cast<float>(window_elapsed);
    out.frames_in_window = window_frames;

    // The mean frame rate over the window is frames / elapsed time. Guard the
    // divide so an empty/zero-length window reports 0 rather than NaN/Inf.
    if (window_elapsed > 0.0) {
        out.mean_fps =
            static_cast<float>(static_cast<double>(window_frames) / window_elapsed);
    }

    // A complete measurement requires the minimum window after reaching the
    // warm-up duration. Short runs still expose their measured time and frame
    // count, but cannot be treated as complete comparison samples.
    out.valid = warmup_elapsed >= warmup_target &&
                window_elapsed >= static_cast<double>(min_window_seconds);

    return out;
}

FrameRateSample summarize_frame_rate(std::span<const float> frame_intervals) {
    FrameRateSample out;
    std::vector<float> valid;
    valid.reserve(frame_intervals.size());

    double total = 0.0;
    for (const float dt : frame_intervals) {
        if (dt > 0.0f && std::isfinite(dt)) {
            valid.push_back(dt);
            total += static_cast<double>(dt);
        }
    }
    if (valid.empty() || total <= 0.0) return out;

    out.fps = static_cast<float>(
        static_cast<double>(valid.size()) / total);
    out.average_frame_ms = static_cast<float>(
        total * 1000.0 / static_cast<double>(valid.size()));

    std::sort(valid.begin(), valid.end(), std::greater<>());
    const size_t slow_count = std::max<size_t>(
        1, static_cast<size_t>(std::ceil(
               static_cast<double>(valid.size()) * 0.01)));
    const double slow_total = std::accumulate(
        valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(slow_count), 0.0);
    if (slow_total > 0.0) {
        out.one_percent_low_fps = static_cast<float>(
            static_cast<double>(slow_count) / slow_total);
    }
    return out;
}

BenchmarkStats::BenchmarkStats()
    : last_time_(Clock::now()) {
    frame_times_.reserve(10000);
    sample_frame_times_.reserve(1000);
}

float BenchmarkStats::begin_frame(Clock::time_point now) {
    raw_frame_delta_ = std::chrono::duration<float>(now - last_time_).count();
    last_time_ = now;

    fps_updated_ = false;
    return std::min(raw_frame_delta_, kMaxDeltaTime);
}

void BenchmarkStats::reset_frame_clock(Clock::time_point now) {
    last_time_ = now;
    last_present_time_ = {};
    raw_frame_delta_ = 0.0f;
}

float BenchmarkStats::record_presented_frame(Clock::time_point now) {
    if (last_present_time_.time_since_epoch().count() == 0) {
        // Establish an interval baseline without folding startup/loading time
        // into the first live sample.
        last_present_time_ = now;
        return 0.0f;
    }

    const float interval =
        std::chrono::duration<float>(now - last_present_time_).count();
    last_present_time_ = now;
    if (!(interval > 0.0f) || !std::isfinite(interval)) return 0.0f;

    frame_times_.push_back(interval);
    sample_frame_times_.push_back(interval);
    fps_timer_ += interval;
    if (fps_timer_ >= 1.0f) {
        const FrameRateSample sample =
            summarize_frame_rate(sample_frame_times_);
        current_fps_                 = sample.fps;
        current_frame_time_ms_       = sample.average_frame_ms;
        current_one_percent_low_fps_ = sample.one_percent_low_fps;

        fps_timer_ = 0.0f;
        sample_frame_times_.clear();
        fps_updated_ = true;
    }
    return interval;
}

void BenchmarkStats::print_results() const {
    if (frame_times_.size() <= static_cast<size_t>(kWarmupFrames)) {
        std::printf("\nNot enough frames for benchmark results.\n");
        return;
    }

    // Skip warmup frames
    std::vector<float> ft(frame_times_.begin() + kWarmupFrames, frame_times_.end());

    const FrameRateSample sample = summarize_frame_rate(ft);

    std::printf("\n=== Benchmark Results ===\n");
    std::printf("Frames:    %zu\n", ft.size());
    std::printf("Avg FPS:   %.1f\n", sample.fps);
    std::printf("Avg frame: %.2f ms\n", sample.average_frame_ms);
    std::printf("1%% low:    %.1f FPS\n", sample.one_percent_low_fps);
}

} // namespace heaven
