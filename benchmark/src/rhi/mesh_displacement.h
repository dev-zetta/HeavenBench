/* HeavenBench — mesh-space sampling footprints for height displacement. */
#ifndef HEAVENBENCH_SRC_RHI_MESH_DISPLACEMENT_H_
#define HEAVENBENCH_SRC_RHI_MESH_DISPLACEMENT_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace heaven::mesh_displacement {

using Footprint = std::array<float, 2>;

inline uint32_t coordinate_bits(float value) noexcept {
    // Opposite signs of zero denote the same point and UV coordinate.
    return value == 0.0f ? 0u : std::bit_cast<uint32_t>(value);
}

inline constexpr uint64_t deformation_signature_seed = 14695981039346656037ull;

inline uint64_t append_deformation_signature(
        uint64_t signature, float u, float v, float normal_magnitude) noexcept {
    for (const float value : {u, v, normal_magnitude}) {
        signature ^= coordinate_bits(value);
        signature *= 1099511628211ull;
    }
    return signature;
}

// Vertex and SkinnedVertex expose the same position and UV0 fields. Indices
// have already passed the mesh loader's bounds checks.
template <typename VertexType>
std::vector<Footprint> calculate_footprints(
        std::span<const VertexType> vertices,
        std::span<const uint32_t> indices) {
    std::vector<Footprint> footprints(vertices.size(), Footprint{0.0f, 0.0f});
    for (std::size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3) {
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const uint32_t a = indices[triangle + edge];
            const uint32_t b = indices[triangle + (edge + 1) % 3];
            const float du = std::abs(vertices[b].u - vertices[a].u);
            const float dv = std::abs(vertices[b].v - vertices[a].v);
            for (const uint32_t index : {a, b}) {
                footprints[index][0] = std::max(footprints[index][0], du);
                footprints[index][1] = std::max(footprints[index][1], dv);
            }
        }
    }

    // Source meshes can duplicate an otherwise identical point at a tangent
    // seam. Share its filter footprint so the two copies cannot sample different
    // height mip levels. Match exact position and UV0 within this surface only;
    // no vertices, normals or topology are welded. Sorting vertex indices keeps
    // temporary storage to four bytes per vertex instead of a hash node per point.
    auto location_key = [&](uint32_t index) {
        const auto& vertex = vertices[index];
        return std::array<uint32_t, 5>{
            coordinate_bits(vertex.px), coordinate_bits(vertex.py),
            coordinate_bits(vertex.pz), coordinate_bits(vertex.u),
            coordinate_bits(vertex.v)};
    };
    std::vector<uint32_t> order(vertices.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return location_key(a) < location_key(b);
    });
    for (std::size_t first = 0; first < order.size();) {
        const auto key = location_key(order[first]);
        Footprint footprint = footprints[order[first]];
        std::size_t end = first + 1;
        while (end < order.size() && location_key(order[end]) == key) {
            const auto& other = footprints[order[end]];
            footprint[0] = std::max(footprint[0], other[0]);
            footprint[1] = std::max(footprint[1], other[1]);
            ++end;
        }
        for (std::size_t index = first; index < end; ++index) {
            footprints[order[index]] = footprint;
        }
        first = end;
    }
    return footprints;
}

} // namespace heaven::mesh_displacement

#endif // HEAVENBENCH_SRC_RHI_MESH_DISPLACEMENT_H_
