/*
 * HeavenBench — toggle isolation / round-trip smoke case
 *                     (renderer-linked, GATED)
 *
 * when a new
 * Render_Feature is toggled at runtime it SHALL add/remove ONLY its own
 * renderables from the shared scene; toggling OFF leaves the scene with
 * none of that feature's renderables  and a feature's restore leaves the
 * view at its shipping state; toggling ON restores its renderables; and
 * an off→on round-trip restores the prior visual contribution from the
 * build-once buffers without restarting the renderer.
 *
 * This case asserts those three sub-claims against the live backend:
 *
 *   1. NO OTHER feature's scene membership changes when rain is toggled off→on.
 *      Observed through the main scene's renderable count
 *      (debug_render_context().scene->getRenderableCount()): after rain is
 *      active (built + in scene) we snapshot the count, toggle rain OFF and
 *      snapshot again (the count drops by exactly rain's renderable count — its
 *      streak + splash entities, 2 when both built), then toggle rain ON and
 *      snapshot (the count returns to the original). Because the OFF→ON
 *      round-trip returns the count to EXACTLY its pre-toggle value and the drop
 *      on OFF equals the rise on ON, only rain's renderables moved — no other
 *      feature's membership changed.
 *
 *   2. Rain's renderables are removed then RESTORED FROM THE BUILD-ONCE BUFFERS
 * The rain build counters (debug_build_counters().
 *      rain_streak_builds / splash_builds) DO NOT change across the toggle
 *      off→on cycle — they stay at 1, proving the toggle reused the build-once
 *      buffers and did NOT rebuild. Combined with the scene-count round-trip in
 *      (1), this shows the entities were removed and re-added, not regenerated.
 *
 *   3. The wet-reflections restore leaves the wetness global and SSR options at
 *      their SHIPPING values when off (and). After an active
 *      frame the wet-reflections feature has pushed the storm wetness global and
 *      retuned SSR; toggling it OFF runs its restore path
 *      (on_enabled_changed(false) → setMaterialGlobal(0,{0,0,0,1}) +
 *      setScreenSpaceReflectionsOptions(shipping_ssr_)). Read back through
 *      debug_render_context().view, getMaterialGlobal(0) must equal {0,0,0,1}
 *      and getScreenSpaceReflectionsOptions() must equal the shipping options
 *      (enabled=true, thickness=0.2, bias=0.01, maxDistance=25.0, stride=1.5 —
 *      post_process.cpp).
 *
 * OBSERVABILITY — this case toggles features by name through one minimal
 * test-only seam on FilamentBackend, mirroring the existing debug_* seams
 * (debug_render_context / debug_build_counters / debug_find_feature /
 * debug_count_features / debug_depth_texture):
 *   - debug_set_feature_enabled(name, on) finds the registered feature and
 *     drives its IRenderFeature::set_enabled(render_ctx_, on), firing the
 *     feature's on_enabled_changed() add/remove (rain) or restore
 *     (wet-reflections) path. It keeps render_ctx_ (the non-const RenderContext
 *     a toggle needs) encapsulated in the backend. The renderer never calls it;
 *     documented test-only; adds no per-effect logic to the backend.
 * The scene renderable count, the SSR options and the material global are read
 * back through the existing debug_render_context() accessor (scene + view).
 *
 * This mirrors smoke_rain_depth_prepass.cpp (its SIBLING case) for the harness
 * conventions — the GraphicsApi::Filament guard + static_cast<FilamentBackend&>,
 * the init_pipelines SKIP, the render_one_active_frame helper (a stack
 * WeatherState at intensity 1.0), and the "rain feature unavailable ⇒ SKIP"
 * pattern keyed off debug_build_counters().rain_streak_builds.
 *
 * CUMULATIVE-BACKEND ROBUSTNESS: the harness shares ONE backend across cases.
 * This case reasons in DELTAS around the toggle (snapshot → toggle → snapshot)
 * rather than absolute counts, and it restores the backend to its as-found state
 * at the end (rain and wet-reflections both toggled back ON) so subsequent cases
 * see the default feature set. The rain build counters are once-per-process, so
 * they read 1 whether or not a prior active-rain case already built the geometry.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if its own deeper bring-up (init_pipelines)
 * fails, tolerating a headless/driver-less environment that passed the gate. It
 * also SKIPs when the registered RainFeature is UNAVAILABLE in this environment
 * (its precipitation assets did not load at init, so an active frame builds
 * nothing): there are then no rain renderables to move, the degraded
 * headless-equivalent case rather than a defect — exactly the SKIP path the
 * sibling rain cases use.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "rhi/filament_backend.h"
#include "rhi/render_feature.h"
#include "engine/types.h"
#include "engine/camera.h"

#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Options.h>

#include <math/vec4.h>

#include <cmath>
#include <cstdio>
#include <span>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_rain_toggle_roundtrip]";

// Render exactly one ACTIVE rain frame with an empty scene. Identical to the
// helper in the sibling rain cases: a stack WeatherState at intensity 1.0 so
// rain is active, which makes the backend's update_features loop call
// RainFeature::update() (build + enter scene) and WetReflectionsFeature::update()
// (push the storm wetness global + retune SSR).
void render_one_active_frame(IRhiBackend& backend, const Camera& camera,
                             const LightingParams& lighting,
                             const TonemapParams& tonemap, int w, int h) {
    MaterialResolver resolver =
        [](const std::string&, const std::string&) -> GpuMaterial {
            return GpuMaterial{};
        };
    WeatherState weather;
    weather.set_intensity(1.0f); // rain active ⇒ feature update()s run
    backend.render_frame(std::span<const Instance>{},
                         std::span<const int>{},
                         std::span<const GpuMesh>{},
                         resolver, camera, lighting, weather, tonemap, w, h);
}

// Small absolute tolerance for the SSR / material-global float read-back. The
// restore writes the shipping constants verbatim, so equality would hold, but a
// tiny epsilon guards against any benign float round-trip through the driver.
constexpr float kEps = 1e-4f;

[[nodiscard]] bool approx(float a, float b) { return std::fabs(a - b) <= kEps; }

} // namespace

CaseResult run_rain_toggle_roundtrip_case(const CaseContext& ctx) {
    // The toggle / scene / view accessors below are FilamentBackend-specific
    // test instrumentation. The harness always constructs a FilamentBackend (it
    // is the sole IRhiBackend), so this is safe; api() lets us bail gracefully
    // if that ever changes rather than risking a bad cast — mirroring
    // smoke_rain_depth_prepass.cpp.
    if (ctx.backend.api() != GraphicsApi::Filament) {
        std::printf("%s SKIP: case needs the Filament backend\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);

    // Bring the render pipeline up so the scene/render features (incl. the
    // registered RainFeature and WetReflectionsFeature) exist. A failure here
    // means the environment can't drive the backend (no usable GPU) — treat it
    // as a SKIP so the gated test still passes, mirroring the harness's
    // headless fallbacks. init_pipelines is idempotent, so an earlier case
    // having already run it is fine.
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

    // Bring rain active so it builds, enters the scene, and the wet-reflections
    // feature settles its storm wetness/SSR state. A few frames let the deferred
    // build and depth/wetness paths settle (mirror the sibling cases' timing).
    constexpr int kFrames = 4;
    for (int i = 0; i < kFrames; ++i) {
        render_one_active_frame(backend, camera, lighting, tonemap, w, h);
    }

    // ── Environment robustness: rain feature unavailable ⇒ SKIP ──
    // The registered RainFeature only builds (and only then has renderables to
    // move on a toggle) when its precipitation assets loaded at init. In
    // an environment where those assets are absent the feature inits disabled
    // and active frames build nothing — there are no rain renderables to remove
    // restore, the headless-equivalent degraded case. SKIP rather than FAIL,
    // exactly as the sibling rain cases do. rain_streak_builds == 0 after
    // several active frames means the feature never built.
    const IRhiBackend::BackendBuildCounters before = backend.debug_build_counters();
    if (before.rain_streak_builds == 0) {
        std::printf("%s SKIP: rain feature unavailable in this environment "
                    "(precipitation assets did not load; nothing built over %d "
                    "active frames, so no renderables to toggle)\n", kTag, kFrames);
        return CaseResult::Skip;
    }

    const filament::Scene* scene = backend.debug_render_context().scene;
    const filament::View*  view  = backend.debug_render_context().view;
    if (!scene || !view) {
        std::printf("%s FAIL: render context scene/view null after bring-up "
                    "(cannot observe scene membership / view state)\n", kTag);
        return CaseResult::Fail;
    }

    bool ok = true;

    // ── (1)+(2) Rain toggle off→on round-trip  ──
    // The rain renderables that are in the scene == the builders that ran (each
    // builds exactly one renderable and both add to the scene together on the
    // first active frame). Compute the expected rain renderable count from the
    // build counters so the assertion is robust if only the streak built.
    const int expected_rain_renderables =
        (before.rain_streak_builds >= 1 ? 1 : 0) +
        (before.splash_builds      >= 1 ? 1 : 0);

    // Pre-toggle snapshot: rain is built and in the scene.
    const size_t count_before = scene->getRenderableCount();

    // Toggle rain OFF — on_enabled_changed(false) removes ONLY rain_entity_ and
    // splash_entity_ from the scene. No frame is needed: the add/remove
    // is synchronous.
    backend.debug_set_feature_enabled("rain", false);
    const size_t count_off = scene->getRenderableCount();
    const IRhiBackend::BackendBuildCounters after_off = backend.debug_build_counters();

    // Toggle rain ON — on_enabled_changed(true) re-adds the SAME build-once
    // entities.
    backend.debug_set_feature_enabled("rain", true);
    const size_t count_on = scene->getRenderableCount();
    const IRhiBackend::BackendBuildCounters after_on = backend.debug_build_counters();

    // Toggling OFF removed rain's renderables: the count dropped by
    // exactly rain's renderable count, leaving the scene with none of them.
    const long drop = static_cast<long>(count_before) - static_cast<long>(count_off);
    if (drop != expected_rain_renderables) {
        std::printf("%s FAIL: toggling rain OFF changed the scene renderable "
                    "count by %ld (expected a drop of %d — rain's streak+splash "
                    "renderables, and nothing else — R6.7/R7.4)\n",
                    kTag, drop, expected_rain_renderables);
        ok = false;
    }

    // Toggling ON restored rain's renderables: the count returned
    // to EXACTLY its pre-toggle value, so the rise on ON equals the drop on OFF
    // and only rain's renderables moved — NO OTHER feature's scene membership
    // changed.
    const long rise = static_cast<long>(count_on) - static_cast<long>(count_off);
    if (count_on != count_before) {
        std::printf("%s FAIL: rain off→on round-trip left the scene renderable "
                    "count at %zu, not its pre-toggle value %zu — another "
                    "feature's membership changed or rain's renderables were not "
                    "fully restored (R6.7/R7.5/R7.6)\n",
                    kTag, count_on, count_before);
        ok = false;
    }
    if (rise != drop) {
        std::printf("%s FAIL: the OFF drop (%ld) and ON rise (%ld) differ — the "
                    "toggle moved more than rain's own renderables (R6.7)\n",
                    kTag, drop, rise);
        ok = false;
    }

    // Rain's renderables were RESTORED FROM THE BUILD-ONCE BUFFERS,
    // not regenerated: the rain build counters did not change across the toggle
    // off→on cycle — they hold their pre-toggle value of 1.
    if (after_off.rain_streak_builds != before.rain_streak_builds ||
        after_off.splash_builds      != before.splash_builds      ||
        after_on.rain_streak_builds  != before.rain_streak_builds ||
        after_on.splash_builds       != before.splash_builds) {
        std::printf("%s FAIL: a rain build counter changed across the toggle "
                    "(streak %llu→%llu→%llu, splash %llu→%llu→%llu) — the toggle "
                    "must reuse the build-once buffers, not regenerate geometry "
                    "(R6.7/R7.6)\n", kTag,
                    static_cast<unsigned long long>(
                        before.rain_streak_builds),
                    static_cast<unsigned long long>(
                        after_off.rain_streak_builds),
                    static_cast<unsigned long long>(
                        after_on.rain_streak_builds),
                    static_cast<unsigned long long>(before.splash_builds),
                    static_cast<unsigned long long>(after_off.splash_builds),
                    static_cast<unsigned long long>(after_on.splash_builds));
        ok = false;
    }

    // ── (3) Wet-reflections restore leaves shipping values  ──
    // After the active frames the wet-reflections feature has pushed the storm
    // wetness global + retuned SSR. Toggle it OFF: on_enabled_changed(false)
    // restores the wetness global to {0,0,0,1} and SSR to the shipping options.
    backend.debug_set_feature_enabled("wet-reflections", false);

    const filament::math::float4 g = view->getMaterialGlobal(0);
    if (!approx(g.x, 0.0f) || !approx(g.y, 0.0f) || !approx(g.z, 0.0f) ||
        !approx(g.w, 1.0f)) {
        std::printf("%s FAIL: after wet-reflections OFF the wetness material "
                    "global is (%g,%g,%g,%g), expected the shipping default "
                    "(0,0,0,1) — the restore must leave the wetness global at "
                    "its shipping value (R7.4/R9.4)\n",
                    kTag, g.x, g.y, g.z, g.w);
        ok = false;
    }

    // Shipping ScreenSpaceReflectionsOptions (post_process.cpp): enabled=true,
    // thickness=0.2, bias=0.01, maxDistance=25.0, stride=1.5.
    const filament::ScreenSpaceReflectionsOptions ssr =
        view->getScreenSpaceReflectionsOptions();
    if (!ssr.enabled || !approx(ssr.thickness, 0.2f) ||
        !approx(ssr.bias, 0.01f) || !approx(ssr.maxDistance, 25.0f) ||
        !approx(ssr.stride, 1.5f)) {
        std::printf("%s FAIL: after wet-reflections OFF the SSR options are "
                    "(enabled=%d, thickness=%g, bias=%g, maxDistance=%g, "
                    "stride=%g), expected the shipping options (1, 0.2, 0.01, "
                    "25.0, 1.5) — the restore must leave SSR at its shipping "
                    "values (R7.4/7.5/7.6/R9.4)\n",
                    kTag, static_cast<int>(ssr.enabled), ssr.thickness, ssr.bias,
                    ssr.maxDistance, ssr.stride);
        ok = false;
    }

    // ── Restore the shared backend to its as-found state ────────
    // The harness reuses ONE backend across cases, so leave rain and
    // wet-reflections both ENABLED (their default) for subsequent cases. Rain
    // is already back on from the round-trip above; re-enable wet-reflections.
    backend.debug_set_feature_enabled("wet-reflections", true);

    if (!ok) return CaseResult::Fail;

    std::printf("%s toggle isolation / round-trip honored: rain off→on returned "
                "the scene renderable count to %zu (drop=%ld == rise=%ld == "
                "rain's %d renderables; no other feature moved), the rain build "
                "counters held (streak=%llu, splash=%llu — restored from the "
                "build-once buffers), and the wet-reflections restore left the "
                "wetness global at (0,0,0,1) and SSR at the shipping options "
                "(R6.7/R7.4/7.5/7.6/R9.4)\n",
                kTag, count_before, drop, rise, expected_rain_renderables,
                static_cast<unsigned long long>(after_on.rain_streak_builds),
                static_cast<unsigned long long>(after_on.splash_builds));
    return CaseResult::Pass;
}

} // namespace heaven::smoke
