#ifndef HEAVENBENCH_SRC_ENGINE_DISPLACEMENT_SURFACE_PRIORITY_H_
#define HEAVENBENCH_SRC_ENGINE_DISPLACEMENT_SURFACE_PRIORITY_H_

#include "engine/bounded_displacement_surface.h"

#include <array>
#include <cstdint>

namespace heaven {

// Upper angle boundaries in degrees; the final bin includes 180 degrees.
inline constexpr std::array<double, 11> kDisplacementPriorityAngleBins{
    0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 30.0, 45.0, 60.0, 90.0, 180.0};

struct DisplacementPriorityHistogram {
    std::array<std::uint64_t, kDisplacementPriorityAngleBins.size()> triangles{};
    std::array<double, kDisplacementPriorityAngleBins.size()> neutral_area{};
};

struct DisplacementSurfacePriority {
    std::uint64_t source_triangles = 0;
    std::uint64_t measured_triangles = 0;
    std::uint64_t degenerate_neutral_triangles = 0;
    std::uint64_t degenerate_displaced_triangles = 0;
    std::uint64_t reversed_displaced_triangles = 0;
    std::uint64_t undefined_raw_reference_triangles = 0;
    double neutral_area = 0.0;
    double displaced_area = 0.0;
    // Canonical preclip square metres times squared radians. The mismatch
    // reference is the uploaded raw displacement normal at the face centroid,
    // not an authored fragment normal map or a particular light direction.
    double height_rotation_energy = 0.0;
    double raw_mismatch_energy = 0.0;
    double neutral_raw_mismatch_energy = 0.0;
    double increased_raw_mismatch_energy = 0.0;
    // Sum of height rotation and the nonnegative per-face mismatch increase.
    // Dividing by source triangles estimates importance per preparation unit;
    // callers retain the totals when choosing a scene-wide allocation policy.
    double priority_energy = 0.0;
    double priority_per_source_triangle = 0.0;
    double height_rotation_rms_degrees = 0.0;
    double raw_mismatch_rms_degrees = 0.0;
    double priority_rms_degrees = 0.0;
    double maximum_height_rotation_degrees = 0.0;
    double maximum_raw_mismatch_degrees = 0.0;
    double minimum_neutral_triangle_angle_degrees = 180.0;
    double minimum_displaced_triangle_angle_degrees = 180.0;
    double minimum_orientation_cosine = 1.0;
    double maximum_vertex_offset = 0.0;
    DisplacementPriorityHistogram height_rotation_histogram;
    DisplacementPriorityHistogram raw_mismatch_histogram;
};

// Cheap O(source triangles), O(1) auxiliary storage. Uses exactly the frozen
// source position / native height / decoded normal / magnitude contract of the
// bounded builder. This is a scheduling score, not an integrity acceptance or
// a shadow-defect classifier. Degenerate geometry is counted, never repaired.
[[nodiscard]] Result<DisplacementSurfacePriority> analyze_displacement_surface_priority(
    const BoundedDisplacementInput& input);

} // namespace heaven
#endif
