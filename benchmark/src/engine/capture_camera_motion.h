// Deterministic diagnostic camera motion; independent of simulation and GPU APIs.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace heaven {

using CaptureCameraPose = std::array<float, 5>;

struct CaptureCameraSample {
    CaptureCameraPose pose;
    double fraction = 0.0;
    std::uint64_t source_index = 1;
    std::uint64_t total_sources = 2;
};

[[nodiscard]] inline bool valid_capture_camera_pose(const CaptureCameraPose& pose) {
    return std::all_of(pose.begin(), pose.end(), [](float value) { return std::isfinite(value); }) &&
        pose[4] >= -89.0f && pose[4] <= 89.0f;
}

// accepted_before is the number of accepted application capture submissions,
// never the loop-attempt or generated-frame count. Retrying the same source
// therefore returns exactly the same pose. The capture-count tail is included
// in total_sources. Intermediate yaw can be outside 0..360; endpoints retain
// their exact supplied values and all orientations follow the shortest arc.
[[nodiscard]] inline std::optional<CaptureCameraSample> sample_capture_camera_motion(
        const CaptureCameraPose& start, const CaptureCameraPose& end,
        std::uint64_t accepted_before, std::uint64_t total_sources) {
    if (total_sources < 2 || !valid_capture_camera_pose(start) || !valid_capture_camera_pose(end)) {
        return std::nullopt;
    }
    const auto source = accepted_before >= total_sources - 1 ? total_sources : accepted_before + 1;
    const double fraction = static_cast<double>(source - 1) / static_cast<double>(total_sources - 1);
    CaptureCameraSample sample{start, fraction, source, total_sources};
    if (source == 1) return sample;
    if (source == total_sources) {
        sample.pose = end;
        return sample;
    }
    for (std::size_t i = 0; i < sample.pose.size(); ++i) {
        const double a = start[i];
        const double difference = i == 3
            ? std::remainder(static_cast<double>(end[i]) - a, 360.0)
            : static_cast<double>(end[i]) - a;
        sample.pose[i] = static_cast<float>(a + difference * fraction);
    }
    return sample;
}

} // namespace heaven
