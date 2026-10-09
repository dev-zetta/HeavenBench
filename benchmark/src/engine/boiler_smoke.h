#ifndef HEAVENBENCH_SRC_ENGINE_BOILER_SMOKE_H_
#define HEAVENBENCH_SRC_ENGINE_BOILER_SMOKE_H_

#include "engine/types.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace heaven {

// The known airship boiler's highest vertices form its chimney rim. Measure
// that rim from geometry: the mesh origin is about ten metres away from it,
// and the whole boiler AABB is not centered on the chimney. Min/max midpoint
// avoids bias from duplicated UV-seam vertices and matching LOD surfaces.
[[nodiscard]] inline std::optional<glm::mat4> boiler_smoke_transform(
        const Mesh& mesh, const glm::mat4& instance_transform) {
    float top = std::numeric_limits<float>::lowest();
    bool found = false;
    for (const auto& surface : mesh.surfaces) {
        for (const auto& vertex : surface.vertices) {
            if (!std::isfinite(vertex.px) || !std::isfinite(vertex.py) ||
                !std::isfinite(vertex.pz)) return std::nullopt;
            top = std::max(top, vertex.pz);
            found = true;
        }
    }
    if (!found) return std::nullopt;

    glm::vec2 rim_min(std::numeric_limits<float>::max());
    glm::vec2 rim_max(std::numeric_limits<float>::lowest());
    const float tolerance = 8.0f * std::numeric_limits<float>::epsilon() *
                            std::max(1.0f, std::abs(top));
    for (const auto& surface : mesh.surfaces) {
        for (const auto& vertex : surface.vertices) {
            if (top - vertex.pz > tolerance) continue;
            const glm::vec2 point(vertex.px, vertex.py);
            rim_min = glm::min(rim_min, point);
            rim_max = glm::max(rim_max, point);
        }
    }
    const glm::vec2 center = rim_min * 0.5f + rim_max * 0.5f;
    const glm::vec3 world(instance_transform * glm::vec4(center, top, 1.0f));
    if (!std::isfinite(world.x) || !std::isfinite(world.y) ||
        !std::isfinite(world.z)) return std::nullopt;
    // Place the opening through the full instance transform, but keep the
    // particle frame upright: smoke rises in world Z, independent of the hull.
    glm::mat4 result(1.0f);
    result[3] = glm::vec4(world, 1.0f);
    return result;
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_BOILER_SMOKE_H_
