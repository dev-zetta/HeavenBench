/*
 * HeavenBench — GPU-instancing grouping helper (pure, GPU-free)
 *
 * `compute_instance_groups` is the pure logic behind GPU instancing: it groups
 * the scene's `Instance` list by `mesh_index` so that repeated static meshes
 * can be drawn as a single instanced renderable instead of one renderable per
 * placement. It has no Filament/GPU dependency, so it is unit/property testable
 * on the host.
 *
 * Rules (see design.md §"Grouping"):
 *   - Group by `mesh_index` and a 32m world-origin cell at startup.
 *   - Invalid/noncanonical transforms and reflections stay per-placement.
 *   - Exclude any `mesh_index` that is an Animated_Object (skinned mesh) — those
 *     keep their own renderable (and GPU skinning) and are never instanced.
 *   - A mesh/cell bucket with exactly one Instance is a singleton and is
 *     NOT emitted: it stays on the existing non-instanced path (trivially
 *     parity-correct, and avoids a 1-element InstanceBuffer).
 *   - Groups with ≥2 members are emitted; a group with more than
 *     `max_per_group` members is split into consecutive chunks of
 *     ≤ `max_per_group` (Filament's `Engine::getMaxAutomaticInstances()` limit),
 *     preserving ascending member order so the split is deterministic.
 *   - Deterministic: identical scene input yields identical groups and identical
 *     per-group member ordering across runs (groups emitted in ascending
 *     mesh/cell order; members in ascending instance-index order).
 */
#ifndef HEAVENBENCH_SRC_RHI_INSTANCE_GROUPING_H_
#define HEAVENBENCH_SRC_RHI_INSTANCE_GROUPING_H_

#include "engine/types.h"
#include "rhi/instance_bounds.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <unordered_set>
#include <vector>
#include <utility>
#include <tuple>

namespace heaven {

// One instanced group: a set of Instances that share a mesh/cell (and thus
// identical surface selection, materials, and shadow split) drawn by one
// Instanced_Renderable. `instance_indices` are indices into the source
// `instances` span, in ascending order.
struct InstanceGroup {
    int mesh_index;                    // shared GpuMesh
    std::vector<int> instance_indices; // members, in ascending instance order
};

// Compute the instanced groups from a scene's `Instance` span and the set of
// skinned (Animated_Object) mesh indices. Invalid/reflected placements and
// mesh/cell singletons remain on the existing per-placement fallback. See the
// file header for the full rule
// set. `max_per_group` caps the member count of each emitted group (chunking
// for the InstanceBuffer 64-instance limit); a value < 1 is treated as 1 so the
// helper never loops indefinitely.
[[nodiscard]] inline std::vector<InstanceGroup> compute_instance_groups(
        std::span<const Instance> instances,
        const std::unordered_set<int>& skinned_mesh_indices,
        std::size_t max_per_group) {
    // Cells limit coarse group culling without selecting content by camera or
    // light. Groups stay persistent when members move; refreshed bounds cover
    // their current placements, so crossing a cell needs no renderable rebuild.
    using GroupKey = std::tuple<int, std::array<int64_t, 3>>;
    std::map<GroupKey, std::vector<int>> by_mesh;
    for (int i = 0; std::cmp_less(i, instances.size()); ++i) {
        const int mesh_index = instances[i].mesh_index;
        const auto& transform = instances[i].transform;
        if (skinned_mesh_indices.contains(mesh_index) ||
            !instance_transform_supports_grouping(transform)) continue;
        std::array<int64_t, 3> cell{};
        bool representable = true;
        for (int axis = 0; axis < 3; ++axis) {
            const double coordinate = std::floor(double(transform[3][axis]) / 32.0);
            // Avoid undefined floating-to-integer conversion at extreme inputs.
            constexpr double limit = 0x1p63;
            if (coordinate < -limit || coordinate >= limit) representable = false;
            else cell[static_cast<size_t>(axis)] = static_cast<int64_t>(coordinate);
        }
        if (representable) by_mesh[{mesh_index, cell}].push_back(i);
    }

    const std::size_t chunk_size = max_per_group < 1 ? 1 : max_per_group;

    std::vector<InstanceGroup> groups;
    for (const auto& [key, members] : by_mesh) {
        // Singletons stay on the non-instanced path.
        if (members.size() < 2) {
            continue;
        }
        // Split into consecutive chunks of ≤ chunk_size, preserving ascending
        // member order.
        for (std::size_t start = 0; start < members.size(); start += chunk_size) {
            const std::size_t end = std::min(start + chunk_size, members.size());
            InstanceGroup group;
            group.mesh_index = std::get<0>(key);
            group.instance_indices.assign(members.begin() + static_cast<std::ptrdiff_t>(start),
                                          members.begin() + static_cast<std::ptrdiff_t>(end));
            groups.push_back(std::move(group));
        }
    }
    return groups;
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_INSTANCE_GROUPING_H_
