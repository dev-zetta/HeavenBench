/*
 * HeavenBench — CPU-skinned tangent-frame upload
 */
#ifndef HEAVENBENCH_SRC_RHI_SKINNED_TANGENT_UPLOAD_H_
#define HEAVENBENCH_SRC_RHI_SKINNED_TANGENT_UPLOAD_H_

#include "engine/types.h"

#include <geometry/SurfaceOrientation.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <span>
#include <cmath>
#include <limits>
#include <vector>

namespace heaven {

// CPU skinning already supplies UV-aligned tangents. A normal-only frame
// discards that orientation and rotates normal-map detail around the normal.
// SurfaceOrientation's supplied-tangent convention reverses Vertex::tw.
inline std::vector<filament::math::short4> pack_cpu_skinned_tangents(
        std::span<const Vertex> vertices) {
    std::vector<filament::math::float3> normals(vertices.size());
    std::vector<filament::math::float4> tangents(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const auto& v = vertices[i];
        normals[i] = {v.nx, v.ny, v.nz};
        tangents[i] = {v.tx, v.ty, v.tz, -v.tw};
        const auto& n = normals[i];
        const filament::math::float3 t{v.tx, v.ty, v.tz};
        const auto projected = t - n * dot(n, t);
        if (dot(projected, projected) <= 1e-12f) {
            // Collapsed UVs have no defined tangent. Match Filament's stable
            // normal-only perpendicular rather than packing a zero basis.
            auto perpendicular = cross(n,
                filament::math::float3{1, 0, 0});
            if (dot(perpendicular, perpendicular) <=
                    std::numeric_limits<float>::epsilon())
                perpendicular = cross(n,
                    filament::math::float3{0, 1, 0});
            perpendicular = normalize(perpendicular);
            tangents[i].xyz = perpendicular;
        }
    }

    auto* orientation = filament::geometry::SurfaceOrientation::Builder()
        .vertexCount(vertices.size())
        .normals(normals.data())
        .tangents(tangents.data())
        .build();
    if (!orientation) return {};
    std::vector<filament::math::short4> quats(vertices.size());
    orientation->getQuats(quats.data(), quats.size());
    delete orientation;
    return quats;
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_SKINNED_TANGENT_UPLOAD_H_
