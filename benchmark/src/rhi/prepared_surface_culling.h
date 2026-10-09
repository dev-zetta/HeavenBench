#ifndef HEAVENBENCH_SRC_RHI_PREPARED_SURFACE_CULLING_H_
#define HEAVENBENCH_SRC_RHI_PREPARED_SURFACE_CULLING_H_

#include <glm/mat4x4.hpp>
#include <glm/mat3x3.hpp>
#include <glm/geometric.hpp>
#include <glm/common.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <initializer_list>

namespace heaven {
struct PreparedSurfaceCullingBounds {
    glm::vec3 inner_min{};
    glm::vec3 inner_max{};
    glm::vec3 outer_center{};
    glm::vec3 outer_half_extent{};
};

// Filament transforms its culling AABB without a homogeneous divide. Prepared
// vertices preserve full matrix W; this canonical affine row needs no margin proof.
// Keep the caller's original static/volume permission; no tolerance is applied.
[[nodiscard]] inline bool prepared_surface_culling_enabled(
        bool base_permission, const glm::mat4& transform) noexcept {
    return base_permission && transform[0][3] == 0.0f &&
        transform[1][3] == 0.0f && transform[2][3] == 0.0f &&
        transform[3][3] == 1.0f;
}

// The inner envelope already includes shader displacement. The actual outer
// Box retains the caller's extra margin. A constant positive W can use that
// margin only while this sufficient containment test succeeds. Filament moves
// the world origin with the camera, so call again even without a dirty entity.
[[nodiscard]] inline bool prepared_surface_culling_enabled(
        bool base_permission, const glm::mat4& transform,
        const PreparedSurfaceCullingBounds& bounds, const glm::dvec3& camera,
        bool camera_origin_modes_supported) noexcept {
    if (prepared_surface_culling_enabled(base_permission, transform)) return true;
    if (!base_permission || !camera_origin_modes_supported ||
        transform[0][3] != 0.0f || transform[1][3] != 0.0f ||
        transform[2][3] != 0.0f || !(transform[3][3] > 0.0f)) return false;
    const double w = transform[3][3];
    glm::dmat3 a(transform);
    const glm::dvec3 translation(transform[3]);
    const glm::dvec3 center = (glm::dvec3(bounds.inner_min) + glm::dvec3(bounds.inner_max)) * .5;
    const glm::dvec3 half = (glm::dvec3(bounds.inner_max) - glm::dvec3(bounds.inner_min)) * .5;
    const glm::dvec3 outer_center(bounds.outer_center);
    const glm::dvec3 outer_half(bounds.outer_half_extent);
    const glm::dvec3 margin = outer_half - half - glm::abs(outer_center - center);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!std::isfinite(transform[i][j])) return false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(center[i]) || !std::isfinite(half[i]) || half[i] < 0 ||
            !std::isfinite(margin[i]) || margin[i] <= 0 || !std::isfinite(camera[i])) return false;

    // Gershgorin bounds the least eigenvalue of A^T A. This lower bound
    // avoids an eigensolver and rejects singular or inconclusive transforms.
    const glm::dmat3 gram = glm::transpose(a) * a;
    double least = std::numeric_limits<double>::infinity();
    double frobenius2 = 0;
    for (int i = 0; i < 3; ++i) {
        double row = gram[i][i];
        frobenius2 += gram[i][i];
        for (int j = 0; j < 3; ++j)
            if (i != j) row -= std::abs(gram[j][i]);
        least = std::min(least, row);
    }
    if (!(least > 0) || !std::isfinite(frobenius2)) return false;
    const double frobenius = std::sqrt(frobenius2);
    const double available = std::min({margin.x, margin.y, margin.z}) * std::sqrt(least);
    const double delta = std::abs(1.0 / w - 1.0);
    // gamma(64) covers scalar FP32 rounding in matrix conversion, affine
    // dot products, Box center/extent arithmetic and homogeneous division.
    // Existing inner shader-envelope padding remains a separate contract.
    constexpr double u = double(std::numeric_limits<float>::epsilon()) * .5;
    constexpr double gamma = 64 * u / (1 - 64 * u);
    return std::ranges::all_of(std::array{glm::dvec3(0), camera}, [&](const glm::dvec3& origin) {
        const double discrepancy = delta *
            (glm::length(a * center + translation - w * origin) + frobenius * glm::length(half));
        const double rounding_scale = frobenius *
            (glm::length(center) + glm::length(outer_center) + glm::length(half) + glm::length(outer_half)) +
            2 * (glm::length(translation) + w * glm::length(origin)) + 1;
        const double reserve = gamma * rounding_scale * (1 + 1 / w);
        return std::isfinite(discrepancy) && std::isfinite(reserve) &&
            discrepancy + reserve < available;
    });
}
} // namespace heaven
#endif // HEAVENBENCH_SRC_RHI_PREPARED_SURFACE_CULLING_H_
