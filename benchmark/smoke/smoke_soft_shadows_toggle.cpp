/*
 * HeavenBench — soft-shadow runtime-toggle / config-path smoke case
 *                     (renderer-linked, GATED)
 *
 * the soft
 * directional-shadow feature is a View_Option_Group — it is NOT an
 * IRenderFeature in render_features_, it is applied through the existing
 * LightingSystem configuration path (configure_soft_shadows, called from
 * LightingSystem::setup). This case asserts that:
 *
 *    the shadow configuration is applied through the LightingSystem /
 *          View-option path, NOT inlined into render_frame — exercised here by
 *          driving LightingSystem::setup() directly and reading the resulting
 *          View shadow state, which is the same per-frame configuration path
 *          render_frame routes through (lighting_->setup(...)).
 *
 *    a runtime flip of FeatureToggles::soft_shadows is re-applied by the
 *          next frame: re-calling setup() with the toggle flipped re-applies the
 *          corresponding View shadow configuration. Enabled ⇒ contact-hardening
 *          PCSS with a penumbra scaled by the soft-shadow Quality_Knob sample
 *          count; disabled ⇒ the Shipping_Look basic PCF mode with Filament's
 *          default SoftShadowOptions (bit-identical to the baseline path).
 *
 *    an out-of-range soft-shadow Quality_Knob level is clamped to the
 *          nearest valid level (and a warning is emitted) by the shared CLI
 *          helper apply_quality_args / clamp_quality_level. The clamp math
 *          itself is unit-tested; here we assert the config-path
 *          consequence — the clamped level is what the configuration path then
 *          applies — and that the clamp is observable (level pulled into [0,2]).
 *
 * WHY THIS LIVES IN THE SMOKE HARNESS (not heaven_benchmark_tests):
 * configure_soft_shadows operates on a real filament::View (it sets the View's
 * ShadowType + SoftShadowOptions). The headless RapidCheck target
 * (heaven_benchmark_tests) deliberately links NEITHER Filament NOR SDL, so it
 * cannot construct an Engine/View. This target links the full renderer, so it
 * can — matching the "GPU smoke tests (gated)" testing strategy.
 *
 * HERMETIC ENGINE: the case stands up its OWN no-op (NOOP) Filament engine +
 * scene + view + camera, drives LightingSystem::setup() against them with the
 * soft-shadow toggle off then on, and reads the View's shadow state back. NOOP
 * needs no GPU/display, and using a private engine means the assertion does not
 * depend on (or perturb) the shared backend the harness brought up. This mirrors
 * smoke_wet_reflections_capability.cpp (the sibling self-contained config-path
 * case), which is also a View-state assertion on a private NOOP engine.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if it cannot create its NOOP engine.
 */

#include "smoke_cases.h"

#include "rhi/lighting_system.h"
#include "rhi/backend_core.h"
#include "rhi/rhi.h"            // LightingParams / lighting_for_time_of_day
#include "engine/types.h"
#include "engine/cli_parser.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Camera.h>
#include <filament/Options.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <cmath>
#include <cstdio>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_soft_shadows_toggle]";

constexpr float kEps = 1e-4f;

[[nodiscard]] bool approx(float a, float b) { return std::fabs(a - b) <= kEps; }

} // namespace

CaseResult run_soft_shadows_toggle_case(const CaseContext& /*ctx*/) {
    // Hermetic NOOP engine: no GPU/display needed, and isolated from the shared
    // backend the harness stood up. If even NOOP can't be created, SKIP so the
    // gated test still passes (exit 0 via the harness).
    filament::Engine* engine =
        filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) {
        std::printf("%s SKIP: could not create a NOOP Filament engine\n", kTag);
        return CaseResult::Skip;
    }

    filament::Scene* scene  = engine->createScene();
    filament::View*  view   = engine->createView();
    view->setScene(scene);

    utils::Entity   cam_entity = utils::EntityManager::get().create();
    filament::Camera* camera   = engine->createCamera(cam_entity);
    view->setCamera(camera);

    bool ok = true;

    {
        // Non-owning core bundle exactly like FilamentBackend hands its
        // subsystems. The renderer is unused by LightingSystem::setup, so a
        // null is fine.
        BackendCore core{};
        core.engine   = engine;
        core.scene    = scene;
        core.view     = view;
        core.renderer = nullptr;
        core.camera   = camera;

        // Daytime sun so shadows are active (castShadows kicks in above the
        // intensity floor used by LightingSystem::setup).
        const LightingParams lighting = lighting_for_time_of_day(12.0f);

        LightingSystem lighting_system(core);
        lighting_system.init_environment(lighting);

        // ──  Out-of-range Quality_Knob clamps via the shared CLI path ──
        // Feed an out-of-range soft-shadow level through apply_quality_args
        // (the same helper main.cpp uses). clamp_quality_level pulls 5 down to
        // the nearest valid level (High = 2) and the helper emits the WARN
        // diagnostic. We assert the observable clamp: the stored level lands in
        // [Low, High] and equals High for this over-range request.
        AppConfig config;
        config.features.soft_shadows = true;
        const char* const argv[] = {"smoke", "--soft-shadow-quality", "5"};
        apply_cli_arguments(config, argv);

        const int level_int = static_cast<int>(config.quality.soft_shadow_level);
        if (level_int < 0 || level_int > 2) {
            std::printf("%s FAIL: out-of-range soft-shadow level was not clamped "
                        "into [0,2] (got %d) — R1.7\n", kTag, level_int);
            ok = false;
        }
        if (config.quality.soft_shadow_level != QualityLevel::High) {
            std::printf("%s FAIL: soft-shadow level '5' should clamp to High (2) "
                        "but is %d — R1.7\n", kTag, level_int);
            ok = false;
        }

        // ──  Disabled ⇒ Shipping_Look basic PCF via the config path ─
        // Run the configuration path with the feature OFF first. This is the
        // baseline / parity state: PCF shadow type + Filament-default
        // SoftShadowOptions.
        config.features.soft_shadows = false;
        lighting_system.setup(lighting, /*sky_sphere_enabled=*/true,
                              config.quality, config.features);

        if (view->getShadowType() != filament::ShadowType::PCF) {
            std::printf("%s FAIL: with soft_shadows OFF the View shadow type is "
                        "not PCF (Shipping_Look parity, R1.4)\n", kTag);
            ok = false;
        }
        {
            const filament::SoftShadowOptions s = view->getSoftShadowOptions();
            if (!approx(s.penumbraScale, 1.0f) ||
                !approx(s.penumbraRatioScale, 1.0f)) {
                std::printf("%s FAIL: with soft_shadows OFF the SoftShadowOptions "
                            "are not Filament defaults (penumbraScale=%g, "
                            "penumbraRatioScale=%g; expected 1.0/1.0) — R1.4\n",
                            kTag, s.penumbraScale, s.penumbraRatioScale);
                ok = false;
            }
        }

        // ──  Toggle ON ⇒ re-applied by the next setup() call ──
        // Flip the toggle and re-run the SAME configuration path (this is what
        // render_frame routes a runtime flip through — lighting_->setup(...)).
        // The View must now be PCSS with the penumbra widened by the soft-shadow
        // Quality_Knob via soft_shadow_penumbra_scale(). The clamp above left the
        // level at High ⇒ penumbraScale = 4.5 (Low 1.5 / Medium 3.0 / High 4.5).
        config.features.soft_shadows = true;
        lighting_system.setup(lighting, /*sky_sphere_enabled=*/true,
                              config.quality, config.features);

        if (view->getShadowType() != filament::ShadowType::PCSS) {
            std::printf("%s FAIL: after toggling soft_shadows ON the View shadow "
                        "type was not re-applied to PCSS by the configuration "
                        "path (R1.1/R1.5/R1.6)\n", kTag);
            ok = false;
        }
        {
            const float expected =
                soft_shadow_penumbra_scale(config.quality.soft_shadow_level);

            const filament::SoftShadowOptions s = view->getSoftShadowOptions();
            if (!approx(s.penumbraScale, expected)) {
                std::printf("%s FAIL: PCSS penumbraScale is %g, expected %g "
                            "(soft_shadow_penumbra_scale(High)) — "
                            "the Quality_Knob did not drive the config (R1.3)\n",
                            kTag, s.penumbraScale, expected);
                ok = false;
            }
        }

        // ──  Toggle back OFF ⇒ re-applied again by the next setup() ──
        // A second flip must land on the next configuration pass too, proving
        // the toggle is honored every frame rather than latched once.
        config.features.soft_shadows = false;
        lighting_system.setup(lighting, /*sky_sphere_enabled=*/true,
                              config.quality, config.features);

        if (view->getShadowType() != filament::ShadowType::PCF) {
            std::printf("%s FAIL: after toggling soft_shadows OFF again the View "
                        "shadow type was not re-applied to PCF by the next "
                        "configuration pass (R1.6)\n", kTag);
            ok = false;
        }

        // LightingSystem destructor (end of scope) frees its sun/moon entities,
        // skybox, IBL and cubemap through `engine` while it is still alive.
    }

    // Tear down our private engine resources (user objects before the engine),
    // after the LightingSystem above has released what it owns.
    engine->destroy(view);
    engine->destroy(scene);
    engine->destroyCameraComponent(cam_entity);
    utils::EntityManager::get().destroy(cam_entity);
    filament::Engine::destroy(&engine);

    if (!ok) return CaseResult::Fail;

    std::printf("%s soft-shadow config-path honored: toggling soft_shadows is "
                "re-applied by the next LightingSystem::setup() pass (OFF⇒PCF "
                "shipping default, ON⇒PCSS with Quality_Knob-scaled penumbra), "
                "and an out-of-range level clamps to the nearest valid level "
                "(R1.5/R1.6/R1.7)\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
