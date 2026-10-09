/*
 * HeavenBench — GPU-free bounds for persistent instanced renderables
 */
#ifndef HEAVENBENCH_SRC_RHI_INSTANCE_BOUNDS_H_
#define HEAVENBENCH_SRC_RHI_INSTANCE_BOUNDS_H_

#include "engine/types.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace heaven {

// Filament applies winding and homogeneous culling at the renderable root,
// rather than independently for each InstanceBuffer member.
[[nodiscard]] inline bool instance_transform_supports_culling(
        const glm::mat4& transform) noexcept {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(transform[column][row])) return false;
    return transform[0][3] == 0 && transform[1][3] == 0 &&
        transform[2][3] == 0 && transform[3][3] == 1;
}

[[nodiscard]] inline bool instance_transform_supports_grouping(
        const glm::mat4& transform) noexcept {
    if (!instance_transform_supports_culling(transform)) return false;
    const double determinant = glm::determinant(glm::dmat3(transform));
    return std::isfinite(determinant) && determinant > 0;
}

struct InstanceLocalBounds {
    glm::vec3 min{std::numeric_limits<float>::infinity()};
    glm::vec3 max{-std::numeric_limits<float>::infinity()};
    bool displaced = false;
    bool valid = true;
};

inline void include_instance_local_bounds(InstanceLocalBounds& bounds,
        glm::vec3 min, glm::vec3 max, bool displaced) noexcept {
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(min[axis]) || !std::isfinite(max[axis]) || min[axis] > max[axis])
            bounds.valid = false;
    bounds.min = glm::min(bounds.min, min);
    bounds.max = glm::max(bounds.max, max);
    bounds.displaced |= displaced;
}

struct InstanceWorldBounds {
    // A finite placeholder is required by the renderable API even when
    // invalid input disables culling. It is not a bound for invalid geometry.
    glm::vec3 min{-1};
    glm::vec3 max{1};
    bool culling = false;
};

// The non-displaced envelope matches the singleton's 1m local margin. For
// ordinary displaced surfaces retain the existing 16m world allowance; this
// is an inherited policy, not a proof for arbitrary shader amplitude/scale.
// Prepared surfaces have a separate proven carrier envelope and never enter.
[[nodiscard]] inline InstanceWorldBounds instance_group_world_bounds(
        const InstanceLocalBounds& local, std::span<const Instance> instances,
        std::span<const int> members) noexcept {
    InstanceWorldBounds result;
    if (members.empty() || !local.valid) return result;
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(local.min[axis]) || !std::isfinite(local.max[axis]) ||
            local.min[axis] > local.max[axis]) return result;
    const glm::dvec3 local_margin(local.displaced ? 0.0 : 1.0);
    const glm::dvec3 lo = glm::dvec3(local.min) - local_margin;
    const glm::dvec3 hi = glm::dvec3(local.max) + local_margin;
    glm::dvec3 world_min(std::numeric_limits<double>::infinity());
    glm::dvec3 world_max(-std::numeric_limits<double>::infinity());
    constexpr double u = double(std::numeric_limits<float>::epsilon()) * .5;
    constexpr double gamma = 8 * u / (1 - 8 * u);
    for (const int member : members) {
        if (member < 0 || std::cmp_greater_equal(member, instances.size())) return result;
        const auto& transform = instances[static_cast<std::size_t>(member)].transform;
        if (!instance_transform_supports_grouping(transform)) return result;
        const glm::dmat4 matrix(transform);
        for (unsigned corner = 0; corner < 8; ++corner) {
            const glm::dvec4 point((corner & 1) ? hi.x : lo.x,
                (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z, 1);
            const glm::dvec3 world(matrix * point);
            glm::dvec3 reserve;
            for (int axis = 0; axis < 3; ++axis) {
                double sum = 1;
                for (int column = 0; column < 4; ++column)
                    sum += std::abs(matrix[column][axis] * point[column]);
                reserve[axis] = gamma * sum;
            }
            world_min = glm::min(world_min, world - reserve);
            world_max = glm::max(world_max, world + reserve);
        }
    }
    const glm::dvec3 world_margin(local.displaced ? 16.0 : 0.0);
    world_min -= world_margin;
    world_max += world_margin;
    for (int axis = 0; axis < 3; ++axis) {
        result.min[axis] = std::nextafter(float(world_min[axis]),
            -std::numeric_limits<float>::infinity());
        result.max[axis] = std::nextafter(float(world_max[axis]),
            std::numeric_limits<float>::infinity());
        if (!std::isfinite(result.min[axis]) || !std::isfinite(result.max[axis]))
            return {};
        // Box::set performs both expressions in FP32 before halving them.
        const double limit = std::numeric_limits<float>::max();
        if (std::abs(double(result.min[axis]) + double(result.max[axis])) > limit ||
            double(result.max[axis]) - double(result.min[axis]) > limit) return {};
    }
    result.culling = true;
    return result;
}

} // namespace heaven
#endif // HEAVENBENCH_SRC_RHI_INSTANCE_BOUNDS_H_
