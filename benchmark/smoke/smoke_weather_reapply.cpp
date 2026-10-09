/*
 * HeavenBench — weather re-application discipline smoke case
 *                    (renderer-linked, GATED)
 *
 * the
 * weather-dependent Filament state (lighting/exposure + distance fog +
 * post-process) is applied ONCE on the first frame — seeding prev_weather_ —
 * and re-applied by render_frame ONLY when the per-frame WeatherState differs
 * from the last successfully-applied one (by its derived-output signature). An
 * UNCHANGED WeatherState triggers NO re-application, so a run that stays at the
 * Shipping_Look default never re-applies after the first frame, while a
 * genuine runtime change (rain starting) re-applies exactly once on the frame it
 * changes.
 *
 * The backend exposes this through the debug_build_counters().weather_reapplies
 * counter — bumped only inside the per-frame change block on a successful
 * re-apply, never by the first-frame baseline seed. This mirrors
 * smoke_rain_build_once.cpp's build-counter discipline (a builder reaches its
 * target once and never increments again) and smoke_perframe_discipline.cpp's
 * delta reasoning (the harness shares ONE backend across cases, so the counter
 * is cumulative — an earlier case already seeded the baseline and may have
 * changed the weather, so this case reasons in DELTAS around its own frames).
 *
 * Flow:
 *   1. Render several frames at an UNCHANGED WeatherState  ⇒ weather_reapplies
 *      delta == 0.
 *   2. Render one frame at a CHANGED WeatherState (rain intensity 1.0) ⇒ the
 *      counter advances by exactly 1 — the change path is reachable after the
 *      baseline seed.
 *   3. Hold that changed state for several more frames ⇒ no further advance
 *      (steady state re-applies nothing).
 *   4. Restore the default WeatherState before returning so the shared backend
 *      is left as found for later cases.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if init_pipelines fails, tolerating a
 * headless/driver-less environment that passed the gate.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "engine/types.h"
#include "engine/camera.h"

#include <cstdio>
#include <span>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_weather_reapply]";

// Render exactly one frame with an empty scene and the supplied WeatherState.
// Mirrors the empty-frame helper in the sibling cases: the backend tolerates
// empty instance/mesh spans, so this drives the full per-frame path — including
// the weather change check — without loading the heavy Heaven scene.
void render_one_frame(IRhiBackend& backend, const WeatherState& weather,
                      const Camera& camera, const LightingParams& lighting,
                      const TonemapParams& tonemap, int w, int h) {
    MaterialResolver resolver =
        [](const std::string&, const std::string&) -> GpuMaterial {
            return GpuMaterial{};
        };
    backend.render_frame(std::span<const Instance>{},
                         std::span<const int>{},
                         std::span<const GpuMesh>{},
                         resolver, camera, lighting, weather, tonemap, w, h);
}

} // namespace

CaseResult run_weather_reapply_case(const CaseContext& ctx) {
    IRhiBackend& backend = ctx.backend;

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

    bool ok = true;

    // ── 1. Unchanged WeatherState ⇒ no re-application  ────
    // The default WeatherState is the Shipping_Look baseline (intensity 0). An
    // earlier case has already seeded prev_weather_ with this same state, so
    // these frames find weather == prev_weather_ and skip the re-apply block.
    const WeatherState steady_default;
    const uint64_t baseline = backend.debug_build_counters().weather_reapplies;

    constexpr int kSteadyFrames = 4;
    for (int i = 0; i < kSteadyFrames; ++i) {
        render_one_frame(backend, steady_default, camera, lighting, tonemap, w, h);
    }
    const uint64_t after_steady = backend.debug_build_counters().weather_reapplies;
    if (after_steady != baseline) {
        std::printf("%s FAIL: weather_reapplies advanced by %llu over %d "
                    "unchanged-WeatherState frames, expected 0 — an unchanged "
                    "state must trigger no re-application (R11.5)\n", kTag,
                    static_cast<unsigned long long>(after_steady - baseline),
                    kSteadyFrames);
        ok = false;
    }

    // ── 2. Changed WeatherState ⇒ exactly one re-application  ──
    // Rain starting at full intensity changes effective_intensity / fog_density
    // wetness / sun_scale, so the signature differs and the per-frame change
    // path re-applies exactly once on this frame.
    WeatherState changed;
    changed.set_intensity(1.0f);
    render_one_frame(backend, changed, camera, lighting, tonemap, w, h);
    const uint64_t after_change = backend.debug_build_counters().weather_reapplies;
    if (after_change != after_steady + 1) {
        std::printf("%s FAIL: a changed WeatherState advanced weather_reapplies "
                    "by %llu, expected exactly 1 — the per-frame change path "
                    "must re-apply once on the frame the state changes "
                    "(R11.4)\n", kTag,
                    static_cast<unsigned long long>(
                        after_change - after_steady));
        ok = false;
    }

    // ── 3. Hold the changed state ⇒ no further re-application  ──
    constexpr int kHoldFrames = 4;
    for (int i = 0; i < kHoldFrames; ++i) {
        render_one_frame(backend, changed, camera, lighting, tonemap, w, h);
    }
    const uint64_t after_hold = backend.debug_build_counters().weather_reapplies;
    if (after_hold != after_change) {
        std::printf("%s FAIL: holding the changed WeatherState advanced "
                    "weather_reapplies by %llu over %d frames, expected 0 — "
                    "steady state must re-apply nothing (R11.5)\n", kTag,
                    static_cast<unsigned long long>(after_hold - after_change),
                    kHoldFrames);
        ok = false;
    }

    // ── 4. Restore the default so the shared backend is left as found ──
    // (This is itself a change back to baseline and bumps the counter once;
    // later cases do not assert the weather counter, so that is fine.)
    render_one_frame(backend, steady_default, camera, lighting, tonemap, w, h);

    if (!ok) return CaseResult::Fail;

    std::printf("%s weather re-application discipline honored: %d unchanged "
                "frames ⇒ 0 re-applies (R11.5); a rain-start change ⇒ exactly 1 "
                "re-apply (R11.4); %d held frames ⇒ 0 further re-applies "
                "(R11.5); default restored\n", kTag, kSteadyFrames, kHoldFrames);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
