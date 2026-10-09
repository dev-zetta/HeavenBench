#include "rhi/instance_bounds.h"
#include "rhi/instance_grouping.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
} // namespace heaven::testing

namespace {
bool contains_transformed_corners(const heaven::InstanceWorldBounds& bounds,
        const heaven::InstanceLocalBounds& local, std::span<const heaven::Instance> instances) {
    if (!bounds.culling) return false;
    for (const auto& instance : instances)
        for (unsigned corner = 0; corner < 8; ++corner) {
            const glm::vec3 p((corner & 1) ? local.max.x : local.min.x,
                (corner & 2) ? local.max.y : local.min.y,
                (corner & 4) ? local.max.z : local.min.z);
            const glm::vec3 world(instance.transform * glm::vec4(p, 1));
            for (int axis = 0; axis < 3; ++axis)
                if (world[axis] < bounds.min[axis] || world[axis] > bounds.max[axis]) return false;
        }
    return true;
}

bool instance_bounds_movement_test() {
    using namespace heaven;
    InstanceLocalBounds local;
    include_instance_local_bounds(local, {-2,-1,-.5f}, {2,1,.5f}, false);
    std::array<Instance, 2> instances{{{0, glm::mat4(1)}, {0, glm::mat4(1)}}};
    const std::array<int, 2> members{0,1};
    const auto initial = instance_group_world_bounds(local, instances, members);
    instances[1].transform = glm::translate(glm::mat4(1), {80,-40,11}) *
        glm::rotate(glm::mat4(1), .73f, glm::normalize(glm::vec3(1,2,3))) *
        glm::scale(glm::mat4(1), {2,.5f,1.25f});
    const auto moved = instance_group_world_bounds(local, instances, members);
    bool ok = initial.culling && moved.culling && moved.max.x > initial.max.x + 60 &&
        moved.min.y < initial.min.y - 30 && contains_transformed_corners(moved, local, instances);
    // The stale startup box (even with the former 16m allowance) misses this member.
    ok &= initial.max.x + 16 < instances[1].transform[3].x;
    instances[0].transform = glm::translate(glm::mat4(1), {-100,0,0});
    const auto moved_again = instance_group_world_bounds(local, instances, members);
    ok &= moved_again.min.x < -100 && contains_transformed_corners(moved_again, local, instances);
    // A displaced ordinary group retains its inherited WORLD allowance under scale.
    local.displaced = true;
    instances[0].transform = instances[1].transform = glm::scale(glm::mat4(1), glm::vec3(.01f));
    const auto displaced = instance_group_world_bounds(local, instances, members);
    ok &= displaced.culling && displaced.min.x < -16.019f && displaced.max.x > 16.019f;
    if (!ok) std::fprintf(stderr, "[instance_bounds] movement/rotation/envelope failed\n");
    return ok;
}

bool instance_bounds_invalid_test() {
    using namespace heaven;
    InstanceLocalBounds local;
    include_instance_local_bounds(local, glm::vec3(-1), glm::vec3(1), false);
    std::array<Instance, 1> instances{{{0, glm::mat4(1)}}};
    const std::array<int, 1> members{0};
    bool ok = true;
    for (const float w : {.8f, 0.f, -1.f, std::numeric_limits<float>::quiet_NaN()}) {
        instances[0].transform[3][3] = w;
        ok &= !instance_group_world_bounds(local, instances, members).culling &&
            !instance_transform_supports_culling(instances[0].transform);
    }
    instances[0].transform = glm::mat4(1);
    instances[0].transform[0][3] = std::numeric_limits<float>::denorm_min();
    ok &= !instance_group_world_bounds(local, instances, members).culling;
    instances[0].transform = glm::mat4(1);
    instances[0].transform[0][0] = -1;
    ok &= !instance_transform_supports_grouping(instances[0].transform) &&
        instance_transform_supports_culling(instances[0].transform);
    instances[0].transform[0][0] = 0;
    ok &= !instance_group_world_bounds(local, instances, members).culling;
    instances[0].transform = glm::mat4(1);
    instances[0].transform[3][0] = std::numeric_limits<float>::infinity();
    ok &= !instance_group_world_bounds(local, instances, members).culling;
    instances[0].transform = glm::mat4(1);
    const std::array<int, 1> invalid_member{1};
    ok &= !instance_group_world_bounds(local, instances, invalid_member).culling;
    // Finite endpoints can still overflow Filament Box::set center or extent.
    const float huge = std::numeric_limits<float>::max() * .75f;
    InstanceLocalBounds overflow;
    include_instance_local_bounds(overflow, glm::vec3(huge), glm::vec3(huge), false);
    ok &= !instance_group_world_bounds(overflow, instances, members).culling;
    overflow = {};
    include_instance_local_bounds(overflow, glm::vec3(-huge), glm::vec3(huge), false);
    ok &= !instance_group_world_bounds(overflow, instances, members).culling;
    include_instance_local_bounds(local, {std::numeric_limits<float>::quiet_NaN(),0,0}, {}, false);
    ok &= !instance_group_world_bounds(local, instances, members).culling;
    if (!ok) std::fprintf(stderr, "[instance_bounds] invalid/member guard failed\n");
    return ok;
}

bool instance_spatial_grouping_test() {
    using namespace heaven;
    std::vector<Instance> instances;
    for (const float x : {0.f,1.f,64.f,65.f,-1.f,-2.f,31.f,32.f,33.f,200.f})
        instances.push_back({0, glm::translate(glm::mat4(1), {x,0,0})});
    instances.push_back({1, glm::mat4(1)});
    instances.push_back({1, glm::mat4(1)}); // skinned exclusion
    instances.push_back({0, glm::scale(glm::mat4(1), {-1,1,1})});
    auto invalid = glm::mat4(1); invalid[3][3] = .8f;
    instances.push_back({0, invalid});
    invalid = glm::mat4(1); invalid[3].x = std::numeric_limits<float>::max();
    instances.push_back({0, invalid}); // unrepresentable cell, per-placement fallback
    const auto groups = compute_instance_groups(instances, {1}, 2);
    const auto repeated = compute_instance_groups(instances, {1}, 2);
    bool ok = groups.size() == 5 && repeated.size() == groups.size();
    std::set<int> grouped;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto& group = groups[i];
        ok &= group.mesh_index == 0 && group.instance_indices == repeated[i].instance_indices &&
            !group.instance_indices.empty() && group.instance_indices.size() <= 2;
        const auto first = static_cast<std::size_t>(group.instance_indices.front());
        const auto cell = std::floor(instances[first].transform[3].x / 32.f);
        for (const int member : group.instance_indices) {
            ok &= grouped.insert(member).second &&
                std::floor(instances[static_cast<std::size_t>(member)].transform[3].x / 32.f) == cell;
        }
    }
    // Grouped members plus exact per-placement fallback partition ALL inputs.
    const std::set<int> expected{0,1,2,3,4,5,6,7,8};
    ok &= grouped == expected;
    std::size_t fallback = 0;
    for (std::size_t i = 0; i < instances.size(); ++i)
        if (!grouped.contains(static_cast<int>(i))) ++fallback;
    ok &= grouped.size() + fallback == instances.size();
    if (!ok) std::fprintf(stderr, "[instance_grouping] spatial partition/coverage failed\n");
    return ok;
}

heaven::testing::Register instance_bounds_movement_registration(instance_bounds_movement_test);
heaven::testing::Register instance_bounds_invalid_registration(instance_bounds_invalid_test);
heaven::testing::Register instance_spatial_grouping_registration(instance_spatial_grouping_test);
} // namespace
