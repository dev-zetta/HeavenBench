#ifndef HEAVENBENCH_SRC_ENGINE_GRASS_SPATIAL_PARTITION_H_
#define HEAVENBENCH_SRC_ENGINE_GRASS_SPATIAL_PARTITION_H_

#include "engine/grass_bounds.h"

#include <array>
#include <cstdint>
#include <new>

namespace heaven {

struct GrassDrawRange {
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
    GrassBounds geometry;
};

struct GrassSpatialPartition {
    std::vector<std::uint32_t> indices;
    std::vector<GrassDrawRange> ranges;
};

// Keep the generated vertex stream immutable. Group complete blades/cards by
// source-local position and permute only their identity-index blocks, retaining
// every triangle and its winding. One shared IB serves color and depth ranges.
// The range ceiling bounds persistent entity/draw overhead independently of
// density; small fixtures keep their original single draw and index order.
[[nodiscard]] inline Result<GrassSpatialPartition> partition_grass_geometry(
        std::span<const GrassVertex> vertices, std::size_t vertices_per_root,
        std::size_t target_roots = 8192, std::size_t maximum_ranges = 64) {
    if (vertices.empty() || vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
            vertices_per_root == 0 || vertices_per_root % 3 != 0 ||
            vertices.size() % vertices_per_root != 0 || target_roots == 0 || maximum_ranges == 0)
        return std::unexpected(Error{"Invalid grass spatial partition input"});
    const auto whole = grass_geometry_bounds(vertices);
    if (!whole.valid)
        return std::unexpected(Error{"Invalid grass spatial partition bounds"});
    try {
        const std::size_t roots = vertices.size() / vertices_per_root;
        const std::size_t minimum_leaf = roots / maximum_ranges +
            (roots % maximum_ranges != 0);
        const std::size_t leaf_size = std::max(target_roots, minimum_leaf);
        struct Key { std::uint32_t morton, root; };
        std::vector<Key> keys(roots);
        // Use one physical scale on every axis. Independently normalizing a
        // thin island's axes can group opposite ends together merely because
        // their blade heights or lateral coordinates differ slightly.
        double extent = 0.0;
        for (glm::length_t axis = 0; axis < 3; ++axis)
            extent = std::max(extent, static_cast<double>(whole.max[axis]) - whole.min[axis]);
        for (std::size_t root = 0; root < roots; ++root) {
            std::uint32_t morton = 0;
            const auto& vertex = vertices[root * vertices_per_root];
            const std::array position{vertex.px, vertex.py, vertex.pz};
            for (unsigned axis = 0; axis < 3; ++axis) {
                const auto coordinate = extent > 0.0
                    ? static_cast<std::uint32_t>(std::clamp(
                        (static_cast<double>(position[axis]) -
                            whole.min[static_cast<glm::length_t>(axis)]) /
                            extent * 1023.0, 0.0, 1023.0)) : 0u;
                for (unsigned bit = 0; bit < 10; ++bit)
                    morton |= ((coordinate >> bit) & 1u) << (3 * bit + axis);
            }
            keys[root] = {morton, static_cast<std::uint32_t>(root)};
        }
        if (roots > leaf_size) {
            std::sort(keys.begin(), keys.end(), [](Key a, Key b) {
                return a.morton < b.morton || (a.morton == b.morton && a.root < b.root);
            });
        }
        GrassSpatialPartition result;
        result.indices.resize(vertices.size());
        result.ranges.reserve(roots / leaf_size + (roots % leaf_size != 0));
        for (std::size_t begin = 0; begin < roots;) {
            const std::size_t count = std::min(leaf_size, roots - begin);
            // Spatial membership changes draw order only between leaves;
            // retain the original source order of all blades inside a leaf.
            std::sort(keys.begin() + static_cast<std::ptrdiff_t>(begin),
                      keys.begin() + static_cast<std::ptrdiff_t>(begin + count),
                      [](Key a, Key b) { return a.root < b.root; });
            GrassDrawRange range;
            range.index_offset = static_cast<std::uint32_t>(begin * vertices_per_root);
            range.index_count = static_cast<std::uint32_t>(count * vertices_per_root);
            for (std::size_t offset = begin; offset < begin + count; ++offset) {
                const std::size_t first = static_cast<std::size_t>(keys[offset].root) * vertices_per_root;
                const auto bounds = grass_geometry_bounds(vertices.subspan(first, vertices_per_root));
                if (!range.geometry.valid) range.geometry = bounds;
                else {
                    range.geometry.min = glm::min(range.geometry.min, bounds.min);
                    range.geometry.max = glm::max(range.geometry.max, bounds.max);
                    range.geometry.max_wind_height_squared = std::max(
                        range.geometry.max_wind_height_squared, bounds.max_wind_height_squared);
                }
                for (std::size_t vertex = 0; vertex < vertices_per_root; ++vertex)
                    result.indices[offset * vertices_per_root + vertex] =
                        static_cast<std::uint32_t>(first + vertex);
            }
            result.ranges.push_back(range);
            begin += count;
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{"Grass spatial partition allocation failed"});
    }
}

} // namespace heaven
#endif // HEAVENBENCH_SRC_ENGINE_GRASS_SPATIAL_PARTITION_H_
