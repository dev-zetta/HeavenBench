/*
 * HeavenBench — per-frame discipline smoke case (renderer-linked, GATED)
 *
 * when the benchmark updates per-frame state it
 * SHALL update only entity transforms and animation uniforms, SHALL reuse the
 * persistent scene entities, grass blades, and bounding boxes built at
 * initialization, AND SHALL NOT regenerate scene geometry, grass blades, or
 * bounding boxes.
 *
 * The backend exposes debug instrumentation (IRhiBackend::debug_build_counters)
 * that records how many times each one-time build site ran versus the per-frame
 * transform sync:
 *
 *   - scene_builds      → scene_builder_->build()          (build the persistent
 *                          renderable entities + their vertex-derived AABBs —
 *                          the bounding boxes — exactly once)
 *   - emission_spawns   → light_spawner_->spawn_emission()  (once)
 *   - grass_generations → grass blade generation            (deferred, once)
 *   - transform_updates → scene_builder_->update_transforms (EVERY frame — the
 *                          one allowed per-frame mutation)
 *
 * This case renders several empty-scene frames and asserts the invariant: the
 * builders run at most once and DO NOT advance between the post-frame-1 snapshot
 * and the final snapshot, while transform_updates increases by one per frame.
 * That is the core "never calls scene/grass/bbox builders per-frame; only
 * transforms" assertion.
 *
 * The harness shares ONE backend across cases, so the counters are cumulative
 * (an earlier case may already have built the scene and advanced the transform
 * sync). The case therefore reasons in DELTAS around its own frames rather than
 * absolute totals.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if its own deeper bring-up (init_pipelines)
 * fails, tolerating a headless/driver-less environment that passed the gate.
 *
 * Note on the empty scene: with no instances/meshes, scene_builder_->build runs
 * once (creating zero entities — built() still flips true) and grass is never
 * generated (its deferred build needs non-empty instance data), so
 * grass_generations stays 0. The invariant we assert is "not regenerated
 * per-frame", expressed as grass_generations <= 1 AND no increase across frames,
 * which holds for 0 just as it would for 1 on the full Heaven scene.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "engine/types.h"
#include "engine/camera.h"

#include <cstdio>
#include <span>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_perframe_discipline]";

// Render exactly one frame with an empty scene. Mirrors the empty-frame pattern
// from smoke_overlay_render.cpp: the backend tolerates empty instance/mesh
// spans (build creates zero entities; every feature's update() guards on empty
// inputs), so the frame drives the full per-frame path — including the one-time
// build block and the per-frame transform sync — without loading the heavy
// Heaven scene.
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

} // namespace

CaseResult run_perframe_discipline_case(const CaseContext& ctx) {
    IRhiBackend& backend = ctx.backend;

    // Bring the render pipeline up so the scene/render features exist. A
    // failure here means the environment can't drive the backend (no usable
    // GPU) — treat it as a SKIP so the gated test still passes, mirroring the
    // harness's headless fallbacks.
    if (auto r = backend.init_pipelines(ctx.config); !r) {
        std::printf("%s SKIP: init_pipelines failed (%s)\n",
                    kTag, r.error().message.c_str());
        return CaseResult::Skip;
    }

    Camera camera;
    LightingParams lighting = lighting_for_time_of_day(ctx.config.time_of_day);
    TonemapParams  tonemap;

    const int w = ctx.width;
    const int h = ctx.height;
    constexpr int kFrames = 5;

    // The smoke harness shares ONE backend across cases, so the build counters
    // are cumulative: an earlier case (overlay_render) may already have built
    // the scene and advanced transform_updates. We therefore reason in DELTAS
    // around this case's frames rather than absolute totals — which is exactly
    // what  is about: across the per-frame path the builders must not
    // advance while the transform sync runs once per frame.
    const IRhiBackend::BackendBuildCounters baseline =
        backend.debug_build_counters();

    // Frame 1: the one-time builders run here only if they have not already run
    // (the persistent scene entities + their vertex-derived bounding boxes and
    // the emission lights are a global one-time build). Snapshot right after.
    render_one_empty_frame(backend, camera, lighting, tonemap, w, h);
    const IRhiBackend::BackendBuildCounters after_frame_1 =
        backend.debug_build_counters();

    // Frames 2..N: the pure per-frame path. Only the transform sync should
    // advance here — no builder may run.
    for (int i = 1; i < kFrames; ++i) {
        render_one_empty_frame(backend, camera, lighting, tonemap, w, h);
    }
    const IRhiBackend::BackendBuildCounters final_counters =
        backend.debug_build_counters();

    // ── Assertions  ──────────────────────────

    bool ok = true;

    // Scene entities (and their vertex-derived bounding boxes) are built at
    // most once over the whole process — never regenerated per-frame. By the
    // end of frame 1 the build has happened exactly once (whether on this
    // frame or in an earlier case), so the count sits at 1 and stays there.
    if (after_frame_1.scene_builds != 1 || final_counters.scene_builds != 1) {
        std::printf("%s FAIL: scene_builds expected 1, got %llu after frame 1, "
                    "%llu final (scene geometry/bboxes must build once)\n", kTag,
                    static_cast<unsigned long long>(
                        after_frame_1.scene_builds),
                    static_cast<unsigned long long>(
                        final_counters.scene_builds));
        ok = false;
    }

    // Emission lights spawned exactly once (same one-time build block).
    if (after_frame_1.emission_spawns != 1 || final_counters.emission_spawns != 1) {
        std::printf("%s FAIL: emission_spawns expected 1, got %llu after frame 1, "
                    "%llu final\n", kTag,
                    static_cast<unsigned long long>(
                        after_frame_1.emission_spawns),
                    static_cast<unsigned long long>(
                        final_counters.emission_spawns));
        ok = false;
    }

    // Grass never regenerated per-frame: at most one generation over the run.
    // (Empty scene ⇒ 0; full scene ⇒ 1. Both satisfy <= 1.)
    if (final_counters.grass_generations > 1) {
        std::printf("%s FAIL: grass_generations expected <= 1, got %llu "
                    "(grass blades must not regenerate per-frame)\n", kTag,
                    static_cast<unsigned long long>(
                        final_counters.grass_generations));
        ok = false;
    }

    // The per-frame transform sync ran on EVERY frame this case rendered —
    // proving the per-frame path DOES perform transform updates. Measured as a
    // delta from the baseline so a prior case's frames don't skew it.
    const uint64_t transforms_this_case =
        final_counters.transform_updates - baseline.transform_updates;
    if (transforms_this_case != static_cast<uint64_t>(kFrames)) {
        std::printf("%s FAIL: transform_updates advanced by %llu over %d frames, "
                    "expected %d (per-frame transform sync must run every frame)\n",
                    kTag,
                    static_cast<unsigned long long>(transforms_this_case),
                    kFrames, kFrames);
        ok = false;
    }

    // Core invariant: between the after-frame-1 snapshot and the final snapshot
    // (frames 2..N — the pure per-frame path), the builders DID NOT advance
    // (they stay at their frame-1 values), while transform_updates increased by
    // (N-1). This is the "never calls scene/grass/bbox builders per-frame; only
    // transforms" assertion.
    if (final_counters.scene_builds != after_frame_1.scene_builds ||
        final_counters.emission_spawns != after_frame_1.emission_spawns ||
        final_counters.grass_generations != after_frame_1.grass_generations) {
        std::printf("%s FAIL: a one-time builder advanced after frame 1 "
                    "(scene %llu->%llu, emission %llu->%llu, grass %llu->%llu) — "
                    "the per-frame path must not call builders\n", kTag,
                    static_cast<unsigned long long>(
                        after_frame_1.scene_builds),
                    static_cast<unsigned long long>(
                        final_counters.scene_builds),
                    static_cast<unsigned long long>(
                        after_frame_1.emission_spawns),
                    static_cast<unsigned long long>(
                        final_counters.emission_spawns),
                    static_cast<unsigned long long>(
                        after_frame_1.grass_generations),
                    static_cast<unsigned long long>(
                        final_counters.grass_generations));
        ok = false;
    }

    const uint64_t transform_delta =
        final_counters.transform_updates - after_frame_1.transform_updates;
    if (transform_delta != static_cast<uint64_t>(kFrames - 1)) {
        std::printf("%s FAIL: transform_updates grew by %llu across frames 2..%d, "
                    "expected %d (the per-frame mutation must run each frame)\n",
                    kTag, static_cast<unsigned long long>(transform_delta),
                    kFrames, kFrames - 1);
        ok = false;
    }

    if (!ok) return CaseResult::Fail;

    std::printf("%s per-frame path mutated only transforms: scene_builds=1, "
                "emission_spawns=1, grass_generations=%llu (<=1), "
                "transform_updates +%llu over %d frames (+%llu over frames "
                "2..%d); builders never re-ran\n", kTag,
                static_cast<unsigned long long>(
                    final_counters.grass_generations),
                static_cast<unsigned long long>(transforms_this_case), kFrames,
                static_cast<unsigned long long>(transform_delta), kFrames);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
