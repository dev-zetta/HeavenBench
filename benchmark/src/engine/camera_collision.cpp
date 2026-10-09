/*
 * HeavenBench — Camera collision implementation
 *
 * Sphere collide-and-slide against a static Jolt collision world.
 */
#include "engine/camera_collision.h"
#include "engine/jolt_common.h"

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Geometry/IndexedTriangle.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <utility>

JPH_SUPPRESS_WARNINGS

using namespace JPH;

namespace heaven {

namespace {

// Single broadphase layer — every collider is static world geometry.
namespace CCLayers {
    static constexpr ObjectLayer WORLD = 0;
}
namespace CCBPLayers {
    static constexpr BroadPhaseLayer WORLD(0);
    static constexpr uint NUM = 1;
}

class CCBroadPhaseLayerInterface final : public BroadPhaseLayerInterface {
public:
    uint GetNumBroadPhaseLayers() const override { return CCBPLayers::NUM; }
    BroadPhaseLayer GetBroadPhaseLayer([[maybe_unused]] ObjectLayer layer) const override {
        return CCBPLayers::WORLD;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName([[maybe_unused]] BroadPhaseLayer layer) const override {
        return "WORLD";
    }
#endif
};

// The camera query collides with the single world layer.
class CCObjVsBPFilter final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide([[maybe_unused]] ObjectLayer layer,
                       [[maybe_unused]] BroadPhaseLayer broad_phase_layer) const override { return true; }
};
class CCObjLayerPairFilter final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide([[maybe_unused]] ObjectLayer layer1,
                       [[maybe_unused]] ObjectLayer layer2) const override { return true; }
};

static RVec3 to_jph(const glm::vec3& v) { return {v.x, v.y, v.z}; }
static glm::vec3 to_glm(RVec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
static Quat glm_to_jph_quat(const glm::mat4& m) {
    glm::quat q = glm::normalize(glm::quat_cast(m));
    return {q.x, q.y, q.z, q.w};
}

} // anonymous namespace

// ── State ───────────────────────────────────────────────────────

struct CameraCollision::State {
    std::unique_ptr<TempAllocatorImpl> temp_allocator;
    std::unique_ptr<PhysicsSystem>     physics_system;
    CCBroadPhaseLayerInterface         bp_layer_interface;
    CCObjVsBPFilter                    obj_vs_bp_filter;
    CCObjLayerPairFilter               obj_layer_pair_filter;

    // One shared (uncuated) MeshShape per unique mesh index. Bodies
    // reference these via a per-instance ScaledShape so we never
    // duplicate triangle data.
    std::unordered_map<int, RefConst<Shape>> mesh_shapes;

    // Map instance index → body id so update_moving can reposition
    // only the instances that actually moved.
    std::unordered_map<int, BodyID> instance_bodies;

    bool initialized = false;

    void clear_world() {
        // Retire bodies while their owning world still exists. Mesh indices and
        // instance indices belong to one scene and cannot survive a rebuild.
        if (physics_system) {
            auto& bi = physics_system->GetBodyInterface();
            for (const auto& [idx, id] : instance_bodies) {
                bi.RemoveBody(id);
                bi.DestroyBody(id);
            }
        }
        instance_bodies.clear();
        mesh_shapes.clear();
        initialized = false;
        physics_system.reset();
        temp_allocator.reset();
    }

    ~State() { clear_world(); }
};

CameraCollision::CameraCollision() : state_(std::make_unique<State>()) {}
CameraCollision::~CameraCollision() = default;
CameraCollision::CameraCollision(CameraCollision&&) noexcept = default;
CameraCollision& CameraCollision::operator=(CameraCollision&&) noexcept = default;

size_t CameraCollision::body_count() const {
    return state_ ? state_->instance_bodies.size() : 0;
}

// ── Build the static collision world ────────────────────────────

bool CameraCollision::build(std::span<const Mesh> meshes,
                            std::span<const Instance> instances) {
    ensure_jolt_registered();

    auto& st = *state_;
    st.clear_world();
    st.temp_allocator = std::make_unique<TempAllocatorImpl>(16 * 1024 * 1024);
    st.physics_system = std::make_unique<PhysicsSystem>();
    // No moving bodies and no constraints — this is a pure static
    // collision world queried via the narrow phase. Generous body
    // cap so the whole Heaven scene (~300 instances) fits.
    st.physics_system->Init(/*maxBodies*/ 4096, /*numBodyMutexes*/ 0,
                            /*maxBodyPairs*/ 4096, /*maxContactConstraints*/ 1024,
                            st.bp_layer_interface, st.obj_vs_bp_filter,
                            st.obj_layer_pair_filter);

    auto& bi = st.physics_system->GetBodyInterface();

    // Build one MeshShape per unique mesh index, lazily. Each mesh's
    // surfaces are concatenated into a single triangle soup in the
    // mesh's local space; the per-instance world transform is applied
    // by the body, so the shape is shared across instances.
    auto build_mesh_shape = [&](int mesh_index) -> RefConst<Shape> {
        if (auto it = st.mesh_shapes.find(mesh_index); it != st.mesh_shapes.end())
            return it->second;
        if (mesh_index < 0 || std::cmp_greater_equal(mesh_index, meshes.size()))
            return {};

        const Mesh& mesh = meshes[mesh_index];

        VertexList   verts;
        IndexedTriangleList tris;
        for (const auto& surf : mesh.surfaces) {
            if (surf.indices.empty() || surf.vertices.empty()) continue;
            const uint32_t base = static_cast<uint32_t>(verts.size());
            verts.reserve(verts.size() + surf.vertices.size());
            for (const auto& v : surf.vertices)
                verts.push_back(Float3(v.px, v.py, v.pz));
            for (size_t i = 0; i + 2 < surf.indices.size(); i += 3) {
                uint32_t a = base + surf.indices[i + 0];
                uint32_t b = base + surf.indices[i + 1];
                uint32_t c = base + surf.indices[i + 2];
                tris.push_back(IndexedTriangle(a, b, c, 0));
            }
        }
        if (tris.empty()) return {};

        MeshShapeSettings settings(std::move(verts), std::move(tris));
        settings.SetEmbedded();
        Shape::ShapeResult result = settings.Create();
        if (result.HasError()) {
            std::fprintf(stderr, "CameraCollision: mesh %d shape error: %s\n",
                         mesh_index, result.GetError().c_str());
            return {};
        }
        RefConst<Shape> shape = result.Get();
        st.mesh_shapes.emplace(mesh_index, shape);
        return shape;
    };

    size_t created = 0;
    for (int idx = 0; std::cmp_less(idx, instances.size()); ++idx) {
        const Instance& inst = instances[idx];
        RefConst<Shape> base_shape = build_mesh_shape(inst.mesh_index);
        if (!base_shape) continue;

        // Decompose the instance transform into translation, rotation
        // and (possibly non-uniform) scale. MeshShape can't be scaled
        // at body level directly, so wrap it in a ScaledShape.
        const glm::mat4& m = inst.transform;
        glm::vec3 scale{
            glm::length(glm::vec3(m[0])),
            glm::length(glm::vec3(m[1])),
            glm::length(glm::vec3(m[2]))};
        // Guard against degenerate scale.
        if (scale.x < 1e-5f || scale.y < 1e-5f || scale.z < 1e-5f) continue;

        RefConst<Shape> shape = base_shape;
        if (std::abs(scale.x - 1.0f) > 1e-4f ||
            std::abs(scale.y - 1.0f) > 1e-4f ||
            std::abs(scale.z - 1.0f) > 1e-4f) {
            ScaledShapeSettings ss(base_shape, Vec3(scale.x, scale.y, scale.z));
            ss.SetEmbedded();
            Shape::ShapeResult sr = ss.Create();
            if (sr.HasError()) continue;
            shape = sr.Get();
        }

        // Rotation-only matrix (strip scale) for the body orientation.
        glm::mat4 rot = m;
        rot[0] = glm::vec4(glm::vec3(m[0]) / scale.x, 0.0f);
        rot[1] = glm::vec4(glm::vec3(m[1]) / scale.y, 0.0f);
        rot[2] = glm::vec4(glm::vec3(m[2]) / scale.z, 0.0f);
        rot[3] = glm::vec4(0, 0, 0, 1);

        glm::vec3 pos = glm::vec3(m[3]);

        BodyCreationSettings bcs(shape, to_jph(pos), glm_to_jph_quat(rot),
                                 EMotionType::Static, CCLayers::WORLD);
        const Body* body = bi.CreateBody(bcs);
        if (!body) continue;
        bi.AddBody(body->GetID(), EActivation::DontActivate);
        st.instance_bodies.emplace(idx, body->GetID());
        ++created;
    }

    // Build the broadphase acceleration structure in one shot now
    // that all static bodies are added — far cheaper than letting it
    // happen incrementally.
    st.physics_system->OptimizeBroadPhase();

    st.initialized = (created > 0);
    std::printf("  Camera collision: %zu static bodies, %zu unique mesh shapes\n",
                created, st.mesh_shapes.size());
    return st.initialized;
}

// ── Per-frame transform refresh for moving instances ────────────

void CameraCollision::update_moving(std::span<const Instance> instances,
                                    std::span<const int> moving_indices,
                                    const glm::vec3& camera_pos,
                                    float active_radius) {
    if (!state_ || !state_->initialized) return;
    if (moving_indices.empty()) return;

    const float r2 = active_radius * active_radius;
    auto& bi = state_->physics_system->GetBodyInterface();
    for (int idx : moving_indices) {
        auto it = state_->instance_bodies.find(idx);
        if (it == state_->instance_bodies.end()) continue;
        if (idx < 0 || std::cmp_greater_equal(idx, instances.size())) continue;

        const glm::mat4& m = instances[idx].transform;
        glm::vec3 pos = glm::vec3(m[3]);

        // Skip bodies the camera can't reach this frame. A floating
        // island 200m away bobbing 2m doesn't need its collision
        // body resynced — the camera will never touch it. This keeps
        // the per-frame cost proportional to nearby geometry rather
        // than the whole world (168 islands + stones).
        glm::vec3 d = pos - camera_pos;
        if (glm::dot(d, d) > r2) continue;

        glm::vec3 scale{
            glm::length(glm::vec3(m[0])),
            glm::length(glm::vec3(m[1])),
            glm::length(glm::vec3(m[2]))};
        glm::mat4 rot = m;
        if (scale.x > 1e-5f && scale.y > 1e-5f && scale.z > 1e-5f) {
            rot[0] = glm::vec4(glm::vec3(m[0]) / scale.x, 0.0f);
            rot[1] = glm::vec4(glm::vec3(m[1]) / scale.y, 0.0f);
            rot[2] = glm::vec4(glm::vec3(m[2]) / scale.z, 0.0f);
        }
        rot[3] = glm::vec4(0, 0, 0, 1);

        // Static bodies are repositioned without activation. The
        // broadphase is updated so subsequent casts see the new pose.
        bi.SetPositionAndRotation(it->second, to_jph(pos), glm_to_jph_quat(rot),
                                  EActivation::DontActivate);
    }
    // NOTE: we deliberately do NOT call OptimizeBroadPhase() here.
    // SetPositionAndRotation already calls NotifyBodiesAABBChanged,
    // which updates the broadphase quadtree incrementally — that's
    // the intended path for moving bodies and is effectively free.
    // OptimizeBroadPhase() rebuilds the *entire* tree from scratch
    // (all 300+ bodies) and was costing ~6K FPS when called every
    // frame. It's only needed once, after the initial bulk insert
    // (done in build()).
}

// ── Collide-and-slide resolve ───────────────────────────────────

// Push a sphere centred at `pos` out of any geometry it currently
// overlaps. Returns the corrected centre.

// Each iteration we query all current overlaps and resolve the
// SINGLE deepest one (push the sphere out along that contact's
// normal by its penetration depth plus a small skin), then
// re-query. Resolving the deepest contact first and re-querying is
// the standard, stable depenetration scheme — it converges in a few
// iterations even in concave corners, whereas summing/maxing all
// contacts at once tends to over- or under-correct and oscillate.

// This is the key fix for the camera getting permanently stuck: a
// sphere-cast that starts inside geometry returns fraction 0 and
// can't move, so we must guarantee the sphere is outside everything
// before sweeping.
namespace {

glm::vec3 depenetrate_sphere(const NarrowPhaseQuery& npq,
                                    const SphereShape& sphere,
                                    glm::vec3 pos) {
    const float kSkin = 0.01f;
    for (int iter = 0; iter < 8; ++iter) {
        CollideShapeSettings settings;
        settings.mBackFaceMode = EBackFaceMode::CollideWithBackFaces;
        settings.mActiveEdgeMode = EActiveEdgeMode::CollideWithAll;
        settings.mMaxSeparationDistance = 0.0f;   // only real overlaps

        AllHitCollisionCollector<CollideShapeCollector> collector;
        npq.CollideShape(&sphere, Vec3::sReplicate(1.0f),
                         RMat44::sTranslation(to_jph(pos)),
                         settings, RVec3::sZero(), collector);
        if (collector.mHits.empty()) break;

        // Find the deepest penetration.
        const CollideShapeResult* deepest = nullptr;
        for (const auto& h : collector.mHits) {
            if (h.mPenetrationDepth <= 0.0f) continue;
            if (!deepest || h.mPenetrationDepth > deepest->mPenetrationDepth)
                deepest = &h;
        }
        if (!deepest) break;

        glm::vec3 n = to_glm(deepest->mPenetrationAxis);
        float nlen = glm::length(n);
        if (nlen < 1e-6f) break;
        n /= nlen;
        // mPenetrationAxis moves shape 2 (world) out of the sphere;
        // to move the sphere out we go the opposite way.
        pos -= n * (deepest->mPenetrationDepth + kSkin);
    }
    return pos;
}

} // namespace

glm::vec3 CameraCollision::resolve(const glm::vec3& position,
                                   const glm::vec3& desired) const {
    if (!enabled_ || !state_ || !state_->initialized)
        return desired;

    const NarrowPhaseQuery& npq = state_->physics_system->GetNarrowPhaseQuery();

    SphereShape sphere(radius_);
    sphere.SetEmbedded();

    // ── Step 1: depenetrate ────────────────────────────────────
    // If the previous frame left the camera touching/inside a
    // surface (numerical drift, a moving island catching up to it,
    // teleport, etc.) push it back out first. Without this a
    // sphere-cast from inside geometry returns fraction 0 and the
    // camera locks up permanently.
    glm::vec3 current = depenetrate_sphere(npq, sphere, position);

    glm::vec3 remaining = desired - current;

    // ── Step 2: collide-and-slide ──────────────────────────────
    // Up to 4 iterations handles walls, corners and wedges.
    for (int iter = 0; iter < 4; ++iter) {
        float dist = glm::length(remaining);
        if (dist < 1e-5f) break;

        RShapeCast shape_cast(
            &sphere, Vec3::sReplicate(1.0f),
            RMat44::sTranslation(to_jph(current)),
            Vec3(remaining.x, remaining.y, remaining.z));

        ShapeCastSettings settings;
        settings.mBackFaceModeTriangles = EBackFaceMode::CollideWithBackFaces;
        settings.mReturnDeepestPoint = false;

        ClosestHitCollisionCollector<CastShapeCollector> collector;
        npq.CastShape(shape_cast, settings, RVec3::sZero(), collector);

        if (!collector.HadHit()) {
            // Free path — move the whole way.
            current += remaining;
            break;
        }

        // Move up to just before the hit, leaving a small skin so the
        // next cast doesn't start already touching the surface.
        const float kSkin = 0.02f;
        glm::vec3 dir = remaining / dist;
        float travel = collector.mHit.mFraction * dist;
        float allowed = std::max(0.0f, travel - kSkin);
        current += dir * allowed;

        // Slide: project the *remaining* motion onto the contact
        // plane so the camera glides along the surface.
        glm::vec3 n = to_glm(-collector.mHit.mPenetrationAxis);
        float nlen = glm::length(n);
        if (nlen < 1e-6f) break;
        n /= nlen;

        glm::vec3 leftover = remaining * (1.0f - collector.mHit.mFraction);
        leftover -= n * glm::dot(leftover, n);

        // If sliding produced essentially no motion we're in a corner
        // — stop rather than spin through more iterations.
        if (glm::dot(leftover, leftover) < 1e-8f) break;
        remaining = leftover;
    }

    // ── Step 3: final safety depenetrate ───────────────────────
    // The slide can leave the sphere a hair inside a second surface
    // it slid into; clean that up so next frame starts free.
    current = depenetrate_sphere(npq, sphere, current);

    return current;
}

} // namespace heaven
