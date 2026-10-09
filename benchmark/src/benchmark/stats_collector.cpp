/*
 * HeavenBench — Per-run frame statistics collector
 */
#include "benchmark/stats_collector.h"

#include <algorithm>
#include <cmath>

namespace heaven {

void StatsCollector::reset() {
    frames_           = 0;
    total_time_       = 0.0;
    min_fps_          = std::numeric_limits<float>::max();
    max_fps_          = 0.0f;
    have_valid_frame_ = false;
}

void StatsCollector::add_frame(float raw_dt_seconds) {
    // Measurement uses accepted-frame wall time, including long hitches. The
    // simulation's delta clamp must not inflate the live FPS or minimum FPS.
    if (!(raw_dt_seconds > 0.0f) || !std::isfinite(raw_dt_seconds)) return;

    frames_     += 1;
    total_time_ += static_cast<double>(raw_dt_seconds);

    float instantaneous_fps = 1.0f / raw_dt_seconds;
    min_fps_ = std::min(min_fps_, instantaneous_fps);
    max_fps_ = std::max(max_fps_, instantaneous_fps);
    have_valid_frame_ = true;
}

float StatsCollector::min_fps() const {
    // Before any valid frame, report 0 so the overlay shows 0 rather than the
    // +inf sentinel used to seed the reduction.
    return have_valid_frame_ ? min_fps_ : 0.0f;
}

} // namespace heaven
