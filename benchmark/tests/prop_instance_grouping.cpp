// Feature: gpu-instancing-and-skinning,: Instance grouping is total, deterministic, mesh-keyed, skinned-excluded, and chunk-bounded
/*
 * HeavenBench — Property test for the pure GPU-instancing grouping helper
 * (gpu-instancing-and-skinning, design.md §"Grouping").
 *
 * Feature: gpu-instancing-and-skinning
 *   •: Instance grouping is total, deterministic, mesh-keyed,
 *     skinned-excluded, and chunk-bounded
 *
 *
 * `compute_instance_groups` (rhi/instance_grouping.h) is the pure, GPU-free
 * logic behind GPU instancing: it buckets the scene's `Instance` list by
 * mesh/cell, drops skinned/noncanonical/reflected placements and singletons, and emits
 * the remaining ≥2-member buckets split into chunks of ≤ `max_per_group`
 * (Filament's `Engine::getMaxAutomaticInstances()` clamp), with members in
 * ascending instance-index order.
 *
 * This property pins every clause of that contract across fuzzed scenes:
 *
 *   1. Skinned-excluded: no emitted group's `mesh_index` is in the
 *      skinned set, and no emitted instance index references a skinned mesh.
 *   2. Mesh-keyed: every member of a group references that group's
 *      `mesh_index` — no group mixes meshes.
 *   3. Chunk-bounded: every emitted group has between 1 and `max_per_group`
 *      members (the helper clamps a `max_per_group` < 1 up to 1).
 *   4. Totality + singleton/skinned exclusion: the multiset of
 *      emitted instance indices equals EXACTLY the set of instance indices whose
 *      mesh/cell is eligible and referenced by ≥2 instances — each such instance
 *      appears in exactly one emitted group, and no singleton/skinned instance
 *      appears at all.
 *   5. Determinism: the same input yields byte-identical groups
 *      (order, mesh keys, and member ordering) across repeated calls, and
 *      members are in strictly ascending instance order within each group while
 *      groups are in non-decreasing `mesh_index` order.
 *
 * The helper is header-only and GPU-free, so it is exercised directly on the
 * host. This file self-registers with the RapidCheck harness in test_main.cpp
 * via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 */
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <unordered_set>
#include <tuple>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <rapidcheck.h>

#include "rhi/instance_grouping.h" // heaven::compute_instance_groups, InstanceGroup, Instance

// Mirror the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Build a fuzzed scene of `Instance`s. The grouping logic only reads
// `mesh_index` and spatial placement. mesh_index is drawn from a
// small range. Spatial cells, reflected transforms and nonunit W exercise the
// distinction between eligible groups and per-placement fallback.
std::vector<heaven::Instance> gen_instances(int count, int mesh_range) {
    std::vector<heaven::Instance> instances;
    instances.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        heaven::Instance inst;
        inst.mesh_index = *rc::gen::inRange(0, mesh_range);
        inst.transform = glm::mat4(1.0f);
        for (int axis = 0; axis < 3; ++axis)
            inst.transform[3][axis] = axis == 0
                ? static_cast<float>(*rc::gen::inRange(-40, 41)) : 0;
        const int kind = *rc::gen::inRange(0, 8);
        if (kind == 0) inst.transform[3][3] = .8f;
        if (kind == 1) inst.transform[0][0] = -1;
        instances.push_back(inst);
    }
    return instances;
}

// Randomly mark some mesh indices in [0, mesh_range) as skinned
// (Animated_Objects), which must never appear in any emitted group.
std::unordered_set<int> gen_skinned(int mesh_range) {
    std::unordered_set<int> skinned;
    for (int m = 0; m < mesh_range; ++m) {
        // Roughly one in four meshes is skinned so most scenes carry a mix of
        // skinned and instanceable meshes.
        if (*rc::gen::inRange(0, 4) == 0) {
            skinned.insert(m);
        }
    }
    return skinned;
}

// Compute the reference set: eligible non-skinned mesh/cell buckets with
// at least two members. All other placements remain singleton fallbacks.
std::set<int> expected_emitted(const std::vector<heaven::Instance>& instances,
                               const std::unordered_set<int>& skinned) {
    // Generator matrices are translations with either positive identity,
    // reflected X, or nonunit W. This oracle does not call production guards.
    std::map<std::tuple<int, int, int, int>, std::vector<int>> buckets;
    for (int i = 0; std::cmp_less(i, instances.size()); ++i) {
        const auto& instance = instances[static_cast<std::size_t>(i)];
        if (skinned.contains(instance.mesh_index) || instance.transform[3][3] != 1 ||
            instance.transform[0][0] != 1) continue;
        const auto p = instance.transform[3];
        buckets[{instance.mesh_index, static_cast<int>(std::floor(double(p.x) / 32)),
            static_cast<int>(std::floor(double(p.y) / 32)),
            static_cast<int>(std::floor(double(p.z) / 32))}].push_back(i);
    }
    std::set<int> expected;
    for (const auto& [key, members] : buckets) {
        if (members.size() >= 2) {
            for (int idx : members) {
                expected.insert(idx);
            }
        }
    }
    return expected;
}

bool groups_equal(const std::vector<heaven::InstanceGroup>& a,
                  const std::vector<heaven::InstanceGroup>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].mesh_index != b[i].mesh_index ||
            a[i].instance_indices != b[i].instance_indices) {
            return false;
        }
    }
    return true;
}

bool run_p1_instance_grouping() {
    return rc::check(
        "Feature: gpu-instancing-and-skinning, Property 1: Instance grouping is "
        "total, deterministic, mesh-keyed, skinned-excluded, and chunk-bounded "
        "(Validates: Requirements 1.1, 1.4, 1.5)",
        [] {
            const int mesh_range = *rc::gen::inRange(1, 8);
            const int count = *rc::gen::inRange(0, 48);
            const std::size_t max_per_group =
                static_cast<std::size_t>(*rc::gen::inRange(1, 12));

            const std::vector<heaven::Instance> instances =
                gen_instances(count, mesh_range);
            const std::unordered_set<int> skinned = gen_skinned(mesh_range);

            const std::vector<heaven::InstanceGroup> groups =
                heaven::compute_instance_groups(
                    std::span<const heaven::Instance>(instances), skinned,
                    max_per_group);

            // ── Per-group invariants ───────────────────────────
            std::multiset<int> emitted_members;
            int prev_mesh_index = -1; // groups are in non-decreasing mesh order
            for (const heaven::InstanceGroup& g : groups) {
                // Skinned-excluded: no skinned mesh is ever grouped.
                RC_ASSERT(!skinned.contains(g.mesh_index));

                // Groups emitted in non-decreasing mesh_index order (a facet of
                // the deterministic ordering contract).
                RC_ASSERT(g.mesh_index >= prev_mesh_index);
                prev_mesh_index = g.mesh_index;

                // Chunk-bounded: 1.. max_per_group members per emitted group.
                RC_ASSERT(!g.instance_indices.empty());
                RC_ASSERT(g.instance_indices.size() <= max_per_group);

                int prev_member = -1;
                for (int idx : g.instance_indices) {
                    // Members are valid indices into the source span.
                    RC_ASSERT(idx >= 0 &&
                              std::cmp_less(idx, instances.size()));
                    // Mesh-keyed: every member references this group's
                    // mesh — no group mixes mesh indices.
                    RC_ASSERT(instances[static_cast<std::size_t>(idx)]
                                  .mesh_index == g.mesh_index);
                    // Strictly ascending member order within a group.
                    RC_ASSERT(idx > prev_member);
                    prev_member = idx;
                    const auto& transform = instances[static_cast<std::size_t>(idx)].transform;
                    RC_ASSERT(transform[3][3] == 1 && transform[0][0] == 1);
                    const auto& first = instances[static_cast<std::size_t>(g.instance_indices.front())].transform;
                    for (int axis = 0; axis < 3; ++axis)
                        RC_ASSERT(std::floor(double(transform[3][axis]) / 32) ==
                            std::floor(double(first[3][axis]) / 32));
                    emitted_members.insert(idx);
                }
            }

            // ── Totality across groups  ──────────
            // Every emitted index appears exactly once (no duplicates across
            // chunks/groups): the multiset has no repeats.
            std::set<int> emitted_unique(emitted_members.begin(),
                                         emitted_members.end());
            RC_ASSERT(emitted_unique.size() == emitted_members.size());

            // The emitted set equals EXACTLY the non-skinned, multiply-
            // referenced instances — singletons and skinned instances never
            // appear, and every qualifying instance appears in one group.
            const std::set<int> expected =
                expected_emitted(instances, skinned);
            RC_ASSERT(emitted_unique == expected);

            // ── Determinism  ──────────────────────────
            // Identical input yields byte-identical output across calls.
            const std::vector<heaven::InstanceGroup> groups2 =
                heaven::compute_instance_groups(
                    std::span<const heaven::Instance>(instances), skinned,
                    max_per_group);
            RC_ASSERT(groups_equal(groups, groups2));
        });
}

bool run_instance_grouping_properties() {
    return run_p1_instance_grouping();
}

const heaven::testing::Register g_register_instance_grouping{
    &run_instance_grouping_properties};

} // namespace
