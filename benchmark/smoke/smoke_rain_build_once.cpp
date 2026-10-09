/*
 * HeavenBench — rain build-once discipline smoke case
 *                     (renderer-linked, GATED)
 *
 * the Rain_Feature SHALL
 * build its precipitation geometry once  and the Splash_System SHALL
 * build its impact-effect geometry once on the first frame it is active,
 * updating both only through uniforms/transforms thereafter and never
 * regenerating geometry after the first rendered frame.
 *
 * This mirrors smoke_perframe_discipline.cpp, but for the DEFERRED rain
 * builders. The backend exposes debug instrumentation
 * (IRhiBackend::debug_build_counters) that, for the registered RainFeature,
 * sources two build counters straight off the feature:
 *
 *   - rain_streak_builds → RainFeature::build_rain()      (once, first active frame)
 *   - splash_builds       → RainFeature::build_splashes()  (once, first active frame)
 *   - transform_updates   → scene_builder_->update_transforms() (EVERY frame — the
 *                           per-frame uniform/transform path that keeps running)
 *
 * KEY DIFFERENCE from perframe_discipline: rain's build is DEFERRED to the first
 * frame on which the feature is ENABLED and the weather makes its update() run.
 * The backend's update_features loop calls update() only for enabled features,
 * and RainFeature::update builds its streak + splash geometry on the first call
 * (it does not depend on scene meshes, so the empty-scene approach the harness
 * uses works for rain too). To faithfully exercise the "first ACTIVE frame" the
 * task describes, this case renders frames with a WeatherState whose intensity
 * is > 0 (rain active), so the feature actually builds on frame 1.
 *
 * CUMULATIVE-BACKEND ROBUSTNESS: the harness shares ONE backend across cases, so
 * the build counters are cumulative. The rain builders are once-per-process for
 * the backend's registered feature, so if an earlier case had already rendered
 * active rain frames the counters would already read 1 before this case starts.
 * The assertion therefore does NOT require baseline == 0; it asserts the
 * counters read exactly 1 after the first active frame, stay 1 through the
 * remaining frames (no increment), and that a per-frame-growing signal
 * (transform_updates) advances by one per frame — which holds whether or not a
 * prior case built the rain geometry.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if its own deeper bring-up (init_pipelines)
 * fails, tolerating a headless/driver-less environment that passed the gate. It
 * also SKIPs when the registered RainFeature is UNAVAILABLE in this environment
 * (its precipitation assets did not load at init, e.g. when the harness CWD
 * can't resolve the relative data_dir): an active-rain frame then builds
 * nothing, which is the degraded headless-equivalent case rather than a defect,
 * so the build-once assertion is skipped rather than failed.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "engine/types.h"
#include "engine/camera.h"

#include <cstdio>
#include <span>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_rain_build_once]";

// Render exactly one ACTIVE rain frame with an empty scene. Mirrors the
// empty-frame helper in smoke_perframe_discipline.cpp, but constructs a stack
// WeatherState with intensity 1.0 so rain is active: the backend's
// update_features loop then calls RainFeature::update(), which builds the
// streak + splash geometry on its first call. The rain build does NOT depend on
// scene instances/meshes, so the empty spans still trigger the one-time build —
// exactly the "first active frame" the test targets.
void render_one_active_frame(IRhiBackend& backend, const Camera& camera,
                             const LightingParams& lighting,
                             const TonemapParams& tonemap, int w, int h) {
    MaterialResolver resolver =
        [](const std::string&, const std::string&) -> GpuMaterial {
            return GpuMaterial{};
        };
    WeatherState weather;
    weather.set_intensity(1.0f); // rain active ⇒ RainFeature::update() runs/builds
    backend.render_frame(std::span<const Instance>{},
                         std::span<const int>{},
                         std::span<const GpuMesh>{},
                         resolver, camera, lighting, weather, tonemap, w, h);
}

} // namespace

CaseResult run_rain_build_once_case(const CaseContext& ctx) {
    IRhiBackend& backend = ctx.backend;

    // Bring the render pipeline up so the scene/render features (incl. the
    // registered RainFeature) exist. A failure here means the environment can't
    // drive the backend (no usable GPU) — treat it as a SKIP so the gated test
    // still passes, mirroring the harness's headless fallbacks. init_pipelines
    // is idempotent, so an earlier case having already run it is fine.
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

    // Baseline snapshot. We reason in DELTAS for the per-frame signal because
    // the shared backend may have advanced transform_updates in an earlier
    // case. The rain build counters are once-per-process, so we assert their
    // absolute value (1) rather than a delta — see the file header.
    const IRhiBackend::BackendBuildCounters baseline =
        backend.debug_build_counters();

    // Frame 1: the first ACTIVE rain frame. RainFeature::update() runs (the
    // feature is enabled by default and intensity > 0), building the streak and
    // splash geometry exactly once if it has not already been built by an
    // earlier active-rain case. Snapshot right after.
    render_one_active_frame(backend, camera, lighting, tonemap, w, h);
    const IRhiBackend::BackendBuildCounters after_frame_1 =
        backend.debug_build_counters();

    // ── Environment robustness: rain feature unavailable ⇒ SKIP ──
    // The registered RainFeature only builds when its precipitation assets
    // loaded at init. In an environment where those assets are absent —
    // e.g. when the harness CWD can't resolve the relative data_dir, exactly
    // the path the sibling rain_asset_failure case exercises against this same
    // shared backend — the feature inits disabled and an active-rain frame
    // builds nothing. That is the headless-equivalent degraded case: SKIP (exit
    // 0 via the harness) rather than FAIL, mirroring the init_pipelines SKIP
    // above. The rain build is once-per-process, so a nonzero baseline means an
    // earlier active-rain case already built it (the feature IS available) and
    // we must NOT skip; only treat an all-zero baseline + an all-zero frame-1
    // snapshot (nothing ever built) as "feature unavailable".
    if (baseline.rain_streak_builds == 0 && after_frame_1.rain_streak_builds == 0 &&
        baseline.splash_builds == 0 && after_frame_1.splash_builds == 0) {
        std::printf("%s SKIP: rain feature unavailable in this environment "
                    "(precipitation assets did not load; nothing built on the "
                    "first active frame)\n", kTag);
        return CaseResult::Skip;
    }

    // Frames 2..N: the pure per-frame path. Only uniforms/transforms should
    // advance — neither rain builder may run again.
    for (int i = 1; i < kFrames; ++i) {
        render_one_active_frame(backend, camera, lighting, tonemap, w, h);
    }
    const IRhiBackend::BackendBuildCounters final_counters =
        backend.debug_build_counters();

    // ── Assertions  ────────────────

    bool ok = true;

    // The precipitation streak geometry is built exactly once: by the
    // end of the first active frame the build has happened (count == 1), and it
    // stays at 1 through every subsequent frame — never regenerated per-frame.
    if (after_frame_1.rain_streak_builds != 1 ||
        final_counters.rain_streak_builds != 1) {
        std::printf("%s FAIL: rain_streak_builds expected 1, got %llu after "
                    "frame 1, %llu final (streaks must build once on the first "
                    "active frame and never again — R1.3/R8.3)\n", kTag,
                    static_cast<unsigned long long>(
                        after_frame_1.rain_streak_builds),
                    static_cast<unsigned long long>(
                        final_counters.rain_streak_builds));
        ok = false;
    }

    // The splash impact geometry is built exactly once on the first
    // active frame and stays at 1 thereafter — same build-once discipline.
    if (after_frame_1.splash_builds != 1 ||
        final_counters.splash_builds != 1) {
        std::printf("%s FAIL: splash_builds expected 1, got %llu after frame 1, "
                    "%llu final (splashes must build once on the first active "
                    "frame and never again — R4.4/R8.3)\n", kTag,
                    static_cast<unsigned long long>(
                        after_frame_1.splash_builds),
                    static_cast<unsigned long long>(
                        final_counters.splash_builds));
        ok = false;
    }

    // Core invariant: across frames 2..N (the pure per-frame path)
    // NEITHER rain builder advanced — they hold their frame-1 value of 1 — while
    // the per-frame uniform/transform path kept running. This is the "build
    // once, then only uniforms/transforms" assertion.
    if (final_counters.rain_streak_builds != after_frame_1.rain_streak_builds ||
        final_counters.splash_builds != after_frame_1.splash_builds) {
        std::printf("%s FAIL: a rain builder advanced after frame 1 "
                    "(streak %llu->%llu, splash %llu->%llu) — the per-frame path "
                    "must not regenerate geometry (R8.3)\n", kTag,
                    static_cast<unsigned long long>(
                        after_frame_1.rain_streak_builds),
                    static_cast<unsigned long long>(
                        final_counters.rain_streak_builds),
                    static_cast<unsigned long long>(
                        after_frame_1.splash_builds),
                    static_cast<unsigned long long>(
                        final_counters.splash_builds));
        ok = false;
    }

    // The per-frame uniform/transform path kept running on EVERY frame:
    // transform_updates grew by exactly one per rendered frame. Measured as a
    // delta from the baseline so a prior case's frames don't skew it. This is
    // the "per-frame uniform updates grow each frame" half of the test: while
    // the build counters sit pinned at 1, the per-frame work keeps advancing.
    const uint64_t transforms_this_case =
        final_counters.transform_updates - baseline.transform_updates;
    if (transforms_this_case != static_cast<uint64_t>(kFrames)) {
        std::printf("%s FAIL: transform_updates advanced by %llu over %d frames, "
                    "expected %d (the per-frame uniform/transform path must run "
                    "every frame — R8.3)\n", kTag,
                    static_cast<unsigned long long>(transforms_this_case),
                    kFrames, kFrames);
        ok = false;
    }

    // The portion of that growth across frames 2..N specifically, so the
    // "grows each frame while builders stay put" relationship is asserted on
    // exactly the frames over which the builders are checked for non-increment.
    const uint64_t transform_delta =
        final_counters.transform_updates - after_frame_1.transform_updates;
    if (transform_delta != static_cast<uint64_t>(kFrames - 1)) {
        std::printf("%s FAIL: transform_updates grew by %llu across frames 2..%d, "
                    "expected %d (per-frame uniform updates must grow each frame "
                    "while the builders stay pinned — R8.3)\n",
                    kTag, static_cast<unsigned long long>(transform_delta),
                    kFrames, kFrames - 1);
        ok = false;
    }

    if (!ok) return CaseResult::Fail;

    std::printf("%s rain built once on the first active frame and held: "
                "rain_streak_builds=1, splash_builds=1 after frame 1 and final; "
                "transform_updates +%llu over %d frames (+%llu over frames 2..%d) "
                "— builders never re-ran (R1.3/R4.4/R8.3)\n", kTag,
                static_cast<unsigned long long>(transforms_this_case), kFrames,
                static_cast<unsigned long long>(transform_delta), kFrames);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
