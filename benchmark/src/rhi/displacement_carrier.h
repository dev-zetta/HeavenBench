/* HeavenBench — shared parent geometry attributes for visible and depth draws. */
#ifndef HEAVENBENCH_SRC_RHI_DISPLACEMENT_CARRIER_H_
#define HEAVENBENCH_SRC_RHI_DISPLACEMENT_CARRIER_H_

#include "rhi/displacement_surface_preparer.h"

#include <array>
#include <span>
#include <vector>

namespace heaven {

using DisplacementParentAttributes = std::array<filament::math::float4, 7>;

struct DisplacementCarrier {
    // Parent-local GPU arrays only. This is not an original source snapshot:
    // footprint/direction/signature metadata remains in prepared.original_packed
    // and cannot be reconstructed from these refined attachment records.
    PackedStaticSurface packed;
    std::vector<DisplacementParentAttributes> parents;
    std::vector<std::uint32_t> indices;
    glm::vec3 bounds_min{};
    glm::vec3 bounds_max{};
};

// Parent-local records retain a common node's position, shading frame and
// correction. Only the original parent attachment differs across a shared
// edge. The shader reconstructs that parent's original per-corner fades.
// This CPU helper is shared by MeshStore and the native pass-parity fixture.
[[nodiscard]] Result<DisplacementCarrier> make_displacement_carrier(
        std::span<const Vertex> original_vertices,
        const PreparedDisplacementSurface& prepared);

} // namespace heaven
#endif
