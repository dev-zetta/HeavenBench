#ifndef HEAVENBENCH_SRC_ENGINE_BOUNDED_DISPLACEMENT_SURFACE_H_
#define HEAVENBENCH_SRC_ENGINE_BOUNDED_DISPLACEMENT_SURFACE_H_

#include "engine/types.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace heaven {

// The caller freezes the renderer's original packed tangent frames, height
// samples and footprints before invoking this GPU-independent builder. Position
// is in model coordinates. Direction is the actual decoded unit shader normal,
// before rigid instance rotation; magnitude remains a separate original field.
struct BoundedDisplacementVertex {
    glm::dvec3 neutral_position{};
    glm::dvec3 displacement_direction{};
    glm::vec2 uv0{};
    glm::vec2 uv1{};
    glm::vec2 footprint{};
    float displacement_magnitude = 1.0f;
    float height = 127.0f / 255.0f;
    std::uint64_t compatible_seam = 0;
};

struct BoundedDisplacementInput {
    std::span<const BoundedDisplacementVertex> vertices;
    std::span<const std::array<std::uint32_t, 3>> triangles;
    // All borrowers must belong to this proper uniform scale class. Translation
    // and rotation do not affect the canonical world-metre construction metric.
    double uniform_scale = 1.0;
    double reference_amplitude = 0.0;
};

struct BoundedDisplacementPolicy {
    double correspondence_bound = 0.020;
    double initial_node_bound = 0.00999;
    double position_error = 0.002;
    double normal_error_degrees = 0.5;
    double neutral_feature_degrees = 30.0;
    double common_control_blend = 0.9;
    std::uint32_t initial_levels = 2;
    std::uint32_t protected_zero_rings = 2;
    std::uint32_t protected_full_rings = 7;
    std::uint32_t maximum_sample_depth = 12;
    std::uint32_t maximum_triangles = 1500000;
    std::uint64_t maximum_work_bytes = 1024ull * 1024 * 1024;
    std::uint64_t maximum_patch_evaluations = 32000000;
};

enum class BoundedDisplacementStatus {
    Complete,
    BudgetLimited,
    TargetBoundFailed,
    TargetOrientationFailed,
    NonconformingTopology,
    NumericalLimit,
};

struct BoundedDisplacementNode {
    glm::dvec3 old_raster_position{};
    glm::dvec3 target_position{};
    // World metres in model orientation: no inverse instance scale is applied.
    glm::dvec3 correction{};
    std::uint32_t source_parent = 0;
    glm::dvec3 source_barycentric{};
};

struct BoundedDisplacementTriangle {
    std::array<std::uint32_t, 3> nodes{};
    std::uint32_t source_parent = 0;
    std::array<glm::dvec3, 3> source_barycentric{};
};

struct BoundedDisplacementMetrics {
    std::uint64_t input_vertices = 0;
    std::uint64_t input_triangles = 0;
    std::uint64_t output_nodes = 0;
    std::uint64_t output_triangles = 0;
    std::uint64_t pinned_nodes = 0;
    std::uint64_t native_regular_patches = 0;
    std::uint64_t native_extraordinary_patches = 0;
    std::uint64_t sampled_quality_failures = 0;
    std::uint64_t patch_evaluations = 0;
    std::uint64_t target_patch_evaluations = 0;
    std::uint64_t sampling_patch_evaluations = 0;
    std::uint64_t orientation_failures = 0;
    std::uint64_t common_control_adjustments = 0;
    double maximum_common_control_adjustment = 0.0;
    std::uint64_t affine_components = 0;
    std::uint64_t eligible_components = 0;
    std::uint64_t accepted_components = 0;
    std::uint64_t minimum_conforming_triangles = 0;
    std::uint64_t rejected_components = 0;
    std::uint64_t rejected_source_triangles = 0;
    std::uint64_t rejected_orientation_failures = 0;
    std::uint64_t rejected_bound_components = 0;
    std::uint64_t rejected_numerical_components = 0;
    double maximum_rejected_target_bound = 0.0;
    std::uint32_t first_failed_source_parent = UINT32_MAX;
    glm::dvec3 first_failed_source_barycentric{};
    double maximum_target_bound = 0.0;
    double maximum_sampled_movement = 0.0;
    double maximum_sampled_position_error = 0.0;
    double maximum_sampled_normal_error_degrees = 0.0;
    double failing_area_fraction = 0.0;
    double minimum_triangle_area = 0.0;
};

struct BoundedDisplacementParentQuality {
    std::uint64_t output_triangles = 0;
    std::uint64_t sampled_quality_failures = 0;
    double maximum_position_error = 0.0;
    double maximum_normal_error_degrees = 0.0;
    bool sampled_target = false;
};

struct BoundedDisplacementSurface {
    BoundedDisplacementStatus status = BoundedDisplacementStatus::Complete;
    // Original source vertices form the prefix, retaining caller-owned shading
    // attributes. Parent-local attachments can duplicate these shared nodes at
    // upload without changing their geometry or original tangent frames.
    std::vector<BoundedDisplacementNode> nodes;
    std::vector<BoundedDisplacementTriangle> triangles;
    BoundedDisplacementMetrics metrics;
    // Original-parent order makes quality and budget allocation reviewable
    // without embedding any scene, light, camera or asset identifier.
    std::vector<BoundedDisplacementParentQuality> parent_quality;
    struct ComponentFallback {
        BoundedDisplacementStatus reason = BoundedDisplacementStatus::NumericalLimit;
        std::vector<std::uint32_t> source_parents;
        std::string diagnostic;
    };
    // Rejected targets are replaced by exact original triangles. They do not
    // contribute failures to the accepted final geometry integrity counters.
    std::vector<ComponentFallback> component_fallbacks;
};

// Invalid/unsupported source data returns Error and has no partial output.
// A valid budget-limited mesh is explicit; it is never labelled converged.
[[nodiscard]] Result<BoundedDisplacementSurface> build_bounded_displacement_surface(
    const BoundedDisplacementInput& input,
    const BoundedDisplacementPolicy& policy = {});

} // namespace heaven
#endif
