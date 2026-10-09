#include "engine/grass_spatial_partition.h"

#include <glm/gtc/matrix_transform.hpp>

#include <functional>
#include <numeric>
#include <utility>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
} // namespace heaven::testing

namespace {

bool check_partition(std::span<const heaven::GrassVertex> vertices, std::size_t stride) {
    const auto original = std::vector(vertices.begin(), vertices.end());
    const auto partition = heaven::partition_grass_geometry(vertices, stride, 2, 4);
    const auto repeat = heaven::partition_grass_geometry(vertices, stride, 2, 4);
    if (!partition || !repeat || partition->indices != repeat->indices ||
            !std::ranges::equal(std::as_bytes(vertices), std::as_bytes(std::span(original)))) return false;
    // The index permutation is complete, and each original blade/card is an
    // indivisible ordered block. This independently preserves every oriented
    // triangle, normal, UV, color and per-blade wind-height attribute.
    auto sorted = partition->indices;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = 0; i < sorted.size(); ++i)
        if (sorted[i] != i) return false;
    std::size_t next = 0;
    for (std::size_t leaf = 0; leaf < partition->ranges.size(); ++leaf) {
        const auto& range = partition->ranges[leaf];
        const auto& again = repeat->ranges[leaf];
        if (range.index_offset != next || range.index_count % stride != 0 ||
                range.index_count == 0 || range.index_offset != again.index_offset ||
                range.index_count != again.index_count ||
                range.geometry.min != again.geometry.min || range.geometry.max != again.geometry.max ||
                range.geometry.max_wind_height_squared != again.geometry.max_wind_height_squared)
            return false;
        std::uint32_t previous = 0;
        for (std::size_t offset = next; offset < next + range.index_count; offset += stride) {
            const auto first = partition->indices[offset];
            if (first % stride != 0 || (offset > next && first <= previous)) return false;
            previous = first;
            for (std::size_t vertex = 0; vertex < stride; ++vertex)
                if (partition->indices[offset + vertex] != first + vertex) return false;
        }
        for (const glm::vec3 scale : {glm::vec3(1.0f), glm::vec3(-0.03f, 2.0f, 0.5f)}) {
            glm::mat4 transform = glm::rotate(glm::mat4(1.0f), 0.8f,
                                              glm::normalize(glm::vec3(1, 2, 3)));
            transform = glm::scale(transform, scale);
            transform[3] = glm::vec4(40, -30, 17, 1);
            const auto bounds = heaven::grass_wind_bounds(range.geometry, transform);
            if (!bounds) return false;
            const glm::dmat4 inverse = glm::inverse(glm::dmat4(transform));
            for (std::size_t i = next; i < next + range.index_count; ++i) {
                const auto& vertex = vertices[partition->indices[i]];
                const auto world = glm::dmat4(transform) *
                    glm::dvec4(vertex.px, vertex.py, vertex.pz, 1);
                for (double phase : {-1.0, 0.0, 1.0}) {
                    // Both sine components can reach their extremum together.
                    const double sway = phase *
                        (std::abs(heaven::render_tuning::grass.wind_tuning.z) +
                         std::abs(heaven::render_tuning::grass.wind_tuning.w)) * vertex.v * vertex.v;
                    const auto local = inverse * (world +
                        glm::dvec4(0.8 * sway, 0.3 * sway, -0.2 * std::abs(sway), 0));
                    for (glm::length_t axis = 0; axis < 3; ++axis)
                        if (local[axis] < bounds->min[axis] || local[axis] > bounds->max[axis])
                            return false;
                }
            }
        }
        next += range.index_count;
    }
    return next == vertices.size();
}

bool unit_grass_ranges_preserve_atomic_geometry_and_wind() {
    const std::vector<heaven::GrassRoot> roots{
        {{9, 0, 1}}, {{-8, 1, 0}}, {{7, 2, -1}}, {{-6, 3, 2}},
        {{5, 4, 0}}, {{-4, 5, 1}}, {{3, 6, -1}}, {{-2, 7, 0}}};
    return check_partition(heaven::generate_grass_blades(roots, 0), heaven::GRASS_VERTS_PER_BLADE) &&
           check_partition(heaven::generate_flower_cards(roots), heaven::FLOWER_VERTS_PER_CARD);
}

bool unit_grass_ranges_cull_distant_blades_without_changing_density() {
    std::vector<heaven::GrassRoot> roots;
    // Interleaving makes a source-order split just as coarse as the old draw.
    for (int i = 0; i < 4; ++i) {
        roots.push_back({{-100, static_cast<float>(i), 0}});
        roots.push_back({{100, static_cast<float>(i), 0}});
    }
    const auto vertices = heaven::generate_grass_blades(roots, 0);
    const auto partition = heaven::partition_grass_geometry(
        vertices, heaven::GRASS_VERTS_PER_BLADE, 4, 2);
    if (!partition || partition->ranges.size() != 2) return false;
    // An axis-aligned orthographic frustum around the right-hand meadow.
    // The old whole-island box intersects and submits all eight blades; tight
    // range boxes can exclude four while preserving all eight in the buffers.
    const auto intersects = [](const heaven::GrassBounds& b) {
        return b.max.x >= 95 && b.min.x <= 105 && b.max.y >= -5 &&
               b.min.y <= 8 && b.max.z >= -5 && b.min.z <= 8;
    };
    const auto whole = heaven::grass_wind_bounds(
        heaven::grass_geometry_bounds(vertices), glm::mat4(1));
    if (!whole || !intersects(*whole)) return false;
    std::size_t eligible_vertices = 0;
    for (const auto& range : partition->ranges) {
        const auto bounds = heaven::grass_wind_bounds(range.geometry, glm::mat4(1));
        if (!bounds) return false;
        if (intersects(*bounds)) eligible_vertices += range.index_count;
    }
    return partition->indices.size() == vertices.size() &&
           eligible_vertices == vertices.size() / 2;
}

bool unit_grass_ranges_bound_draw_overhead_and_keep_small_order() {
    const std::vector<heaven::GrassRoot> roots(257, {{0, 0, 0}});
    const auto vertices = heaven::generate_flower_cards(roots);
    const auto small = heaven::partition_grass_geometry(vertices, heaven::FLOWER_VERTS_PER_CARD);
    const auto capped = heaven::partition_grass_geometry(vertices, heaven::FLOWER_VERTS_PER_CARD, 1, 64);
    if (!small || !capped || small->ranges.size() != 1 || capped->ranges.size() > 64)
        return false;
    for (std::size_t i = 0; i < vertices.size(); ++i)
        if (small->indices[i] != i) return false;
    auto invalid = vertices;
    invalid[0].px = std::numeric_limits<float>::quiet_NaN();
    return !heaven::partition_grass_geometry({}, 21) &&
           !heaven::partition_grass_geometry(vertices, 0) &&
           !heaven::partition_grass_geometry(vertices, 7) &&
           !heaven::partition_grass_geometry(vertices, 12, 0) &&
           !heaven::partition_grass_geometry(vertices, 12, 1, 0) &&
           !heaven::partition_grass_geometry(invalid, 12);
}

const heaven::testing::Register g_grass_atomic_ranges{unit_grass_ranges_preserve_atomic_geometry_and_wind};
const heaven::testing::Register g_grass_culling_ranges{unit_grass_ranges_cull_distant_blades_without_changing_density};
const heaven::testing::Register g_grass_bounded_ranges{unit_grass_ranges_bound_draw_overhead_and_keep_small_order};

} // namespace
