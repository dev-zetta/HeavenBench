/*
 * HeavenBench — bridge endpoint topology regression.
 *
 * Visible endpoint planks must remain dynamic. Separate invisible kinematic
 * anchors follow the authored endpoint dummies; otherwise the last visible
 * plank retains a fixed rotation while its neighbor follows a moving island,
 * producing the intermittent hard kink seen in the scene.
 */
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/rope_sim.h"

namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

glm::mat4 translated(float x) {
    return glm::translate(glm::mat4(1.0f), glm::vec3{x, 0.0f, 0.0f});
}

bool unit_bridge_endpoint_dynamics() {
    heaven::BridgeSystem bridge;

    heaven::BridgeChain chain;
    chain.segment_instance_indices = {0, 1, 2};
    chain.segment_base_transforms = {
        translated(2.5f), translated(7.5f), translated(12.5f)};
    bridge.chains.push_back(std::move(chain));
    bridge.anchors.push_back({
        .start_base = {0.0f, 0.0f, 0.0f},
        .end_base = {15.0f, 0.0f, 0.0f},
        .start_group = -1,
        .end_group = 0,
    });

    std::vector<heaven::Instance> instances = {
        {-1, translated(2.5f)},
        {-1, translated(7.5f)},
        {-1, translated(12.5f)},
    };

    bridge.init_physics();

    // Raise one endpoint smoothly over two seconds, mirroring a floating
    // island without applying an artificial single-frame impulse.
    constexpr int kSteps = 240;
    for (int i = 1; i <= kSteps; ++i) {
        const float z = 4.0f * static_cast<float>(i) /
                        static_cast<float>(kSteps);
        const bool stepped =
            bridge.update(1.0f / 120.0f, std::map<int, float>{{0, z}});
        if (!stepped) {
            std::printf("[unit_bridge_endpoint_dynamics] FAILED: fixed-step "
                        "update did not advance\n");
            return false;
        }
    }
    bridge.apply_to_instances(instances);

    const glm::mat4& last = instances.back().transform;
    const glm::vec3 last_x_axis = glm::normalize(glm::vec3(last[0]));
    const float endpoint_rotation = std::abs(last_x_axis.z);
    if (endpoint_rotation < 0.05f) {
        std::printf("[unit_bridge_endpoint_dynamics] FAILED: last visible "
                    "plank stayed kinematic (x-axis z=%.6f)\n",
                    endpoint_rotation);
        return false;
    }

    const float final_joint_distance = glm::length(
        glm::vec3(instances[2].transform[3]) -
        glm::vec3(instances[1].transform[3]));
    if (!std::isfinite(final_joint_distance) ||
        final_joint_distance > 6.0f) {
        std::printf("[unit_bridge_endpoint_dynamics] FAILED: final joint "
                    "separated (distance=%.6f)\n", final_joint_distance);
        return false;
    }

    std::printf("[unit_bridge_endpoint_dynamics] passed: endpoint rotated "
                "naturally, final joint distance %.3f\n",
                final_joint_distance);
    return true;
}

const heaven::testing::Register reg_bridge_endpoint_dynamics{
    unit_bridge_endpoint_dynamics};

} // namespace
