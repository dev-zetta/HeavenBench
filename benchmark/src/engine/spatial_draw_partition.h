#ifndef HEAVENBENCH_SRC_ENGINE_SPATIAL_DRAW_PARTITION_H_
#define HEAVENBENCH_SRC_ENGINE_SPATIAL_DRAW_PARTITION_H_

#include "engine/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace heaven {

struct SpatialDrawRange {
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
    glm::vec3 bounds_min{};
    glm::vec3 bounds_max{};
};

struct SpatialDrawPartition {
    std::vector<std::uint32_t> indices;
    std::vector<SpatialDrawRange> ranges;
};

// Permute complete triangle triplets into deterministic spatial draw ranges.
// Positions are the immutable neutral GPU positions. Padding is the original
// shader displacement envelope in model units, independently of target shape.
// No vertex, winding, triangle multiplicity or geometry attribute is changed.
[[nodiscard]] Result<SpatialDrawPartition> partition_spatial_draws(
        std::span<const glm::vec3> positions,
        std::span<const std::uint32_t> indices,
        glm::vec3 padding,
        std::size_t target_triangles = 32768,
        std::size_t maximum_ranges = 64);

} // namespace heaven
#endif // HEAVENBENCH_SRC_ENGINE_SPATIAL_DRAW_PARTITION_H_
