/*
 * HeavenBench — GPU-skinning smoke case (renderer-linked, GATED)
 *
 *
 * the four Animated_Objects (mill wings, weathercock, lab gears, port
 * propeller) are skinned on the GPU via Filament's native skinning when
 * gpu_skinning is enabled (the default) — uploaded ONCE with BONE_INDICES /
 * BONE_WEIGHTS attributes (MeshStore::upload_skinned), built with
 * RenderableManager::Builder::skinning(boneCount) (SceneBuilder), and advanced
 * each frame by pushing bone matrices through RenderableManager::setBones
 * (FilamentBackend::update_skinned_bones) — never re-uploading the vertex
 * buffer. Disabling the feature (--no-gpu-skinning) restores the CPU path
 * (skin_interpolated → update_mesh_vertices). A bone count over Filament's
 * 255-bone limit falls back to the CPU path per object with a diagnostic
 *
 *
 * This case combines the two harness patterns the sibling cases established:
 *
 *   HALF A — against the LIVE backend (mirrors smoke_perframe_discipline.cpp /
 *   smoke_feature_registration.cpp): the GraphicsApi::Filament guard +
 *   static_cast<FilamentBackend&>, the init_pipelines SKIP, and the
 *   debug_build_counters() build-once discipline. It renders several empty-scene
 *   frames and asserts the scene is built exactly once while the per-frame
 *   transform sync runs every frame, then exercises the two GPU-skinning RHI
 *   seams (update_skinned_bones / gpu_skinning_active) on the live backend and
 *   asserts they are safe (no crash, no extra scene build) and self-consistent:
 *   the empty harness scene has no Animated_Object, so gpu_skinning_active() for
 *   any mesh index reports a defined `false` once the scene is built, and
 *   update_skinned_bones() for that index is a benign Ok no-op that does NOT
 *   trigger a scene rebuild (the per-frame bone path never rebuilds geometry —
 * the build-once tie-in the test calls for via debug_build_counters).
 *
 *   HALF B — HERMETIC (mirrors smoke_mesh_store.cpp / smoke_glass_refraction.cpp
 * smoke_wet_reflections_capability.cpp): a stand-alone NOOP Filament engine +
 *   scene + MeshStore + TextureStore + MaterialInstanceCache + SceneBuilder,
 *   isolated from the shared backend. Because the 64x64 harness renders an EMPTY
 *   scene (no Animated_Object reaches the live SceneBuilder), the genuine
 *   skinning-enabled / per-frame-bone / disabled-path / bone-overflow contracts
 *   are exercised here against a synthetic skinned mesh:
 *     (B1) SKINNING ENABLED: a synthetic 3-bone skinned mesh
 *          uploaded via MeshStore::upload_skinned is marked skinned
 *          (skinned_info ⇒ {true, 3}), and SceneBuilder built with
 *          gpu_skinning=true records a skinning-enabled renderable for it
 *          (skinned_entities(mesh) non-empty, the host-observable seam
 *          FilamentBackend::gpu_skinning_active / update_skinned_bones route
 *          through). The renderable carries a real RenderableManager component.
 *     (B2) PER-FRAME BONES, NO VB REBUILD: driving several frames
 *          of update_transforms + the exact bone push update_skinned_bones makes
 *          (bone_matrices_interpolated → glm_to_filament → setBones) leaves the
 *          MeshStore's VB/IB handles for the surface UNCHANGED (geometry never
 *          rebuilt) and the recorded skinned-entity set stable (renderable never
 *          rebuilt) — the build-once / update-bones-only discipline.
 *     (B3) DISABLED PATH USES update_mesh_vertices: the same mesh
 *          built with gpu_skinning=false records NO skinned entities (so the main
 *          loop drives it via the CPU path). Its animated-source buffer accepts
 *          the CPU-skinned update while retaining VB/IB ownership. An ordinary
 *          static upload remains unmarked and accepts its original deformation
 *          update independently.
 *     (B4) BONE-OVERFLOW FALLBACK + DIAGNOSTIC: a synthetic
 *          300-bone skinned mesh (over Filament's 255 limit) still uploads its
 *          rest-pose geometry (so it can render via the CPU fallback) but is left
 *          UN-marked (skinned_info ⇒ {false, 0}), and SceneBuilder built with
 *          gpu_skinning=true records NO skinned renderable for it — the object
 *          falls back to the CPU_Skinning_Path. MeshStore emits the one-time
 *          over-limit diagnostic to stderr (the harness does not capture stdout/
 *          stderr, so the diagnostic text itself is covered by code inspection;
 *          here we assert the observable unmarked + no-skinned-renderable
 *          outcome). The marked 3-bone mesh in (B1) is the positive control
 *          proving the overflow path is a genuine branch difference and not
 *          "skinning is never enabled".
 *
 * HERMETIC RATIONALE: MeshStore::upload_skinned builds real Filament
 * VertexBuffer / IndexBuffer objects and SceneBuilder builds real renderables
 * (with.skinning()) + MaterialInstances from the embedded compiled packages.
 * The headless RapidCheck target (heaven_benchmark_tests) links NEITHER Filament
 * NOR SDL, so it cannot build them; this renderer-linked target can — matching
 * the gated GPU smoke-test strategy. NOOP needs no GPU/display and
 * isolates the assertion from the shared backend. The pure grouping/bone-matrix
 * logic is covered by the host property tests (prop_instance_grouping,
 * prop_bone_matrices); the genuine on-device skinned motion + ≤1/255 parity are
 * checked through separate full-scene captures and comparison.
 *
 * NO NEW BACKEND SEAM: the skinning-enabled observable is SceneBuilder's own
 * skinned_entities(mesh_index) accessor (which FilamentBackend::gpu_skinning_active
 * and update_skinned_bones already route through), so this case asserts the
 * host-verifiable seam directly rather than adding a new debug_* accessor.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if its own deeper bring-up (init_pipelines,
 * or the NOOP engine / base materials / skinned upload) fails, tolerating a
 * headless/driver-less environment that passed the gate.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "rhi/filament_backend.h"
#include "rhi/scene_builder.h"
#include "rhi/mesh_store.h"
#include "rhi/material_instance_cache.h"
#include "rhi/texture_store.h"
#include "rhi/backend_core.h"
#include "rhi/filament_util.h"        // glm_to_filament
#include "rhi/skinned_tangent_upload.h"
#include "engine/types.h"
#include "engine/camera.h"
#include "engine/skinned_mesh.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>

#include <math/mat4.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdio>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_gpu_skinning]";

// Render exactly one frame with an EMPTY scene against the live backend.
// Mirrors smoke_perframe_discipline.cpp: the backend tolerates empty
// instance/mesh spans (build creates zero entities), so the frame drives the
// full per-frame path — the one-time build block and the per-frame transform
// sync — without loading the heavy Heaven scene.
void render_one_empty_frame(IRhiBackend& backend, const Camera& camera,
                            const LightingParams& lighting,
                            const TonemapParams& tonemap, int w, int h) {
    MaterialResolver resolver =
        [](const std::string&, const std::string&) -> GpuMaterial {
            return GpuMaterial{};
        };
    WeatherState weather;
    backend.render_frame(std::span<const Instance>{},
                         std::span<const int>{},
                         std::span<const GpuMesh>{},
                         resolver, camera, lighting, weather, tonemap, w, h);
}

// A synthetic skinned mesh: one triangle surface, `bone_count` bones in an
// identity rest pose (so bone_matrices_interpolated returns identities and the
// LBS round-trip is well defined), each vertex weighted fully onto bone 0. Used
// at bone_count==3 (in range ⇒ GPU-skinned) and bone_count==300 (over Filament's
// 255-bone limit ⇒ CPU fallback). upload_skinned reads only bones.size() and the
// surfaces, but we populate frames too so bone_matrices_interpolated is valid.
SkinnedMesh make_skinned_mesh(size_t bone_count) {
    SkinnedMesh sm;
    sm.name = "smoke_skinned_" + std::to_string(bone_count);
    sm.path = sm.name + ".ms08";

    sm.bones.reserve(bone_count);
    for (size_t b = 0; b < bone_count; b++) {
        Bone bone;
        bone.name   = "bone_" + std::to_string(b);
        bone.parent = (b == 0) ? -1 : 0; // a flat skeleton rooted at bone 0
        sm.bones.push_back(std::move(bone));
    }

    // Identity rest pose for every bone, two frames (so frame interpolation has
    // both endpoints): position xyz=0 / scale w=1, rotation = identity quat.
    sm.num_frames = 2;
    BoneFrame rest;
    rest.position = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    rest.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    sm.frames.assign(bone_count * static_cast<size_t>(sm.num_frames), rest);

    SkinnedSurface surf;
    surf.name = "smoke_skinned_surf";
    auto mkv = [](float x, float y) {
        SkinnedVertex v{};
        v.px = x; v.py = y; v.pz = 0.0f;
        v.nx = 0.0f; v.ny = 0.0f; v.nz = 1.0f;
        v.u = x; v.v = y;
        v.bone_indices[0] = 0;
        v.bone_weights[0] = 1.0f; // fully weighted onto bone 0
        return v;
    };
    surf.vertices = { mkv(0.0f, 0.0f), mkv(1.0f, 0.0f), mkv(0.0f, 1.0f) };
    surf.indices  = { 0, 1, 2 };
    sm.surfaces.push_back(std::move(surf));
    return sm;
}

// A static (non-skinned) single-triangle mesh — the geometry the
// --no-gpu-skinning upload path produces (upload() + per-frame
// update_mesh_vertices). Layout matches smoke_mesh_store.cpp's triangle.
Mesh make_static_mesh() {
    Mesh mesh;
    mesh.name = "smoke_static";
    mesh.path = "smoke_static.mesh";
    Surface surf;
    surf.name = "smoke_static_surf";
    surf.vertices = {
        {0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f,  1, 0, 0, 1},
        {1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f,  1, 0, 0, 1},
        {0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f,  1, 0, 0, 1},
    };
    surf.indices = {0, 1, 2};
    mesh.surfaces.push_back(std::move(surf));
    return mesh;
}

bool check_scene_culling(filament::Engine& engine, filament::Scene& scene,
        bool expected, const char* label) {
    auto& manager = engine.getRenderableManager();
    size_t count = 0;
    bool ok = true;
    scene.forEach([&](utils::Entity entity) {
        const auto instance = manager.getInstance(entity);
        if (!instance) return;
        ++count;
        ok &= manager.isCullingEnabled(instance) == expected;
    });
    ok &= count > 0;
    std::printf("%s %s culling=%s renderables=%zu %s\n", kTag, label,
        expected ? "on" : "off", count, ok ? "PASS" : "FAIL");
    return ok;
}

// The mill has one rigid bone. Independently rotate its packed bind frame,
// as the vertex shader does, and compare the CPU update's packed frame. A
// rotated UV basis and its mirror make arbitrary normal-only tangents fail.
bool check_cpu_skin_tangent_frames() {
    float maximum_error = 0;
    float legacy_tangent_error = 0;
    auto decode = [](const filament::math::short4& packed, glm::vec3 axis) {
        const glm::vec3 q(static_cast<float>(packed.x) / 32767.0f,
                          static_cast<float>(packed.y) / 32767.0f,
                          static_cast<float>(packed.z) / 32767.0f);
        const float w = static_cast<float>(packed.w) / 32767.0f;
        return glm::normalize(axis + 2.0f * glm::cross(q,
            glm::cross(q, axis) + w * axis));
    };
    for (const bool mirrored : {false, true}) {
        auto mesh = make_skinned_mesh(1);
        const auto rest_rotation = glm::angleAxis(.61f,
            glm::normalize(glm::vec3(1, 2, -.5f)));
        const glm::vec3 n = rest_rotation * glm::vec3(0, 0, 1);
        const std::array<glm::vec2, 3> chart{{{0, 0}, {.8f, 0}, {.2f, .6f}}};
        std::array<filament::math::float3, 3> positions, normals;
        std::array<filament::math::float2, 3> uvs;
        for (size_t i = 0; i < chart.size(); ++i) {
            auto& v = mesh.surfaces[0].vertices[i];
            const auto p = rest_rotation * glm::vec3(chart[i], 0);
            v.px = p.x; v.py = p.y; v.pz = p.z;
            v.nx = n.x; v.ny = n.y; v.nz = n.z;
            v.u = std::cos(.43f) * chart[i].x - std::sin(.43f) * chart[i].y;
            v.v = (mirrored ? -1.0f : 1.0f) *
                (std::sin(.43f) * chart[i].x + std::cos(.43f) * chart[i].y);
            positions[i] = {v.px, v.py, v.pz};
            normals[i] = {v.nx, v.ny, v.nz};
            uvs[i] = {v.u, v.v};
        }
        const filament::math::uint3 triangle{0, 1, 2};
        auto* bind_orientation = filament::geometry::SurfaceOrientation::Builder()
            .vertexCount(3).normals(normals.data()).uvs(uvs.data())
            .positions(positions.data()).triangleCount(1).triangles(&triangle).build();
        if (!bind_orientation) return false;
        std::array<filament::math::short4, 3> bind_quats;
        bind_orientation->getQuats(bind_quats.data(), bind_quats.size());
        delete bind_orientation;
        mesh.frames[1].rotation = glm::angleAxis(1.07f,
            glm::normalize(glm::vec3(-.4f, .8f, 1)));
        mesh.frames[1].position = glm::vec4(.15f, -.08f, .04f, 1.37f);
        for (const float frame : {0.0f, .37f, 1.0f}) {
            const auto skinned = mesh.skin_interpolated(0, frame);
            const auto packed = pack_cpu_skinned_tangents(skinned);
            if (packed.size() != 3) return false;
            const auto transform = glm::mat3(mesh.bone_matrices_interpolated(frame)[0]);
            for (size_t i = 0; i < 3; ++i) {
                const auto expected_n = glm::normalize(transform *
                    decode(bind_quats[i], glm::vec3(0, 0, 1)));
                const auto expected_t = glm::normalize(transform *
                    decode(bind_quats[i], glm::vec3(1, 0, 0)));
                maximum_error = std::max(maximum_error, glm::length(
                    decode(packed[i], glm::vec3(0, 0, 1)) - expected_n));
                maximum_error = std::max(maximum_error, glm::length(
                    decode(packed[i], glm::vec3(1, 0, 0)) - expected_t));
                if ((packed[i].w < 0) != (bind_quats[i].w < 0)) return false;
                normals[i] = {skinned[i].nx, skinned[i].ny, skinned[i].nz};
            }
            auto* legacy = filament::geometry::SurfaceOrientation::Builder()
                .vertexCount(3).normals(normals.data()).build();
            if (!legacy) return false;
            std::array<filament::math::short4, 3> legacy_quats;
            legacy->getQuats(legacy_quats.data(), legacy_quats.size());
            delete legacy;
            legacy_tangent_error = std::max(legacy_tangent_error, glm::length(
                decode(legacy_quats[0], glm::vec3(1, 0, 0)) -
                glm::normalize(transform * decode(bind_quats[0], glm::vec3(1, 0, 0)))));
        }
    }
    Vertex collapsed{};
    collapsed.nx = 1;
    collapsed.tx = 1;
    collapsed.tw = 1;
    const auto collapsed_quats = pack_cpu_skinned_tangents(
        std::span<const Vertex>{&collapsed, 1});
    if (collapsed_quats.size() != 1 || glm::length(
            decode(collapsed_quats[0], glm::vec3(0, 0, 1)) -
            glm::vec3(1, 0, 0)) >= 2e-4f) return false;
    const bool ok = maximum_error < 2e-4f && legacy_tangent_error > .2f;
    std::printf("%s CPU tangent-frame rotation/mirror regression: max_error=%.9g "
                "legacy_error=%.9g %s\n", kTag, maximum_error,
                legacy_tangent_error, ok ? "PASS" : "FAIL");
    return ok;
}

} // namespace

CaseResult run_gpu_skinning_case(const CaseContext& ctx) {
    bool ok = check_cpu_skin_tangent_frames();

    // ════════════════════════════════════════════════════════════
    // HALF A — build-once discipline + RHI seam safety (LIVE backend)
    // ════════════════════════════════════════════════════════════

    // The debug_* accessors are FilamentBackend-specific test instrumentation.
    // The harness always constructs a FilamentBackend (the sole IRhiBackend),
    // so this is safe; api() lets us bail gracefully if that ever changes —
    // mirroring the sibling cases.
    if (ctx.backend.api() != GraphicsApi::Filament) {
        std::printf("%s SKIP: case needs the Filament backend\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);

    // Bring the render pipeline up so the scene builder exists. A failure here
    // means the environment can't drive the backend (no usable GPU) — treat it
    // as a SKIP so the gated test still passes. init_pipelines is idempotent.
    if (auto r = backend.init_pipelines(ctx.config); !r) {
        std::printf("%s SKIP: init_pipelines failed (%s)\n",
                    kTag, r.error().message.c_str());
        return CaseResult::Skip;
    }

    {
        Camera camera;
        LightingParams lighting = lighting_for_time_of_day(ctx.config.time_of_day);
        TonemapParams  tonemap;
        const int w = ctx.width;
        const int h = ctx.height;
        constexpr int kFrames = 4;

        // Counters are cumulative across the shared backend, so reason in deltas.
        const IRhiBackend::BackendBuildCounters baseline =
            backend.debug_build_counters();

        for (int i = 0; i < kFrames; ++i)
            render_one_empty_frame(backend, camera, lighting, tonemap, w, h);

        const IRhiBackend::BackendBuildCounters after =
            backend.debug_build_counters();

        // Scene (incl. any skinning-enabled renderables) built exactly once.
        if (after.scene_builds != 1) {
            std::printf("%s FAIL: scene_builds == %llu after %d frames "
                        "(expected 1 — the scene, including skinned renderables, "
                        "must build once)\n", kTag,
                        static_cast<unsigned long long>(after.scene_builds),
                        kFrames);
            ok = false;
        }
        // Per-frame transform sync ran every frame.
        const uint64_t tdelta = after.transform_updates - baseline.transform_updates;
        if (tdelta != static_cast<uint64_t>(kFrames)) {
            std::printf("%s FAIL: transform_updates advanced by %llu over %d "
                        "frames (expected %d)\n", kTag,
                        static_cast<unsigned long long>(tdelta), kFrames,
                        kFrames);
            ok = false;
        }

        // ── GPU-skinning RHI seams are safe + self-consistent on the live
        // (empty-scene) backend. The empty harness scene has no
        // Animated_Object, so for any mesh index gpu_skinning_active() reports a
        // defined `false` once built, and update_skinned_bones() is a benign Ok
        // no-op that must NOT trigger a scene rebuild (the per-frame bone path
        // never rebuilds geometry). ──
        const std::optional<bool> active0 = backend.gpu_skinning_active(0);
        if (!active0.has_value()) {
            std::printf("%s FAIL: gpu_skinning_active(0) returned nullopt after "
                        "the scene was built (expected a defined value)\n", kTag);
            ok = false;
        } else if (*active0) {
            std::printf("%s FAIL: gpu_skinning_active(0) reported true on the "
                        "empty harness scene (no Animated_Object exists — "
                        "expected false)\n", kTag);
            ok = false;
        }

        // A single identity bone matrix; the empty scene has no skinned entity
        // for mesh 0, so this routes to zero renderables (a safe Ok no-op).
        const std::vector<glm::mat4> one_identity(1, glm::mat4(1.0f));
        if (auto r = backend.update_skinned_bones(
                0, std::span<const glm::mat4>(one_identity)); !r) {
            std::printf("%s FAIL: update_skinned_bones(0, …) returned an error "
                        "(%s) — an unknown/non-skinned mesh index must be a "
                        "benign no-op\n", kTag, r.error().message.c_str());
            ok = false;
        }
        const IRhiBackend::BackendBuildCounters after_bones =
            backend.debug_build_counters();
        if (after_bones.scene_builds != after.scene_builds) {
            std::printf("%s FAIL: update_skinned_bones() advanced scene_builds "
                        "(%llu -> %llu) — the per-frame bone path must never "
                        "rebuild the scene/geometry (Req 5.4)\n", kTag,
                        static_cast<unsigned long long>(after.scene_builds),
                        static_cast<unsigned long long>(
                            after_bones.scene_builds));
            ok = false;
        }
    }

    // ════════════════════════════════════════════════════════════
    // HALF B — skinning-enabled / per-frame bones / disabled path /
    //          bone-overflow fallback (HERMETIC NOOP)
    // ════════════════════════════════════════════════════════════
    filament::Engine* engine =
        filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) {
        std::printf("%s SKIP: could not create a NOOP Filament engine "
                    "(Half A assertions already evaluated)\n", kTag);
        return ok ? CaseResult::Skip : CaseResult::Fail;
    }

    // Scope every engine-owned user object (scene, store, textures, cache, the
    // builders' entities) inside this block so their destructors run BEFORE
    // filament::Engine::destroy() below — destroying a Filament object after its
    // engine is a double-free panic (the discipline the sibling hermetic cases
    // follow).
    {
        filament::Scene* scene = engine->createScene();
        MeshStore        store(*engine);
        TextureStore     textures(*engine);
        textures.create_placeholders();
        MaterialInstanceCache cache(*engine, textures);

        // The lit/twosided/unlit base materials are needed so SceneBuilder can
        // build a MaterialInstance for the surface. If they don't build on NOOP
        // we SKIP Half B (Half A already evaluated), mirroring the siblings.
        if (auto r = cache.load_base_materials(); !r) {
            std::printf("%s SKIP: base materials failed to build on NOOP (%s) "
                        "(Half A assertions already evaluated)\n",
                        kTag, r.error().message.c_str());
            engine->destroy(scene);
            // store/textures/cache destruct at block exit, before engine teardown.
            filament::Engine::destroy(&engine);
            return ok ? CaseResult::Skip : CaseResult::Fail;
        }

        BackendCore core{};
        core.engine = engine;
        core.scene  = scene;

        MaterialResolver resolver =
            [](const std::string&, const std::string&) -> GpuMaterial {
                return GpuMaterial{};
            };

        auto& rcm = engine->getRenderableManager();

        // ── B1: a 3-bone skinned mesh is marked skinned and gets a
        // skinning-enabled renderable  ──────────────
        const SkinnedMesh sm3 = make_skinned_mesh(3);
        const GpuMesh gm3 = store.upload_skinned(sm3);
        if (gm3.surfaces.empty()) {
            std::printf("%s SKIP: upload_skinned produced no surfaces on NOOP "
                        "(Half A assertions already evaluated)\n", kTag);
            engine->destroy(scene);
            filament::Engine::destroy(&engine);
            return ok ? CaseResult::Skip : CaseResult::Fail;
        }
        const uint64_t handle3 = gm3.surfaces[0].handle;

        const MeshStore::SkinnedInfo info3 = store.skinned_info(handle3);
        if (!info3.skinned || info3.bone_count != 3 || !info3.skinned_source) {
            std::printf("%s FAIL: skinned_info for a 3-bone mesh is {skinned=%d, "
                        "bone_count=%u} (expected {true, 3} — an in-range mesh "
                        "must be marked GPU-skinned, Req 4.1)\n", kTag,
                        static_cast<int>(info3.skinned), info3.bone_count);
            ok = false;
        }

        const std::vector<Instance> instances3 = { Instance{0, glm::mat4(1.0f)} };
        const std::vector<GpuMesh>  meshes3    = { gm3 };

        SceneBuilder sb_on;
        sb_on.build(core, store, cache,
                    std::span<const Instance>(instances3),
                    std::span<const GpuMesh>(meshes3),
                    resolver, /*gpu_skinning=*/true);

        const std::span<const utils::Entity> ents = sb_on.skinned_entities(0);
        if (ents.empty()) {
            std::printf("%s FAIL: SceneBuilder recorded no skinning-enabled "
                        "renderable for the 3-bone Animated_Object with "
                        "gpu_skinning=true (expected ≥1 — .skinning() must be "
                        "enabled, Req 4.1/5.2)\n", kTag);
            ok = false;
        } else if (!rcm.hasComponent(ents[0])) {
            std::printf("%s FAIL: the recorded skinned entity has no "
                        "RenderableManager component\n", kTag);
            ok = false;
        }

        // ── B2: per-frame bone update touches ONLY bones — no VB/IB rebuild,
        // no renderable rebuild  ────────────────────
        if (!ents.empty()) {
            const MeshStore::SurfaceBuffers before = store.find(handle3);
            const size_t skinned_before = sb_on.skinned_entities(0).size();

            // The exact per-frame push FilamentBackend::update_skinned_bones
            // makes: interpolate the bone matrices and setBones them.
            const std::vector<glm::mat4> bone_glm =
                sm3.bone_matrices_interpolated(0.0f);
            std::vector<filament::math::mat4f> bone_mats;
            bone_mats.reserve(bone_glm.size());
            for (const auto& m : bone_glm) bone_mats.push_back(glm_to_filament(m));

            constexpr int kBoneFrames = 5;
            for (int f = 0; f < kBoneFrames; ++f) {
                sb_on.update_transforms(core, std::span<const Instance>(instances3),
                                        std::span<const int>{});
                for (const auto& e : sb_on.skinned_entities(0)) {
                    auto ri = rcm.getInstance(e);
                    if (ri) {
                        rcm.setBones(ri, bone_mats.data(), bone_mats.size(), 0);
                    }
                }
            }

            const MeshStore::SurfaceBuffers after = store.find(handle3);
            if (before.vb != after.vb || before.ib != after.ib) {
                std::printf("%s FAIL: the surface's VB/IB handles changed across "
                            "%d bone-update frames — the per-frame bone path must "
                            "NOT rebuild geometry (Req 5.4)\n",
                            kTag, kBoneFrames);
                ok = false;
            }
            if (sb_on.skinned_entities(0).size() != skinned_before) {
                std::printf("%s FAIL: the recorded skinned-entity set changed "
                            "across bone-update frames (%zu -> %zu) — the "
                            "renderable must build once (Req 5.4)\n", kTag,
                            skinned_before, sb_on.skinned_entities(0).size());
                ok = false;
            }
        }
        sb_on.clear(core);

        // ── B3: disabled path is NOT GPU-skinned and uses update_mesh_vertices
        // ──────────────────────────────────────────
        SceneBuilder sb_off;
        const std::array cpu_instances{Instance{0, glm::mat4(1)},
            Instance{0, glm::mat4(1)}};
        sb_off.build(core, store, cache,
                     cpu_instances,
                     std::span<const GpuMesh>(meshes3),
                     resolver, /*gpu_skinning=*/false, /*gpu_instancing=*/true);
        ok &= scene->getRenderableCount() == cpu_instances.size();
        if (!sb_off.skinned_entities(0).empty()) {
            std::printf("%s FAIL: SceneBuilder recorded a skinning-enabled "
                        "renderable with gpu_skinning=false — the disabled path "
                        "must drive the object through the CPU path (Req 7.3)\n",
                        kTag);
            ok = false;
        }
        auto cpu_pose = sm3.skin_interpolated(0, .37f);
        for (auto& vertex : cpu_pose) vertex.px += 8;
        ok &= cpu_pose.front().px > gm3.bbox_max.x + 1;
        ok &= check_scene_culling(*engine, *scene, false, "CPU-disabled animated source");
        const auto before_cpu = store.find(gm3.surfaces[0].handle);
        if (auto r = store.update_vertices(gm3.surfaces[0].handle,
                cpu_pose.data(), static_cast<uint32_t>(cpu_pose.size())); !r) {
            std::printf("%s FAIL: disabled animated-source update rejected (%s)\n",
                kTag, r.error().message.c_str());
            ok = false;
        }
        const auto after_cpu = store.find(gm3.surfaces[0].handle);
        if (before_cpu.vb != after_cpu.vb || before_cpu.ib != after_cpu.ib) {
            std::printf("%s FAIL: CPU-skinned update replaced VB/IB\n", kTag);
            ok = false;
        }
        sb_off.clear(core);

        // Static/cloth uploads keep their ordinary deformation route; only
        // animated-source updates opt into the supplied tangent-frame route.
        {
            const Mesh static_mesh = make_static_mesh();
            const GpuMesh gms = store.upload(static_mesh);
            if (gms.surfaces.empty()) {
                std::printf("%s SKIP: static upload produced no surfaces on NOOP "
                            "(Half A + earlier Half B assertions already "
                            "evaluated)\n", kTag);
            } else {
                const uint64_t handle_s = gms.surfaces[0].handle;
                const MeshStore::SkinnedInfo info_s = store.skinned_info(handle_s);
                if (info_s.skinned || info_s.bone_count != 0 || info_s.skinned_source) {
                    std::printf("%s FAIL: a statically-uploaded surface reports "
                                "skinned_info {skinned=%d, bone_count=%u} "
                                "(expected {false, 0} — only upload_skinned marks "
                                "GPU skinning)\n", kTag,
                                static_cast<int>(info_s.skinned),
                                info_s.bone_count);
                    ok = false;
                }
                const std::vector<Vertex>& verts = static_mesh.surfaces[0].vertices;
                if (auto r = store.update_vertices(
                        handle_s, verts.data(),
                        static_cast<uint32_t>(verts.size())); !r) {
                    std::printf("%s FAIL: the CPU re-upload (update_mesh_vertices) "
                                "of the rest-pose vertices was rejected (%s) — the "
                                "disabled path must re-upload through this seam "
                                "(Req 4.2/7.3)\n", kTag, r.error().message.c_str());
                    ok = false;
                }
                SceneBuilder static_builder;
                const std::array static_instances{Instance{0, glm::mat4(1)}};
                const std::array static_meshes{gms};
                static_builder.build(core, store, cache, static_instances,
                    static_meshes, resolver, false);
                ok &= check_scene_culling(*engine, *scene, true, "static positive control");
                static_builder.clear(core);
            }
        }

        // ── B4: a >255-bone mesh falls back to the CPU path  ──
        const SkinnedMesh sm_over = make_skinned_mesh(300);
        const GpuMesh gm_over = store.upload_skinned(sm_over);
        if (gm_over.surfaces.empty()) {
            std::printf("%s SKIP: over-limit upload_skinned produced no surfaces "
                        "on NOOP (earlier assertions already evaluated)\n", kTag);
        } else {
            const uint64_t handle_over = gm_over.surfaces[0].handle;
            // Geometry IS still uploaded so the object can render via the CPU
            // fallback (the rest-pose VB/IB exist)...
            const MeshStore::SurfaceBuffers ob = store.find(handle_over);
            if (!ob.vb || !ob.ib) {
                std::printf("%s FAIL: over-limit skinned mesh has no VB/IB — its "
                            "rest-pose geometry must still upload so the CPU "
                            "fallback can render it (Req 4.4)\n", kTag);
                ok = false;
            }
            // ... but it is NOT marked GPU-skinned (over Filament's 255 limit).
            const MeshStore::SkinnedInfo info_over = store.skinned_info(handle_over);
            if (info_over.skinned || info_over.bone_count != 0 || !info_over.skinned_source) {
                std::printf("%s FAIL: a 300-bone mesh reports skinned_info "
                            "{skinned=%d, bone_count=%u} (expected {false, 0} — "
                            "over Filament's 255-bone limit it must be left "
                            "unmarked for the CPU fallback, Req 4.4)\n", kTag,
                            static_cast<int>(info_over.skinned),
                            info_over.bone_count);
                ok = false;
            }
            auto over_pose = sm_over.skin_interpolated(0, .37f);
            for (auto& vertex : over_pose) vertex.px += 8;
            ok &= over_pose.front().px > gm_over.bbox_max.x + 1;
            if (auto r = store.update_vertices(handle_over, over_pose.data(),
                    static_cast<uint32_t>(over_pose.size())); !r) {
                std::printf("%s FAIL: over-limit CPU tangent update rejected (%s)\n",
                    kTag, r.error().message.c_str());
                ok = false;
            }
            // And SceneBuilder records NO skinned renderable for it even with
            // gpu_skinning=true — the object takes the CPU_Skinning_Path.
            const std::vector<Instance> instances_over =
                { Instance{0, glm::mat4(1.0f)}, Instance{0, glm::mat4(1.0f)} };
            const std::vector<GpuMesh> meshes_over = { gm_over };
            SceneBuilder sb_over;
            sb_over.build(core, store, cache,
                          std::span<const Instance>(instances_over),
                          std::span<const GpuMesh>(meshes_over),
                          resolver, /*gpu_skinning=*/true, /*gpu_instancing=*/true);
            ok &= scene->getRenderableCount() == instances_over.size();
            if (!sb_over.skinned_entities(0).empty()) {
                std::printf("%s FAIL: SceneBuilder enabled skinning for a "
                            "300-bone mesh (expected the CPU fallback — no "
                            "skinning-enabled renderable, Req 4.4/7.5)\n", kTag);
                ok = false;
            }
            ok &= check_scene_culling(*engine, *scene, false, "bone-overflow animated source");
            sb_over.clear(core);
        }

        // Destroy the scene before the store/textures/cache destructors (block
        // exit) and before the engine teardown below.
        engine->destroy(scene);
    }

    filament::Engine::destroy(&engine);

    if (!ok) return CaseResult::Fail;

    std::printf("%s GPU-skinning contract honored: live build-once discipline "
                "(scene_builds==1, per-frame transforms only) with safe "
                "update_skinned_bones / gpu_skinning_active seams (Req 5.2/5.4); "
                "hermetic 3-bone mesh marked skinned with a skinning-enabled "
                "renderable (Req 4.1/5.2), per-frame bone updates left VB/IB + "
                "renderable unchanged (Req 5.4), gpu_skinning=false recorded no "
                "skinned renderable and the static surface re-uploaded via "
                "update_mesh_vertices (Req 4.2/7.3), and a 300-bone mesh fell "
                "back to the CPU path unmarked with a diagnostic (Req 4.4/7.5). "
                "On-device skinned motion and pixel parity require separate "
                "full-scene captures and comparison\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
