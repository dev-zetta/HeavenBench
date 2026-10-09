/* HeavenBench — original static vertex packing shared by upload and preparation. */
#ifndef HEAVENBENCH_SRC_RHI_PACKED_STATIC_MESH_H_
#define HEAVENBENCH_SRC_RHI_PACKED_STATIC_MESH_H_

#include "engine/types.h"
#include "rhi/mesh_displacement.h"

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace heaven {

enum class StaticTangentPacking { PositionUvNormal, NormalOnlyFallback, Unavailable };

// These are the original upload arrays, not a recomputation after subdivision.
// Preserve the short4 bytes and CUSTOM0 components when replacing topology.
struct PackedStaticSurface {
    std::vector<filament::math::float3> positions;
    std::vector<filament::math::float2> uv0;
    std::vector<filament::math::float2> uv1;
    std::vector<filament::math::short4> tangent_frames;
    std::vector<filament::math::float4> displacement_custom;
    std::vector<mesh_displacement::Footprint> footprints;
    std::vector<filament::math::float3> unit_displacement_directions;
    std::uint64_t deformation_signature = mesh_displacement::deformation_signature_seed;
    std::uint64_t source_signature = 0;
    StaticTangentPacking tangent_packing = StaticTangentPacking::Unavailable;
    // Invalid/zero source normals or an invalid encoded direction leave the
    // original upload arrays available, but must not enter geometry smoothing.
    bool displacement_directions_valid = false;
};

// Matches SNORM16 attribute conversion followed by Filament toTangentFrame(q,n)
// and the materialVertex normalize(worldNormal), in an identity instance metric.
// No authored normal is substituted for the actual packed tangent-frame normal.
[[nodiscard]] Result<filament::math::float3> decode_static_displacement_direction(
        const filament::math::short4& packed);

// Only the Static Vertex layout is accepted here. Positions, raw authored
// normals, UV0, triangle ordering, fallback mode and getQuats(short4) are the
// shipping MeshStore::upload contract; normals are deliberately not normalized
// before SurfaceOrientation. No Engine, asset lookup or texture decoding.
[[nodiscard]] Result<PackedStaticSurface> pack_static_surface(
        std::span<const Vertex> vertices, std::span<const std::uint32_t> indices);

// Exact P/N/UV0/UV1/magnitude fields and ordered indices; unused authored
// tangents are excluded because SurfaceOrientation never consumes them.
[[nodiscard]] std::uint64_t packed_static_source_signature(
        std::span<const Vertex> vertices, std::span<const std::uint32_t> indices) noexcept;

// Same byte stream as the flat-index overload, without flattening the caller's
// ordered source-parent triples or allocating a temporary index buffer.
[[nodiscard]] std::uint64_t packed_static_source_signature(
        std::span<const Vertex> vertices,
        std::span<const std::array<std::uint32_t, 3>> triangles) noexcept;

} // namespace heaven
#endif
