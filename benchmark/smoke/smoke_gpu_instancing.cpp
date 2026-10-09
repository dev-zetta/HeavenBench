/*
 * HeavenBench — GPU-instancing smoke case (renderer-linked, GATED)
 *
 *
 * repeated static meshes are drawn as Filament instanced renderables when
 * gpu_instancing is enabled (the default). SceneBuilder groups the Instances
 * that share a mesh_index (excluding skinned Animated_Objects and singletons)
 * via compute_instance_groups, splits any group larger than
 * Engine::getMaxAutomaticInstances() into chunks, and builds ONE instanced
 * renderable per non-empty Shadow_Split subset per chunk — built once with
 * RenderableManager::Builder::instances(chunkSize, InstanceBuffer*), then only
 * its per-instance local transforms refreshed each frame. Disabling the feature
 * (--no-gpu-instancing) restores the per-instance path (one renderable per
 * Instance per subset), the byte-parity Shipping_Look fallback.
 *
 * This case combines the two harness patterns the sibling cases established,
 * exactly like its closest sibling smoke_gpu_skinning.cpp:
 *
 *   HALF A — against the LIVE backend (mirrors smoke_perframe_discipline.cpp /
 *   smoke_gpu_skinning.cpp HALF A): the GraphicsApi::Filament guard +
 *   static_cast<FilamentBackend&>, the init_pipelines SKIP, and the
 *   debug_build_counters() build-once discipline. It renders several empty-scene
 *   frames and asserts the scene is built exactly once (scene_builds == 1) while
 *   the per-frame transform sync runs every frame (transform_updates grows one
 *   per frame). This is the backend-level "build once, per-frame only updates"
 *   contract  — for instancing it means the instanced renderables
 *   and InstanceBuffers are never rebuilt after the first build, only their
 *   local transforms are refreshed. The empty harness scene contains no
 *   instanced group (no repeated mesh reaches the live SceneBuilder), so the
 *   instancing-SPECIFIC build/chunk/sync facts are asserted hermetically below.
 *
 *   HALF B — HERMETIC (mirrors smoke_mesh_store.cpp / smoke_gpu_skinning.cpp
 *   HALF B): a stand-alone NOOP Filament engine + scene + MeshStore +
 *   TextureStore + MaterialInstanceCache + SceneBuilder, isolated from the
 *   shared backend. The genuine instanced-build / chunking / disabled-path /
 *   build-once-per-frame contracts are exercised here against a synthetic
 *   repeated static mesh, observed through the Filament Scene's renderable count
 *   (the same scene->getRenderableCount() seam smoke_rain_toggle_roundtrip.cpp /
 *   smoke_ssgi.cpp use to observe scene membership) and the pure
 *   compute_instance_groups helper:
 *
 *     The synthetic mesh has a SINGLE surface with the default
 *     GpuMaterial (casts_shadow=true) and GpuMesh.cast_shadows=true, so it
 *     yields EXACTLY ONE Shadow_Split subset (the caster). Therefore each
 *     instanced chunk realises as exactly ONE instanced renderable, and each
 *     per-instance build realises as exactly ONE renderable — making the
 *     renderable count a clean, non-vacuous witness of the instanced vs
 *     per-instance decision.
 *
 *     (B1) INSTANCED BUILD, NOT N RENDERABLES: N (>1, <= max) Instances
 *          sharing one mesh_index, built with gpu_instancing=true, produce ONE
 *          instanced renderable (the single Shadow_Split subset of one chunk) —
 *          NOT N. The SAME N Instances built with gpu_instancing=false produce N
 *          per-instance renderables (the disabled-path fallback).
 *     (B2) CHUNKING OVER THE 64-INSTANCE LIMIT: a synthetic group of
 *          M = getMaxAutomaticInstances() + 2 members splits into
 *          ceil(M / max) chunks covering all members exactly. Asserted both
 *          through the pure compute_instance_groups (group count == ceil(M/max),
 *          the union of members == {0..M-1} exactly, every group in [2, max])
 *          and through SceneBuilder's realisation (the scene holds exactly
 *          ceil(M/max) instanced renderables — one per chunk's single subset).
 *          If the NOOP engine reports max < 2 (the no-instancing fallback) this
 *          asserts the per-instance realisation instead (M renderables) — the
 *          documented  fallback.
 *     (B3) BUILD-ONCE + PER-FRAME SYNC: after the instanced scene is
 *          built once, driving several update_transforms frames with ANIMATED
 *          (changing) member transforms — the exact per-frame call the backend
 *          makes — leaves the scene renderable count UNCHANGED and built() true:
 *          the renderables and InstanceBuffers are built once and only their
 *          local transforms refreshed (never rebuilt). The per-instance
 *          placement tracking itself (each member placed by its own local
 *          transform) is the pure, deterministic logic covered by the host
 *          property test (prop_instance_grouping) and driven here without
 *          rebuild; the genuine on-screen instanced placement + ≤1/255 parity is
 *          checked through separate full-scene captures and comparison.
 *
 * HERMETIC RATIONALE: SceneBuilder builds real Filament renderables (with
 * .instances() + a real InstanceBuffer) and MaterialInstances from the embedded
 * compiled packages, and MeshStore builds real VertexBuffer / IndexBuffer
 * objects. The headless RapidCheck target (heaven_benchmark_tests) links NEITHER
 * Filament NOR SDL, so it cannot build them; this renderer-linked target can —
 * matching the gated GPU smoke-test strategy. NOOP needs no GPU/display
 * and isolates the assertion from the shared backend. The pure grouping logic is
 * covered by the host property test (prop_instance_grouping); the genuine
 * on-device instanced placement and pixel parity require separate full-scene
 * captures and accepted reference images.
 *
 * NO NEW BACKEND SEAM: the instanced-build observable is the Filament Scene's
 * own getRenderableCount() (the same seam the sibling toggle/registration cases
 * use) plus the pure compute_instance_groups helper, so this case asserts the
 * host-verifiable facts directly rather than adding a new debug_* accessor —
 * exactly as  did with SceneBuilder::skinned_entities().
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if its own deeper bring-up (init_pipelines,
 * or the NOOP engine / base materials / mesh upload) fails, tolerating a
 * headless/driver-less environment that passed the gate.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "rhi/filament_backend.h"
#include "rhi/scene_builder.h"
#include "rhi/instance_grouping.h"
#include "rhi/mesh_store.h"
#include "rhi/material_instance_cache.h"
#include "rhi/texture_store.h"
#include "rhi/backend_core.h"
#include "engine/types.h"
#include "engine/camera.h"

#include <filament/Engine.h>
#include <filament/Scene.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_gpu_instancing]";

// Render exactly one frame with an EMPTY scene against the live backend.
// Mirrors smoke_perframe_discipline.cpp / smoke_gpu_skinning.cpp: the backend
// tolerates empty instance/mesh spans (build creates zero entities), so the
// frame drives the full per-frame path — the one-time build block and the
// per-frame transform sync — without loading the heavy Heaven scene.
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

// A static (non-skinned) single-triangle mesh — the geometry every Instance in
// this case shares. Layout matches smoke_mesh_store.cpp's triangle. The single
// surface (name carries no LOD marker, so it is never an imposter) with the
// default GpuMaterial (casts_shadow=true) and GpuMesh.cast_shadows=true yields
// exactly ONE Shadow_Split subset (the caster), so each instanced chunk / each
// per-instance build realises as exactly one renderable.
Mesh make_instanced_mesh() {
    Mesh mesh;
    mesh.name = "smoke_inst";
    mesh.path = "smoke_inst.mesh";
    Surface surf;
    surf.name = "smoke_inst_surf";
    surf.vertices = {
        {0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f,  1, 0, 0, 1},
        {1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f,  1, 0, 0, 1},
        {0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f,  1, 0, 0, 1},
    };
    surf.indices = {0, 1, 2};
    mesh.surfaces.push_back(std::move(surf));
    return mesh;
}

// Build a list of `n` Instances all referencing mesh_index 0, placed along X so
// their transforms differ (proves the per-frame sync has real, changing data to
// track). All members fit in one startup cell, independently of the engine's
// instance-count limit, so this fixture still isolates count-based chunking.
std::vector<Instance> make_instances(int n, float t) {
    std::vector<Instance> instances;
    instances.reserve(n);
    for (int i = 0; i < n; i++) {
        glm::mat4 m = glm::translate(glm::mat4(1.0f),
                                     glm::vec3(16.0f * static_cast<float>(i) / static_cast<float>(std::max(n, 1)) + t,
                                               0.0f, 0.0f));
        instances.push_back(Instance{0, m});
    }
    return instances;
}

} // namespace

CaseResult run_gpu_instancing_case(const CaseContext& ctx) {
    bool ok = true;

    // ════════════════════════════════════════════════════════════
    // HALF A — build-once discipline + per-frame transform sync (LIVE backend)
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

        // Scene (incl. any instanced renderables + InstanceBuffers) built once.
        if (after.scene_builds != 1) {
            std::printf("%s FAIL: scene_builds == %llu after %d frames "
                        "(expected 1 — the scene, including instanced "
                        "renderables, must build once; per-frame work is only "
                        "the InstanceBuffer transform refresh, Req 2.1)\n", kTag,
                        static_cast<unsigned long long>(after.scene_builds),
                        kFrames);
            ok = false;
        }
        // Per-frame transform sync ran every frame (the only per-frame mutation).
        const uint64_t tdelta = after.transform_updates - baseline.transform_updates;
        if (tdelta != static_cast<uint64_t>(kFrames)) {
            std::printf("%s FAIL: transform_updates advanced by %llu over %d "
                        "frames (expected %d — the per-frame transform/instance "
                        "sync must run every frame, Req 2.1)\n", kTag,
                        static_cast<unsigned long long>(tdelta), kFrames,
                        kFrames);
            ok = false;
        }
    }

    // ════════════════════════════════════════════════════════════
    // HALF B — instanced build / chunking / disabled-path / build-once
    //          (HERMETIC NOOP)
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
            filament::Engine::destroy(&engine);
            return ok ? CaseResult::Skip : CaseResult::Fail;
        }

        BackendCore core{};
        core.engine = engine;
        core.scene  = scene;

        MaterialResolver resolver =
            [](const std::string&, const std::string&) -> GpuMaterial {
                return GpuMaterial{};  // default: not hidden, casts_shadow=true
            };

        // One shared static mesh; every Instance references mesh_index 0.
        const Mesh mesh = make_instanced_mesh();
        const GpuMesh gm = store.upload(mesh);
        if (gm.surfaces.empty()) {
            std::printf("%s SKIP: upload produced no surfaces on NOOP "
                        "(Half A assertions already evaluated)\n", kTag);
            engine->destroy(scene);
            filament::Engine::destroy(&engine);
            return ok ? CaseResult::Skip : CaseResult::Fail;
        }
        const std::vector<GpuMesh> meshes = { gm };
        const std::unordered_set<int> no_skinned;  // nothing skinned here

        // The single Shadow_Split caster subset ⇒ exactly one renderable per
        // chunk and one per per-instance build, so the renderable count is a
        // clean witness of the instanced vs per-instance decision.
        constexpr size_t kSubsetsPerRenderable = 1;

        const size_t max_per_group = engine->getMaxAutomaticInstances();

        // ── B1: instanced build yields ONE renderable, not N; the
        // disabled path yields N per-instance renderables  ──────────
        if (max_per_group >= 2) {
            const int n = static_cast<int>(std::min<size_t>(4, max_per_group));
            const std::vector<Instance> instances = make_instances(n, 0.0f);

            SceneBuilder sb_on;
            sb_on.build(core, store, cache,
                        std::span<const Instance>(instances),
                        std::span<const GpuMesh>(meshes),
                        resolver, /*gpu_skinning=*/false,
                        /*gpu_instancing=*/true, no_skinned);
            const size_t inst_count = scene->getRenderableCount();
            if (inst_count != kSubsetsPerRenderable) {
                std::printf("%s FAIL: %d instances sharing one mesh produced %zu "
                            "renderables with gpu_instancing=true (expected %zu — "
                            "one instanced renderable per Shadow_Split subset, "
                            "NOT %d per-instance renderables, Req 1.1)\n", kTag,
                            n, inst_count, kSubsetsPerRenderable, n);
                ok = false;
            }
            sb_on.clear(core);

            SceneBuilder sb_off;
            sb_off.build(core, store, cache,
                         std::span<const Instance>(instances),
                         std::span<const GpuMesh>(meshes),
                         resolver, /*gpu_skinning=*/false,
                         /*gpu_instancing=*/false, no_skinned);
            const size_t per_inst_count = scene->getRenderableCount();
            const size_t expected_off = static_cast<size_t>(n) * kSubsetsPerRenderable;
            if (per_inst_count != expected_off) {
                std::printf("%s FAIL: %d instances produced %zu renderables with "
                            "gpu_instancing=false (expected %zu — the disabled "
                            "path is one renderable per Instance per subset, "
                            "Req 7.2)\n", kTag, n, per_inst_count, expected_off);
                ok = false;
            }
            sb_off.clear(core);

            // Non-vacuity: instancing must genuinely COLLAPSE the renderable
            // count (1 vs N), not be a no-op. (n >= 2, so expected_off > 1.)
            if (kSubsetsPerRenderable >= expected_off) {
                std::printf("%s FAIL: instanced (%zu) vs per-instance (%zu) "
                            "renderable counts are not distinct — the instanced "
                            "build is not collapsing the count (vacuous test)\n",
                            kTag, kSubsetsPerRenderable, expected_off);
                ok = false;
            }
        } else {
            std::printf("%s NOTE: engine reports max automatic instances = %zu "
                        "(< 2) — exercising the per-instance fallback in B2\n",
                        kTag, max_per_group);
        }

        // ── B2: chunking over the per-renderable instance limit  ───
        // Pure-helper expectation first, then the SceneBuilder realisation.
        if (max_per_group >= 2 && max_per_group <= 100000) {
            const int m = static_cast<int>(max_per_group) + 2;  // > max ⇒ splits
            const size_t expected_chunks =
                (static_cast<size_t>(m) + max_per_group - 1) / max_per_group;
            const std::vector<Instance> instances = make_instances(m, 0.0f);

            // (a) Pure compute_instance_groups: ceil(m/max) groups, every group
            // in [2, max], and the union of members == {0..m-1} exactly.
            const std::vector<InstanceGroup> groups = compute_instance_groups(
                std::span<const Instance>(instances), no_skinned, max_per_group);
            if (groups.size() != expected_chunks) {
                std::printf("%s FAIL: compute_instance_groups split %d members "
                            "(max=%zu) into %zu groups (expected ceil = %zu)\n",
                            kTag, m, max_per_group, groups.size(), expected_chunks);
                ok = false;
            }
            std::unordered_set<int> covered;
            bool bounds_ok = true;
            for (const auto& g : groups) {
                if (g.instance_indices.size() < 2 ||
                    g.instance_indices.size() > max_per_group) {
                    bounds_ok = false;
                }
                for (int idx : g.instance_indices) covered.insert(idx);
            }
            if (!bounds_ok) {
                std::printf("%s FAIL: a chunk's member count fell outside "
                            "[2, %zu] — chunks must be bounded by the instance "
                            "limit (Req 1.1)\n", kTag, max_per_group);
                ok = false;
            }
            if (covered.size() != static_cast<size_t>(m)) {
                std::printf("%s FAIL: the chunks cover %zu distinct members "
                            "(expected all %d — chunking must cover every member "
                            "exactly once)\n", kTag, covered.size(), m);
                ok = false;
            }

            // (b) SceneBuilder realisation: exactly expected_chunks instanced
            // renderables (each chunk's single Shadow_Split subset).
            SceneBuilder sb_chunk;
            sb_chunk.build(core, store, cache,
                           std::span<const Instance>(instances),
                           std::span<const GpuMesh>(meshes),
                           resolver, /*gpu_skinning=*/false,
                           /*gpu_instancing=*/true, no_skinned);
            const size_t chunk_count = scene->getRenderableCount();
            const size_t expected_renderables =
                expected_chunks * kSubsetsPerRenderable;
            if (chunk_count != expected_renderables) {
                std::printf("%s FAIL: a group of %d members (max=%zu) realised as "
                            "%zu instanced renderables (expected %zu — one per "
                            "chunk subset, Req 1.1)\n", kTag, m, max_per_group,
                            chunk_count, expected_renderables);
                ok = false;
            }
            sb_chunk.clear(core);
        } else if (max_per_group < 2) {
            // No-instancing fallback: with max < 2 SceneBuilder builds
            // every instance via the per-instance path. Assert that realisation.
            const int m = 5;
            const std::vector<Instance> instances = make_instances(m, 0.0f);
            SceneBuilder sb_fallback;
            sb_fallback.build(core, store, cache,
                              std::span<const Instance>(instances),
                              std::span<const GpuMesh>(meshes),
                              resolver, /*gpu_skinning=*/false,
                              /*gpu_instancing=*/true, no_skinned);
            const size_t fallback_count = scene->getRenderableCount();
            const size_t expected_fallback =
                static_cast<size_t>(m) * kSubsetsPerRenderable;
            if (fallback_count != expected_fallback) {
                std::printf("%s FAIL: with max automatic instances < 2 the %d "
                            "instances must fall back to the per-instance path "
                            "(expected %zu renderables, got %zu — Req 7.5)\n",
                            kTag, m, expected_fallback, fallback_count);
                ok = false;
            }
            sb_fallback.clear(core);
        } else {
            std::printf("%s NOTE: engine reports an implausibly large max "
                        "automatic instances = %zu; skipping the SceneBuilder "
                        "chunk realisation (would allocate too many instances)\n",
                        kTag, max_per_group);
        }

        // Spatially separated repetitions produce independent persistent groups.
        if (max_per_group >= 2) {
            auto instances = make_instances(4, 0);
            instances[0].transform[3].x = 0;
            instances[1].transform[3].x = 1;
            instances[2].transform[3].x = 64;
            instances[3].transform[3].x = 65;
            SceneBuilder spatial;
            spatial.build(core, store, cache, instances, meshes, resolver,
                false, true, no_skinned);
            if (scene->getRenderableCount() != 2 * kSubsetsPerRenderable) {
                std::printf("%s FAIL: two occupied cells must produce two instanced groups\n", kTag);
                ok = false;
            }
            spatial.clear(core);
        }

        // ── B3: build-once + per-frame sync  ───────────────────────
        // Build the instanced scene ONCE, then drive several update_transforms
        // frames with ANIMATED (changing) member transforms — the exact
        // per-frame call the backend makes — and assert the renderable count is
        // UNCHANGED and built() stays true: renderables + InstanceBuffers build
        // once, only their local transforms refresh.
        if (max_per_group >= 2) {
            const int n = static_cast<int>(std::min<size_t>(4, max_per_group));
            std::vector<Instance> instances = make_instances(n, 0.0f);

            SceneBuilder sb_sync;
            sb_sync.build(core, store, cache,
                          std::span<const Instance>(instances),
                          std::span<const GpuMesh>(meshes),
                          resolver, /*gpu_skinning=*/false,
                          /*gpu_instancing=*/true, no_skinned);
            const size_t count_built = scene->getRenderableCount();
            if (!sb_sync.built()) {
                std::printf("%s FAIL: SceneBuilder.built() is false after an "
                            "instanced build\n", kTag);
                ok = false;
            }

            constexpr int kSyncFrames = 5;
            for (int f = 1; f <= kSyncFrames; ++f) {
                // Animate the placements so the per-frame sync has real, changing
                // data to push into the InstanceBuffer.
                instances = make_instances(n, static_cast<float>(f) * 0.25f);
                std::vector<int> dirty;
                dirty.reserve(static_cast<size_t>(n));
                for (int i = 0; i < n; ++i) dirty.push_back(i);
                sb_sync.update_transforms(
                    core, std::span<const Instance>(instances), dirty);
            }

            const size_t count_after_sync = scene->getRenderableCount();
            if (count_after_sync != count_built) {
                std::printf("%s FAIL: the scene renderable count changed across "
                            "%d update_transforms frames (%zu -> %zu) — the "
                            "instanced renderables/buffers must build once and "
                            "only refresh transforms (Req 2.1)\n", kTag,
                            kSyncFrames, count_built, count_after_sync);
                ok = false;
            }
            if (!sb_sync.built()) {
                std::printf("%s FAIL: SceneBuilder.built() flipped false across "
                            "the per-frame sync (the scene must stay built)\n",
                            kTag);
                ok = false;
            }
            sb_sync.clear(core);
        }

        // Destroy the scene before the store/textures/cache destructors (block
        // exit) and before the engine teardown below.
        engine->destroy(scene);
    }

    filament::Engine::destroy(&engine);

    if (!ok) return CaseResult::Fail;

    std::printf("%s GPU-instancing contract honored: live build-once discipline "
                "(scene_builds==1, per-frame transforms only, Req 2.1/7.2); "
                "hermetic N instances of one mesh collapsed to one instanced "
                "renderable while gpu_instancing=false produced N per-instance "
                "renderables (Req 1.1/7.2), a group over the instance limit split "
                "into ceil(M/max) chunks covering all members exactly (Req 1.1), "
                "and per-frame update_transforms left the renderable count "
                "unchanged — build-once, transform-refresh-only (Req 2.1). "
                "On-device placement and pixel parity require separate full-scene "
                "captures and comparison\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
