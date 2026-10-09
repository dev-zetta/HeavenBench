/*
 * HeavenBench — Bridge chain system using Jolt Physics
 */
#include "engine/rope_sim.h"
#include "engine/jolt_common.h"

// Jolt includes
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>

#include <glm/gtc/quaternion.hpp>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <utility>

JPH_SUPPRESS_WARNINGS

using namespace JPH;
using namespace JPH::literals;

namespace heaven {

// ── Jolt boilerplate (anonymous namespace) ──────────────────────

namespace {

namespace Layers {
    static constexpr ObjectLayer NON_MOVING = 0;
    static constexpr ObjectLayer MOVING = 1;
}

namespace BPLayers {
    static constexpr BroadPhaseLayer NON_MOVING(0);
    static constexpr BroadPhaseLayer MOVING(1);
}

class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
public:
    uint GetNumBroadPhaseLayers() const override { return 2; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override {
        return inLayer == Layers::NON_MOVING ? BPLayers::NON_MOVING : BPLayers::MOVING;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
        return inLayer == BPLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjVsBPLayerFilterImpl final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide([[maybe_unused]] ObjectLayer layer,
                       [[maybe_unused]] BroadPhaseLayer broad_phase_layer) const override { return false; }
};

class ObjLayerPairFilterImpl final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide([[maybe_unused]] ObjectLayer layer1,
                       [[maybe_unused]] ObjectLayer layer2) const override { return false; }
};

static RVec3 to_jph(glm::vec3 v) { return {v.x, v.y, v.z}; }

static glm::mat4 jph_to_glm(RMat44 m) {
    glm::mat4 r;
    auto c0 = m.GetColumn4(0); r[0] = glm::vec4(c0.GetX(), c0.GetY(), c0.GetZ(), c0.GetW());
    auto c1 = m.GetColumn4(1); r[1] = glm::vec4(c1.GetX(), c1.GetY(), c1.GetZ(), c1.GetW());
    auto c2 = m.GetColumn4(2); r[2] = glm::vec4(c2.GetX(), c2.GetY(), c2.GetZ(), c2.GetW());
    auto c3 = m.GetColumn4(3); r[3] = glm::vec4(c3.GetX(), c3.GetY(), c3.GetZ(), c3.GetW());
    return r;
}

static Quat glm_to_jph_quat(const glm::mat4& m) {
    glm::quat q = glm::quat_cast(m);
    return {q.x, q.y, q.z, q.w};
}

} // anonymous namespace

// ── Jolt state ──────────────────────────────────────────────────

struct BridgeSystem::JoltState {
    std::unique_ptr<TempAllocatorImpl> temp_allocator;
    std::unique_ptr<JobSystemThreadPool> job_system;
    std::unique_ptr<PhysicsSystem> physics_system;
    BPLayerInterfaceImpl bp_layer_interface;
    ObjVsBPLayerFilterImpl obj_vs_bp_filter;
    ObjLayerPairFilterImpl obj_layer_pair_filter;
    bool initialized = false;
};

// ── BridgeSystem lifecycle ──────────────────────────────────────

BridgeSystem::BridgeSystem() : jolt(std::make_unique<JoltState>()) {}
BridgeSystem::~BridgeSystem() {
    if (jolt && jolt->initialized && jolt->physics_system) {
        auto& bi = jolt->physics_system->GetBodyInterface();
        for (auto& chain : chains) {
            for (auto id : chain.body_ids) {
                bi.RemoveBody(BodyID(id));
                bi.DestroyBody(BodyID(id));
            }
            for (uint32_t id : {chain.start_anchor_id, chain.end_anchor_id}) {
                BodyID body_id(id);
                if (!body_id.IsInvalid()) {
                    bi.RemoveBody(body_id);
                    bi.DestroyBody(body_id);
                }
            }
        }
    }
}
BridgeSystem::BridgeSystem(BridgeSystem&&) noexcept = default;
BridgeSystem& BridgeSystem::operator=(BridgeSystem&&) noexcept = default;

// ── Physics initialization ──────────────────────────────────────

void BridgeSystem::init_physics() {
    if (chains.empty()) return;

    ensure_jolt_registered();

    jolt->temp_allocator = std::make_unique<TempAllocatorImpl>(4 * 1024 * 1024);
    jolt->job_system = std::make_unique<JobSystemThreadPool>(cMaxPhysicsJobs, cMaxPhysicsBarriers, 1);

    jolt->physics_system = std::make_unique<PhysicsSystem>();
    jolt->physics_system->Init(1024, 0, 1024, 256,
        jolt->bp_layer_interface, jolt->obj_vs_bp_filter, jolt->obj_layer_pair_filter);
    jolt->physics_system->SetGravity(Vec3(0, 0, -2.5f));

    auto& bi = jolt->physics_system->GetBodyInterface();

    // Bridge segment physics shape — half extents roughly matching the mesh
    // Mesh local: X=2.39 (chain dir), Y=6.38 (lateral), Z=3.75 (up)
    // Use half of those as half-extents
    RefConst<Shape> segment_shape = new BoxShape(Vec3(1.2f, 3.2f, 1.8f));
    // Endpoint anchors are invisible and collisions are disabled for this
    // physics world, so only a small valid shape is needed.
    RefConst<Shape> anchor_shape = new BoxShape(Vec3::sReplicate(0.05f));

    for (size_t ci = 0; ci < chains.size(); ci++) {
        auto& chain = chains[ci];
        auto& anchor = anchors[ci];
        int n = static_cast<int>(chain.segment_base_transforms.size());
        if (n == 0) continue;

        // The authored start/end dummies are the kinematic island attachment
        // points. Keep those as separate invisible bodies so the first and
        // last visible planks remain dynamic and can rotate naturally as an
        // island bobs. Making a visible endpoint plank kinematic creates a hard
        // orientation discontinuity at the final joint.
        auto create_anchor = [&](glm::vec3 position) {
            BodyCreationSettings settings(
                anchor_shape, to_jph(position), Quat::sIdentity(),
                EMotionType::Kinematic, Layers::MOVING);
            return bi.CreateAndAddBody(settings, EActivation::Activate);
        };
        const BodyID start_anchor = create_anchor(anchor.start_base);
        const BodyID end_anchor = create_anchor(anchor.end_base);
        chain.start_anchor_id = start_anchor.GetIndexAndSequenceNumber();
        chain.end_anchor_id = end_anchor.GetIndexAndSequenceNumber();

        // Every visible bridge segment is dynamic.
        chain.body_ids.resize(n);
        for (int i = 0; i < n; i++) {
            const glm::mat4& base = chain.segment_base_transforms[i];
            glm::vec3 pos = glm::vec3(base[3]);
            Quat rot = glm_to_jph_quat(base);

            BodyCreationSettings s(
                segment_shape, to_jph(pos), rot,
                EMotionType::Dynamic, Layers::MOVING);
            // Reduce gravity at the attachments so the transition from the
            // moving island into the freely sagging span stays smooth.
            float dist_from_end = std::min(
                static_cast<float>(i),
                static_cast<float>(n - 1 - i));
            float gravity_ramp = std::min(1.0f, dist_from_end / 3.0f);
            s.mLinearDamping = 0.5f;
            s.mAngularDamping = 0.8f;
            s.mGravityFactor = gravity_ramp;

            BodyID id = bi.CreateAndAddBody(s, EActivation::Activate);
            chain.body_ids[i] = id.GetIndexAndSequenceNumber();
        }

        // PointConstraints between consecutive segments
        auto add_constraint = [&](BodyID id1, BodyID id2, glm::vec3 world_point) {
            PointConstraintSettings pcs;
            pcs.mSpace = EConstraintSpace::WorldSpace;
            pcs.mPoint1 = to_jph(world_point);
            pcs.mPoint2 = to_jph(world_point);

            BodyLockRead lock1(jolt->physics_system->GetBodyLockInterface(), id1);
            BodyLockRead lock2(jolt->physics_system->GetBodyLockInterface(), id2);
            if (lock1.Succeeded() && lock2.Succeeded()) {
                auto* constraint = pcs.Create(const_cast<Body&>(lock1.GetBody()), const_cast<Body&>(lock2.GetBody()));
                jolt->physics_system->AddConstraint(constraint);
            }
        };

        // Connect consecutive segments with TWO PointConstraints per joint.
        // Each bridge piece has two rope attachment points on each end (left and right
        // sides of the plank). Using two constraints prevents rotation around the chain axis.
        for (int i = 0; i + 1 < n; i++) {
            glm::vec3 pi = glm::vec3(chain.segment_base_transforms[i][3]);
            glm::vec3 pj = glm::vec3(chain.segment_base_transforms[i + 1][3]);
            glm::vec3 mid = (pi + pj) * 0.5f;

            // Get the lateral (Y) axis from the base transform to offset constraint points
            glm::vec3 y_axis = glm::normalize(glm::vec3(chain.segment_base_transforms[i][1]));
            float lateral_offset = 1.5f; // half the plank width (railing positions)

            // Left side constraint
            add_constraint(BodyID(chain.body_ids[i]), BodyID(chain.body_ids[i + 1]),
                            mid + y_axis * lateral_offset);
            // Right side constraint
            add_constraint(BodyID(chain.body_ids[i]), BodyID(chain.body_ids[i + 1]),
                            mid - y_axis * lateral_offset);
        }

        // Attach the dynamic endpoint planks to the actual authored dummies,
        // using the same two-point lateral constraint as the internal joints.
        const float lateral_offset = 1.5f;
        const glm::vec3 start_y = glm::normalize(
            glm::vec3(chain.segment_base_transforms.front()[1]));
        const glm::vec3 end_y = glm::normalize(
            glm::vec3(chain.segment_base_transforms.back()[1]));
        add_constraint(start_anchor, BodyID(chain.body_ids.front()),
                       anchor.start_base + start_y * lateral_offset);
        add_constraint(start_anchor, BodyID(chain.body_ids.front()),
                       anchor.start_base - start_y * lateral_offset);
        add_constraint(BodyID(chain.body_ids.back()), end_anchor,
                       anchor.end_base + end_y * lateral_offset);
        add_constraint(BodyID(chain.body_ids.back()), end_anchor,
                       anchor.end_base - end_y * lateral_offset);

        std::printf("    Jolt chain %zu: %d dynamic planks, 2 anchors, "
                    "%d point constraints\n",
                    ci, n, 2 * (n + 1));
    }

    // Settle gently: run a few small steps to let constraints stabilize
    // without bodies falling far
    for (int i = 0; i < 30; i++)
        jolt->physics_system->Update(1.0f / 120.0f, 1, jolt->temp_allocator.get(), jolt->job_system.get());

    jolt->initialized = true;
    std::printf("  Jolt Physics initialized for %zu bridge chains\n", chains.size());
}

// ── Per-frame update ────────────────────────────────────────────

bool BridgeSystem::update(float dt, const std::map<int, float>& group_offsets) {
    if (!jolt || !jolt->initialized) return false;

    constexpr float kFixedStep = 1.0f / 120.0f;
    constexpr int kMaxCatchUpSteps = 4;
    fixed_step_accumulator =
        std::min(fixed_step_accumulator + std::max(dt, 0.0f),
                 kFixedStep * static_cast<float>(kMaxCatchUpSteps));

    int steps = 0;
    while (fixed_step_accumulator >= kFixedStep &&
           steps < kMaxCatchUpSteps) {
        fixed_step_accumulator -= kFixedStep;
        ++steps;

        auto& bi = jolt->physics_system->GetBodyInterface();

        for (size_t ci = 0; ci < chains.size(); ci++) {
            auto& chain = chains[ci];
            auto& a = anchors[ci];
            if (chain.segment_base_transforms.empty()) continue;

            // Compute the Z offsets for each endpoint.
            float start_z = 0.0f;
            float end_z = 0.0f;
            if (a.start_group >= 0) {
                auto it = group_offsets.find(a.start_group);
                if (it != group_offsets.end()) start_z = it->second;
            }
            if (a.end_group >= 0) {
                auto it = group_offsets.find(a.end_group);
                if (it != group_offsets.end()) end_z = it->second;
            }

            // Move the invisible authored anchors. Visible endpoint planks are
            // dynamic and follow through their constraints.
            int n = static_cast<int>(chain.body_ids.size());

            {
                glm::vec3 pos = a.start_base;
                pos.z += start_z;
                bi.MoveKinematic(
                    BodyID(chain.start_anchor_id), to_jph(pos),
                    Quat::sIdentity(), kFixedStep);
            }
            {
                glm::vec3 pos = a.end_base;
                pos.z += end_z;
                bi.MoveKinematic(
                    BodyID(chain.end_anchor_id), to_jph(pos),
                    Quat::sIdentity(), kFixedStep);
            }

            for (int i = 0; i < n; i++)
                bi.ActivateBody(BodyID(chain.body_ids[i]));
        }

        jolt->physics_system->Update(
            kFixedStep, 1, jolt->temp_allocator.get(), jolt->job_system.get());
    }
    return steps > 0;
}

// ── Read back transforms ────────────────────────────────────────

void BridgeSystem::apply_to_instances(std::vector<Instance>& instances,
                                      std::vector<int>* dirty_instance_indices) {
    if (!jolt || !jolt->initialized) return;

    auto& bi = jolt->physics_system->GetBodyInterface();

    for (auto& chain : chains) {
        for (size_t i = 0; i < chain.body_ids.size(); i++) {
            int inst_idx = chain.segment_instance_indices[i];
            if (inst_idx < 0 || std::cmp_greater_equal(inst_idx, instances.size()))
                continue;

            RMat44 t = bi.GetWorldTransform(BodyID(chain.body_ids[i]));
            instances[inst_idx].transform = jph_to_glm(t);
            if (dirty_instance_indices) {
                dirty_instance_indices->push_back(inst_idx);
            }
        }
    }
}

} // namespace heaven
