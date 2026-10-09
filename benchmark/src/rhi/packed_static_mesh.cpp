/* HeavenBench — preserve shipping static tangent frames and height footprints. */
#include "rhi/packed_static_mesh.h"

#include <geometry/SurfaceOrientation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>

namespace heaven {
namespace {
bool finite_vertex(const Vertex& v) {
    return std::ranges::all_of(std::array{v.px, v.py, v.pz, v.nx, v.ny, v.nz,
            v.u, v.v, v.u1, v.v1, v.displacement_normal_scale},
        [](float value) { return std::isfinite(value); });
}
} // namespace

std::uint64_t packed_static_source_signature(
        std::span<const Vertex> vertices, std::span<const std::uint32_t> indices) noexcept {
    static_assert(offsetof(Vertex, displacement_normal_scale) == 40);
    std::uint64_t signature = 14695981039346656037ull;
    const auto append = [&](const void* pointer, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(pointer);
        for (std::size_t i = 0; i < size; ++i) {
            signature ^= bytes[i];
            signature *= 1099511628211ull;
        }
    };
    for (const auto& vertex : vertices) append(&vertex, 44);
    append(indices.data(), indices.size_bytes());
    return signature;
}

std::uint64_t packed_static_source_signature(
        std::span<const Vertex> vertices,
        std::span<const std::array<std::uint32_t, 3>> triangles) noexcept {
    static_assert(offsetof(Vertex, displacement_normal_scale) == 40);
    std::uint64_t signature = 14695981039346656037ull;
    const auto append = [&](const void* pointer, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(pointer);
        for (std::size_t i = 0; i < size; ++i) {
            signature ^= bytes[i];
            signature *= 1099511628211ull;
        }
    };
    for (const auto& vertex : vertices) append(&vertex, 44);
    for (const auto& triangle : triangles)
        append(triangle.data(), 3 * sizeof(std::uint32_t));
    return signature;
}

Result<filament::math::float3> decode_static_displacement_direction(
        const filament::math::short4& packed) {
    const auto snorm = [](std::int16_t value) {
        return std::max(float(value) / 32767.0f, -1.0f);
    };
    const float x = snorm(packed.x), y = snorm(packed.y);
    const float z = snorm(packed.z), w = snorm(packed.w);
    // Exact scalar expansion of common_math.glsl::toTangentFrame(q,n).
    const float nx = 2.0f * x * z + 2.0f * y * w;
    const float ny = -2.0f * x * w + 2.0f * y * z;
    const float nz = 1.0f - 2.0f * x * x - 2.0f * y * y;
    const float length2 = nx * nx + ny * ny + nz * nz;
    if (!std::isfinite(length2) || length2 <= 1e-20f)
        return std::unexpected(Error{"Invalid packed static displacement direction"});
    const float inverse_length = 1.0f / std::sqrt(length2);
    return filament::math::float3{nx * inverse_length, ny * inverse_length,
                                 nz * inverse_length};
}

Result<PackedStaticSurface> pack_static_surface(
        std::span<const Vertex> vertices, std::span<const std::uint32_t> indices) {
    if (vertices.empty() || indices.empty() || indices.size() % 3 != 0 ||
            vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
            indices.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(Error{"Static packing requires a bounded indexed triangle surface"});
    if (!std::ranges::all_of(vertices, finite_vertex) ||
            !std::ranges::all_of(indices, [&](std::uint32_t i) { return i < vertices.size(); }))
        return std::unexpected(Error{"Static packing source is nonfinite or has an invalid index"});
    try {
        PackedStaticSurface result;
        result.source_signature = packed_static_source_signature(vertices, indices);
        const auto count = vertices.size();
        result.positions.resize(count);
        result.uv0.resize(count);
        result.uv1.resize(count);
        result.tangent_frames.resize(count);
        result.displacement_custom.resize(count);
        result.unit_displacement_directions.resize(count);
        result.footprints = mesh_displacement::calculate_footprints<Vertex>(vertices, indices);
        std::vector<filament::math::float3> normals(count);
        bool valid_source_normals = true;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& v = vertices[i];
            result.positions[i] = {v.px, v.py, v.pz};
            normals[i] = {v.nx, v.ny, v.nz};
            result.uv0[i] = {v.u, v.v};
            result.uv1[i] = {v.u1, v.v1};
            result.displacement_custom[i] = {
                    v.displacement_normal_scale, result.footprints[i][0],
                    result.footprints[i][1], 0};
            result.deformation_signature = mesh_displacement::append_deformation_signature(
                    result.deformation_signature, v.u, v.v, v.displacement_normal_scale);
            const double normal_length2 = double(v.nx) * v.nx +
                    double(v.ny) * v.ny + double(v.nz) * v.nz;
            valid_source_normals &= normal_length2 > 1e-20;
        }
        std::vector<filament::math::uint3> triangles(indices.size() / 3);
        for (std::size_t i = 0; i < triangles.size(); ++i)
            triangles[i] = {indices[i * 3], indices[i * 3 + 1], indices[i * 3 + 2]};

        std::unique_ptr<filament::geometry::SurfaceOrientation> orientation(
                filament::geometry::SurfaceOrientation::Builder()
                .vertexCount(count).normals(normals.data()).uvs(result.uv0.data())
                .positions(result.positions.data()).triangleCount(triangles.size())
                .triangles(triangles.data()).build());
        if (orientation) {
            orientation->getQuats(result.tangent_frames.data(), count);
            result.tangent_packing = StaticTangentPacking::PositionUvNormal;
        } else {
            std::unique_ptr<filament::geometry::SurfaceOrientation> fallback(
                    filament::geometry::SurfaceOrientation::Builder()
                    .vertexCount(count).normals(normals.data()).build());
            if (fallback) {
                fallback->getQuats(result.tangent_frames.data(), count);
                result.tangent_packing = StaticTangentPacking::NormalOnlyFallback;
            }
        }
        result.displacement_directions_valid = valid_source_normals &&
                result.tangent_packing != StaticTangentPacking::Unavailable;
        for (std::size_t i = 0; i < count; ++i) {
            const auto decoded = decode_static_displacement_direction(result.tangent_frames[i]);
            if (decoded) result.unit_displacement_directions[i] = *decoded;
            else result.displacement_directions_valid = false;
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{"Static source packing allocation failed"});
    }
}
} // namespace heaven
