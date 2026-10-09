#include "engine/spatial_draw_partition.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>

namespace heaven {
namespace {
bool finite(glm::vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

std::uint32_t morton_key(const std::array<std::uint32_t, 3>& coordinate) {
    std::uint32_t key = 0;
    for (unsigned bit = 0; bit < 10; ++bit)
        for (unsigned axis = 0; axis < 3; ++axis)
            key |= ((coordinate[axis] >> bit) & 1u) << (3 * bit + axis);
    return key;
}
} // namespace

Result<SpatialDrawPartition> partition_spatial_draws(
        std::span<const glm::vec3> positions,
        std::span<const std::uint32_t> indices,
        glm::vec3 padding, std::size_t target_triangles, std::size_t maximum_ranges) {
    if (positions.empty() || indices.empty() || indices.size() % 3 ||
            positions.size() > std::numeric_limits<std::uint32_t>::max() ||
            indices.size() > std::numeric_limits<std::uint32_t>::max() ||
            !target_triangles || !maximum_ranges || !finite(padding) ||
            std::min({padding.x, padding.y, padding.z}) < 0)
        return std::unexpected(Error{"Invalid spatial draw partition input"});
    try {
        glm::vec3 source_min(std::numeric_limits<float>::infinity());
        glm::vec3 source_max(-std::numeric_limits<float>::infinity());
        for (const auto index : indices) {
            if (index >= positions.size() || !finite(positions[index]))
                return std::unexpected(Error{"Invalid spatial draw partition vertex"});
            source_min = glm::min(source_min, positions[index]);
            source_max = glm::max(source_max, positions[index]);
        }
        struct Key { std::uint32_t morton, triangle; };
        const std::size_t triangle_count = indices.size() / 3;
        std::vector<Key> keys(triangle_count);
        for (std::size_t triangle = 0; triangle < triangle_count; ++triangle) {
            std::array<std::uint32_t, 3> coordinate{};
            for (int axis = 0; axis < 3; ++axis) {
                const double extent = double(source_max[axis]) - double(source_min[axis]);
                if (extent > 0) {
                    const double centroid = (double(positions[indices[3 * triangle]][axis]) +
                        double(positions[indices[3 * triangle + 1]][axis]) +
                        double(positions[indices[3 * triangle + 2]][axis])) / 3;
                    coordinate[axis] = static_cast<std::uint32_t>(std::clamp(
                        (centroid - double(source_min[axis])) / extent * 1023, 0.0, 1023.0));
                }
            }
            keys[triangle] = {morton_key(coordinate), static_cast<std::uint32_t>(triangle)};
        }
        std::sort(keys.begin(), keys.end(), [](Key a, Key b) {
            return a.morton < b.morton || (a.morton == b.morton && a.triangle < b.triangle);
        });
        // Division avoids overflowing triangle_count + maximum_ranges - 1.
        const std::size_t minimum_leaf = triangle_count / maximum_ranges +
            (triangle_count % maximum_ranges != 0);
        const std::size_t leaf_size = std::max(target_triangles, minimum_leaf);
        SpatialDrawPartition result;
        result.indices.resize(indices.size());
        result.ranges.reserve(triangle_count / leaf_size + (triangle_count % leaf_size != 0));
        for (std::size_t begin = 0; begin < triangle_count;) {
            const std::size_t count = std::min(leaf_size, triangle_count - begin);
            SpatialDrawRange range;
            range.index_offset = static_cast<std::uint32_t>(begin * 3);
            range.index_count = static_cast<std::uint32_t>(count * 3);
            range.bounds_min = glm::vec3(std::numeric_limits<float>::infinity());
            range.bounds_max = -range.bounds_min;
            for (std::size_t offset = begin; offset < begin + count; ++offset) {
                const auto triangle = keys[offset].triangle;
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    const auto index = indices[3 * static_cast<std::size_t>(triangle) + corner];
                    result.indices[3 * offset + corner] = index;
                    range.bounds_min = glm::min(range.bounds_min, positions[index]);
                    range.bounds_max = glm::max(range.bounds_max, positions[index]);
                }
            }
            for (int axis = 0; axis < 3; ++axis) {
                range.bounds_min[axis] = std::nextafter(range.bounds_min[axis] - padding[axis],
                    -std::numeric_limits<float>::infinity());
                range.bounds_max[axis] = std::nextafter(range.bounds_max[axis] + padding[axis],
                    std::numeric_limits<float>::infinity());
            }
            if (!finite(range.bounds_min) || !finite(range.bounds_max))
                return std::unexpected(Error{"Spatial draw partition bounds overflow"});
            result.ranges.push_back(range);
            begin += count;
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{"Spatial draw partition allocation failed"});
    }
}
} // namespace heaven
