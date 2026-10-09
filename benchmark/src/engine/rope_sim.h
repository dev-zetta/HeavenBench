/*
 * HeavenBench — Bridge chain system using Jolt Physics
 *
 * Each bridge segment is a Jolt rigid body (box shape) connected to
 * its neighbors by PointConstraints. Endpoints are kinematic bodies
 * that track the floating island positions.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_ROPE_SIM_H_
#define HEAVENBENCH_SRC_ENGINE_ROPE_SIM_H_

#include "engine/types.h"
#include <glm/glm.hpp>
#include <vector>
#include <map>
#include <memory>

// Forward declarations for Jolt types (avoid including heavy headers here)
namespace JPH {
    class PhysicsSystem;
    class TempAllocator;
    class BodyID;
}

namespace heaven {

//Per-chain data: instance indices, base transforms, and Jolt body IDs
struct BridgeChain {
    std::vector<int> segment_instance_indices;
    std::vector<glm::mat4> segment_base_transforms;
    std::vector<uint32_t> body_ids;       // Jolt BodyID raw values for segments
    uint32_t start_anchor_id = 0;         // Invisible kinematic Jolt anchor
    uint32_t end_anchor_id = 0;           // Invisible kinematic Jolt anchor
};

//Bridge system using Jolt Physics for rigid body chain simulation
struct BridgeSystem {
    struct ChainAnchors {
        glm::vec3 start_base;
        glm::vec3 end_base;
        int start_group;  // -1 = static, 0+ = floating group index
        int end_group;
    };

    std::vector<BridgeChain> chains;
    std::vector<ChainAnchors> anchors;

    // Jolt Physics internals
    struct JoltState;
    std::unique_ptr<JoltState> jolt;

    BridgeSystem();
    ~BridgeSystem();
    BridgeSystem(BridgeSystem&&) noexcept;
    BridgeSystem& operator=(BridgeSystem&&) noexcept;

    //Initialize the Jolt physics world. Call once after all chains are set up.
    void init_physics();

    //Advance a bounded 120 Hz fixed-step simulation. Returns true only when
    //at least one physics step ran, allowing callers to skip transform
    //readback and GPU uploads on faster render frames.
    [[nodiscard]] bool update(float dt, const std::map<int, float>& group_offsets);

    //Read back Jolt body transforms and append changed instance indices.
    void apply_to_instances(std::vector<Instance>& instances,
                            std::vector<int>* dirty_instance_indices = nullptr);

    float fixed_step_accumulator = 0.0f;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_ROPE_SIM_H_
