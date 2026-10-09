#include "engine/spatial_draw_partition.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
} // namespace heaven::testing

namespace {
using Triangle = std::array<std::uint32_t, 3>;

std::vector<Triangle> triangle_multiset(std::span<const std::uint32_t> indices) {
    std::vector<Triangle> result;
    for (std::size_t i = 0; i < indices.size(); i += 3)
        result.push_back({indices[i], indices[i + 1], indices[i + 2]});
    std::sort(result.begin(), result.end());
    return result;
}

bool spatial_draw_partition_test() {
    using namespace heaven;
    std::vector<glm::vec3> positions;
    std::vector<std::uint32_t> indices;
    // Deliberately interleave distant patches; a contiguous source-order cut
    // would span the whole surface. Spatial ranges must retain winding exactly.
    for (const float x : {7.f, 0.f, 6.f, 1.f, 5.f, 2.f, 4.f, 3.f}) {
        const auto base = static_cast<std::uint32_t>(positions.size());
        positions.insert(positions.end(), {{x - .1f, 0, 0}, {x + .1f, 0, 0}, {x, .1f, 0}});
        indices.insert(indices.end(), {base, base + 1, base + 2});
    }
    const auto original_indices = indices;
    const glm::vec3 padding{.2f, .3f, .4f};
    const auto partition = partition_spatial_draws(positions, indices, padding, 2, 3);
    const auto repeated = partition_spatial_draws(positions, indices, padding, 2, 3);
    bool ok = partition && repeated;
    if (!ok) return false;
    ok &= indices == original_indices && partition->indices == repeated->indices &&
        partition->ranges.size() == 3 && partition->indices.size() == indices.size() &&
        triangle_multiset(partition->indices) == triangle_multiset(indices);
    std::uint32_t next = 0;
    for (std::size_t leaf = 0; leaf < partition->ranges.size(); ++leaf) {
        const auto& range = partition->ranges[leaf];
        const auto& again = repeated->ranges[leaf];
        ok &= range.index_offset == next && range.index_count > 0 && range.index_count % 3 == 0 &&
            range.index_count <= 9 && range.index_offset == again.index_offset &&
            range.index_count == again.index_count && range.bounds_min == again.bounds_min &&
            range.bounds_max == again.bounds_max;
        // Three adjacent patches have a substantially narrower spatial box.
        ok &= range.bounds_max.x - range.bounds_min.x < 3;
        for (std::size_t i = range.index_offset; i < range.index_offset + range.index_count; ++i) {
            const auto p = positions[partition->indices[i]];
            for (int axis = 0; axis < 3; ++axis) {
                ok &= double(range.bounds_min[axis]) <= double(p[axis]) - double(padding[axis]);
                ok &= double(range.bounds_max[axis]) >= double(p[axis]) + double(padding[axis]);
            }
        }
        next += range.index_count;
    }
    ok &= next == indices.size();
    if (!ok) std::fprintf(stderr, "[spatial_draw_partition] permutation/range/envelope invariant failed\n");
    return ok;
}

bool spatial_draw_partition_multiplicity_test() {
    using namespace heaven;
    const std::array<glm::vec3, 3> flat{{{-1, 0, 0}, {0, 0, 0}, {1, 0, 0}}};
    const std::array<std::uint32_t, 12> tie{{0, 1, 2, 1, 2, 0, 2, 0, 1, 2, 1, 0}};
    const auto tied = partition_spatial_draws(flat, tie, {}, 1, 64);
    const std::array<std::uint32_t, 12> duplicates{{0, 1, 2, 0, 1, 2, 2, 1, 0, 0, 0, 0}};
    const auto duplicate = partition_spatial_draws(flat, duplicates, {}, 1, 64);
    std::vector<std::uint32_t> large;
    large.reserve(std::size_t{65537} * 3);
    for (std::size_t i = 0; i < 65537; ++i) large.insert(large.end(), {0, 1, 2});
    const auto defaults = partition_spatial_draws(flat, large, {});
    const auto capped = partition_spatial_draws(flat, large, {}, 1, 64);
    const bool ok = tied && std::equal(tie.begin(), tie.end(), tied->indices.begin()) &&
        duplicate && triangle_multiset(duplicates) == triangle_multiset(duplicate->indices) &&
        defaults && defaults->ranges.size() == 3 && defaults->indices.size() == large.size() &&
        capped && capped->ranges.size() <= 64 && capped->indices == large;
    if (!ok) std::fprintf(stderr, "[spatial_draw_partition] multiplicity/tie/cap invariant failed\n");
    return ok;
}

bool spatial_draw_partition_invalid_test() {
    using namespace heaven;
    const std::array<glm::vec3, 3> positions{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}};
    const std::array<std::uint32_t, 3> indices{{0, 1, 2}}, invalid{{0, 1, 3}};
    auto nonfinite = positions;
    nonfinite[0].x = std::numeric_limits<float>::quiet_NaN();
    auto overflow = positions;
    for (auto& p : overflow) p.x = std::numeric_limits<float>::max();
    const bool ok = !partition_spatial_draws({}, indices, {}) &&
        !partition_spatial_draws(positions, {}, {}) &&
        !partition_spatial_draws(positions, std::span(indices).first(2), {}) &&
        !partition_spatial_draws(positions, invalid, {}) &&
        !partition_spatial_draws(nonfinite, indices, {}) &&
        !partition_spatial_draws(positions, indices, {-1, 0, 0}) &&
        !partition_spatial_draws(positions, indices, {std::numeric_limits<float>::infinity(), 0, 0}) &&
        !partition_spatial_draws(positions, indices, {}, 0, 64) &&
        !partition_spatial_draws(positions, indices, {}, 1, 0) &&
        !partition_spatial_draws(overflow, indices, {std::numeric_limits<float>::max(), 0, 0});
    if (!ok) std::fprintf(stderr, "[spatial_draw_partition] invalid input was accepted\n");
    return ok;
}

heaven::testing::Register spatial_draw_partition_registration(spatial_draw_partition_test);
heaven::testing::Register spatial_draw_partition_multiplicity_registration(spatial_draw_partition_multiplicity_test);
heaven::testing::Register spatial_draw_partition_invalid_registration(spatial_draw_partition_invalid_test);
} // namespace
