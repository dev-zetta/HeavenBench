/*
 * HeavenBench — wet-reflections capability-unavailable smoke case
 *                     (renderer-linked, GATED)
 *
 * IF a selected technique requires a Vulkan
 * extension / GPU capability that is unavailable at initialisation, THEN the
 * renderer SHALL initialise that technique's Render_Feature DISABLED, continue
 * rendering the remaining scene without it, and emit a diagnostic naming the
 * disabled technique — leaving the view's screen-space-reflections options and
 * the wetness material global UNTOUCHED so the Shipping_Look is preserved
 *
 *
 * WHY THIS LIVES IN THE SMOKE HARNESS (not heaven_benchmark_tests):
 * WetReflectionsFeature operates on a real filament::View (it sets/reads SSR
 * options and material global 0) and its capability gate probes a real
 * filament::Engine feature level. The headless RapidCheck target
 * (heaven_benchmark_tests) deliberately links NEITHER Filament NOR SDL, so it
 * cannot construct an Engine/View. This target links the full renderer, so it
 * can — matching the "GPU smoke tests (gated)" testing strategy.
 *
 * HOW WE "FORCE THE CAPABILITY PROBE TO FAIL" deterministically without a GPU
 * dependency: WetReflectionsFeature exposes the decision through a tiny
 * protected virtual seam, `probe_capability(filament::Engine&)`, whose default
 * is the production feature-level check (FEATURE_LEVEL_1). On a real desktop
 * backend that always returns true, so the unavailable branch can never be hit
 * naturally. This case subclasses the feature and overrides the seam to return
 * false, exercising the real disabled-path branch in init().
 *
 * HERMETIC ENGINE: the case stands up its OWN no-op (NOOP) Filament engine +
 * view, sets a distinctive "shipping" SSR state and material global on it, runs
 * the forced-unavailable feature's init() against it, and asserts the feature
 * came up disabled and the view state is bit-identical to the sentinel. NOOP
 * needs no GPU/display, and using a private engine means the assertion does not
 * depend on (or perturb) the shared backend the harness brought up.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if it cannot create its NOOP engine.
 */

#include "smoke_cases.h"

#include "rhi/render_feature.h"
#include "rhi/features/wet_reflections_feature.h"
#include "engine/types.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Options.h>

#include <math/vec4.h>

#include <cstdio>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_wet_reflections_capability]";

// Test-only subclass that forces the capability probe to report the
// capability as UNAVAILABLE, regardless of what the engine actually supports.
// This is the whole point of the protected virtual seam: it lets the disabled
// branch be exercised on hardware that would otherwise always be capable.
class ForcedUnavailableWetReflections final : public WetReflectionsFeature {
protected:
    bool probe_capability(filament::Engine& /*engine*/) const override {
        return false;
    }
};

class ForcedAvailableWetReflections final : public WetReflectionsFeature {
protected:
    bool probe_capability(filament::Engine& /*engine*/) const override {
        return true;
    }
};

// ScreenSpaceReflectionsOptions has no operator==; compare every field so any
// write by the (disabled) feature is caught.
bool ssr_equal(const filament::ScreenSpaceReflectionsOptions& a,
               const filament::ScreenSpaceReflectionsOptions& b) {
    return a.thickness == b.thickness &&
           a.bias == b.bias &&
           a.maxDistance == b.maxDistance &&
           a.stride == b.stride &&
           a.enabled == b.enabled;
}

} // namespace

CaseResult run_wet_reflections_capability_case(const CaseContext& /*ctx*/) {
    // Hermetic NOOP engine: no GPU/display needed, and isolated from the shared
    // backend the harness stood up. If even NOOP can't be created, SKIP so the
    // gated test still passes (exit 0 via the harness).
    filament::Engine* engine =
        filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) {
        std::printf("%s SKIP: could not create a NOOP Filament engine\n", kTag);
        return CaseResult::Skip;
    }

    filament::Scene* scene = engine->createScene();
    filament::View*  view  = engine->createView();
    view->setScene(scene);

    // ── Sentinel "Shipping_Look" view state the disabled feature must NOT
    // touch. Distinctive non-default values so any write is observable. ──
    filament::ScreenSpaceReflectionsOptions sentinel_ssr{};
    sentinel_ssr.enabled     = true;
    sentinel_ssr.thickness   = 0.37f;
    sentinel_ssr.bias        = 0.027f;
    sentinel_ssr.maxDistance = 13.5f;
    sentinel_ssr.stride      = 2.75f;
    view->setScreenSpaceReflectionsOptions(sentinel_ssr);

    const filament::math::float4 sentinel_global{0.11f, 0.22f, 0.33f, 0.44f};
    view->setMaterialGlobal(0, sentinel_global);

    // RenderContext pointing at our private engine/view (the only state the
    // feature's init() reads/writes).
    RenderContext rctx{};
    rctx.engine = engine;
    rctx.scene  = scene;
    rctx.view   = view;

    // features.wet_reflections defaults true, so the ONLY thing that can
    // disable the feature here is the forced-failing capability gate — which
    // is exactly what we are validating.
    AppConfig config;

    // init() prints the diagnostic
    // ("Wet reflections: required GPU capability unavailable — technique
    //  disabled") on the forced-unavailable path; the harness does not capture
    // stdout, so we assert the observable disabled state + untouched view below
    // (the diagnostic itself is covered by code inspection).
    ForcedUnavailableWetReflections feature;
    feature.init(rctx, config);

    bool ok = true;

    // The feature initialised DISABLED because the probe failed.
    if (feature.enabled()) {
        std::printf("%s FAIL: feature is enabled after a forced-unavailable "
                    "capability probe (expected disabled)\n", kTag);
        ok = false;
    }

    // The view's SSR options are UNTOUCHED — the feature did not call
    // setScreenSpaceReflectionsOptions on the disabled path.
    const filament::ScreenSpaceReflectionsOptions after_ssr =
        view->getScreenSpaceReflectionsOptions();
    if (!ssr_equal(after_ssr, sentinel_ssr)) {
        std::printf("%s FAIL: SSR options changed by a disabled feature "
                    "(maxDistance %.3f->%.3f, stride %.3f->%.3f, enabled %d->%d)\n",
                    kTag, sentinel_ssr.maxDistance, after_ssr.maxDistance,
                    sentinel_ssr.stride, after_ssr.stride,
                    static_cast<int>(sentinel_ssr.enabled),
                    static_cast<int>(after_ssr.enabled));
        ok = false;
    }

    // Material global 0 (the wetness term read by the lit shaders) is
    // UNTOUCHED — still the sentinel, not the feature's {0,0,0,1} restore or a
    // wetness vector.
    const filament::math::float4 after_global = view->getMaterialGlobal(0);
    if (after_global.x != sentinel_global.x ||
        after_global.y != sentinel_global.y ||
        after_global.z != sentinel_global.z ||
        after_global.w != sentinel_global.w) {
        std::printf("%s FAIL: material global 0 changed by a disabled feature "
                    "(%.2f,%.2f,%.2f,%.2f -> %.2f,%.2f,%.2f,%.2f)\n", kTag,
                    sentinel_global.x, sentinel_global.y,
                    sentinel_global.z, sentinel_global.w,
                    after_global.x, after_global.y,
                    after_global.z, after_global.w);
        ok = false;
    }

    // Wet-surface shading may retune SSR, but must preserve the independent
    // reflections toggle during dry weather, rain, live toggles and teardown.
    for (bool reflections : {false, true}) {
        config.features.reflections = reflections;
        auto base_ssr = view->getScreenSpaceReflectionsOptions();
        base_ssr.enabled = reflections;
        view->setScreenSpaceReflectionsOptions(base_ssr);
        ForcedAvailableWetReflections available;
        available.init(rctx, config);
        WeatherState weather;
        FrameContext frame;
        frame.weather = &weather;
        const auto check_enabled = [&] {
            if (view->getScreenSpaceReflectionsOptions().enabled != reflections) {
                std::printf("%s FAIL: wet-surface lifecycle overrode reflections=%d\n",
                            kTag, static_cast<int>(reflections));
                ok = false;
            }
        };
        available.update(rctx, frame);
        check_enabled();
        weather.set_intensity(1.0f);
        weather.advance(10.0f);
        available.update(rctx, frame);
        check_enabled();
        if (view->getMaterialGlobal(0).z <= 0.0f) {
            std::printf("%s FAIL: disabling SSR also disabled wet shading\n", kTag);
            ok = false;
        }
        // A live SSR edit changes both the wet retune and the dry/off restore
        // baseline; it must not change whether surfaces are still wet.
        reflections = !reflections;
        available.set_reflections_enabled(reflections);
        available.update(rctx, frame);
        check_enabled();
        const WeatherState dry_weather;
        frame.weather = &dry_weather;
        available.update(rctx, frame);
        check_enabled();
        frame.weather = &weather;
        available.set_enabled(rctx, false);
        check_enabled();
        available.set_enabled(rctx, true);
        available.update(rctx, frame);
        check_enabled();
        available.destroy(rctx);
        check_enabled();
    }

    // Tear down our private engine resources (user objects before the engine).
    engine->destroy(view);
    engine->destroy(scene);
    filament::Engine::destroy(&engine);

    if (!ok) return CaseResult::Fail;

    std::printf("%s forced-unavailable probe ⇒ feature disabled; SSR options "
                "and material global 0 left untouched (Shipping_Look preserved)\n",
                kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
