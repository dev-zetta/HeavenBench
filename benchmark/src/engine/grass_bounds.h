#ifndef HEAVENBENCH_SRC_ENGINE_GRASS_BOUNDS_H_
#define HEAVENBENCH_SRC_ENGINE_GRASS_BOUNDS_H_

#include "engine/grass_cpu.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>

namespace heaven {

struct GrassBounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    float max_wind_height_squared = 0.0f;
    bool valid = false;
};

// Measure the generated positions, including their authored bend and rest-pose
// wind. Keep UV.y squared because both grass shaders scale live wind by it.
[[nodiscard]] inline GrassBounds grass_geometry_bounds(
        std::span<const GrassVertex> vertices) {
    GrassBounds result;
    if (vertices.empty()) return result;
    result.min = glm::vec3(std::numeric_limits<float>::max());
    result.max = glm::vec3(std::numeric_limits<float>::lowest());
    for (const auto& vertex : vertices) {
        const glm::vec3 position(vertex.px, vertex.py, vertex.pz);
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z) || !std::isfinite(vertex.v)) return {};
        result.min = glm::min(result.min, position);
        result.max = glm::max(result.max, position);
        result.max_wind_height_squared = std::max(
            result.max_wind_height_squared, vertex.v * vertex.v);
    }
    result.valid = std::isfinite(result.max_wind_height_squared);
    return result;
}

// heaven_grass and heaven_grass_depth add wind in WORLD space after the model
// transform: (0.8*w*h*h, 0.3*w*h*h, -0.2*abs(w)*h*h), with |w| bounded by the
// sum of the two sine amplitudes. Map a conservative symmetric world envelope
// back into local space so rotated, mirrored and nonuniformly scaled instances
// cannot clip moving blade tips. Singular/projective/nonfinite transforms must
// leave culling disabled; no finite local box can cover their world-space sway.
[[nodiscard]] inline std::optional<GrassBounds> grass_wind_bounds(
        const GrassBounds& geometry, const glm::mat4& transform) {
    if (!geometry.valid) return std::nullopt;
    for (glm::length_t column = 0; column < 4; ++column) {
        for (glm::length_t row = 0; row < 4; ++row) {
            if (!std::isfinite(transform[column][row])) return std::nullopt;
        }
    }
    if (transform[0][3] != 0.0f || transform[1][3] != 0.0f ||
        transform[2][3] != 0.0f || transform[3][3] != 1.0f) return std::nullopt;
    const glm::dmat3 linear(transform);
    const double determinant = glm::determinant(linear);
    if (!std::isfinite(determinant) || determinant == 0.0) return std::nullopt;
    const glm::dmat3 inverse = glm::inverse(linear);
    const double amplitude = std::abs(render_tuning::grass.wind_tuning.z) +
                             std::abs(render_tuning::grass.wind_tuning.w);
    // One centimetre of additional world padding covers rounded shader and
    // bounding-box arithmetic without relying on the particular camera pose.
    const glm::dvec3 world_margin =
        glm::dvec3(0.8, 0.3, 0.2) * amplitude *
        static_cast<double>(geometry.max_wind_height_squared) + glm::dvec3(0.01);
    GrassBounds result = geometry;
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        double padding = 0.0;
        for (glm::length_t column = 0; column < 3; ++column) {
            padding += std::abs(inverse[column][axis]) * world_margin[column];
        }
        // Also protect center/halfExtent cancellation for translated local
        // geometry; the renderer uses Filament's explicit min/max conversion.
        padding += 16.0 * std::numeric_limits<float>::epsilon() *
            std::max({1.0, std::abs(static_cast<double>(geometry.min[axis])),
                      std::abs(static_cast<double>(geometry.max[axis]))});
        const double lower = static_cast<double>(geometry.min[axis]) - padding;
        const double upper = static_cast<double>(geometry.max[axis]) + padding;
        if (!std::isfinite(lower) || !std::isfinite(upper) ||
            lower < -std::numeric_limits<float>::max() ||
            upper > std::numeric_limits<float>::max() ||
            std::abs(lower + upper) > std::numeric_limits<float>::max() ||
            upper - lower > std::numeric_limits<float>::max()) return std::nullopt;
        result.min[axis] = static_cast<float>(lower);
        result.max[axis] = static_cast<float>(upper);
    }
    return result;
}

// Generated once with the geometry, then recalculated only when an instance's
// linear transform changes. Floating-island translation needs no new bounds.
struct GrassBoundsCache {
    GrassBounds geometry;
    std::optional<GrassBounds> local;
    glm::mat4 applied_transform{0.0f};
    bool initialized = false;

    bool update(const glm::mat4& transform) {
        glm::mat4 signature = transform;
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            if (std::isfinite(signature[3][axis])) signature[3][axis] = 0.0f;
        }
        if (initialized && signature == applied_transform) return false;
        applied_transform = signature;
        initialized = true;
        local = grass_wind_bounds(geometry, transform);
        return true;
    }
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_GRASS_BOUNDS_H_
