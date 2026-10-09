/*
 * HeavenBench — point-light shadow disabled-parity / live-selection smoke
 *                     case (renderer-linked, GATED)
 *
 * shadow-casting
 * point/spot lights are NOT an IRenderFeature — they are applied through the
 * LightingSystem configuration path (configure_point_light_shadows, called
 * per-frame from render_frame). That path runs the pure
 * select_shadow_casting_lights helper (highest-luminous-intensity-first, ties
 * broken by the stable per-light entity id, budget-bounded) over the
 * backend-owned scene lights handed in via FrameContext::scene_lights, then
 * flips LightManager::setShadowCaster(true/false) on exactly the selected
 * lights. It creates/destroys NO lights — it only toggles shadow casting on
 * lights the scene-light spawner already built.
 *
 * WHAT THIS CASE ASSERTS (the config-path consequence the harness CAN verify,
 * mirroring smoke_contact_shadows.cpp / smoke_soft_shadows_toggle.cpp). Against
 * a hermetic NOOP Filament engine it builds a small set of POINT lights at
 * DISTINCT luminous intensities, wraps them in SceneLightInfo entries pointing
 * at those entities, and drives configure_point_light_shadows:
 *
 *    DISABLED ⇒ select_shadow_casting_lights is fed an effective budget
 *          of 0, so zero lights report LightManager::isShadowCaster — local
 *          lighting is bit-identical to the Shipping_Look (no point/spot light
 *          casts a shadow).
 *
 *    ENABLED with a budget < n ⇒ EXACTLY `budget` lights are shadow
 *          casters, and they are the `budget` HIGHEST-intensity ones; the rest
 *          report isShadowCaster == false. No lights are created or destroyed —
 *          the LightManager component count is identical before and after the
 *          calls (the path only toggles a flag on existing lights).
 *
 * WHY THIS LIVES IN THE SMOKE HARNESS (not heaven_benchmark_tests): the pure
 * select_shadow_casting_lights ranking is exercised by the property
 * test in the headless target. configure_point_light_shadows itself operates on
 * a real filament::LightManager (setShadowCaster / isShadowCaster) built on a
 * filament::Engine. The headless RapidCheck target (heaven_benchmark_tests)
 * deliberately links NEITHER Filament NOR SDL, so it cannot construct an
 * Engine/light. This target links the full renderer, so it can — matching the
 * "GPU smoke tests (gated)" testing strategy.
 *
 * HERMETIC ENGINE: the case stands up its OWN no-op (NOOP) Filament engine +
 * scene, builds the POINT lights itself, drives configure_point_light_shadows
 * via a private LightingSystem, and reads the shadow-caster flags back through
 * the LightManager. NOOP needs no GPU/display, and using a private engine means
 * the assertion does not depend on (or perturb) the shared backend the harness
 * brought up. This mirrors smoke_contact_shadows.cpp (the sibling self-contained
 * config-path case).
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if it cannot create its NOOP engine.
 */

#include "smoke_cases.h"

#include "rhi/lighting_system.h"
#include "rhi/backend_core.h"
#include "rhi/render_feature.h"   // SceneLightInfo
#include "engine/types.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/LightManager.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <cstddef>
#include <cstdio>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_point_light_shadows]";

// Count how many of `lights` currently report LightManager::isShadowCaster.
[[nodiscard]] std::size_t count_shadow_casters(
        filament::LightManager& lm,
        const std::vector<SceneLightInfo>& lights) {
    std::size_t n = 0;
    for (const auto& sl : lights) {
        auto inst = lm.getInstance(sl.entity);
        if (inst && lm.isShadowCaster(inst)) ++n;
    }
    return n;
}

} // namespace

CaseResult run_point_light_shadows_case(const CaseContext& /*ctx*/) {
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

    bool ok = true;

    // Build a small set of POINT lights with DISTINCT intensities so the
    // highest-intensity-first selection has an unambiguous answer. The order in
    // the SceneLightInfo span is deliberately NOT sorted by intensity (the
    // brightest light sits in the middle) so the assertion proves the selection
    // ranks by intensity rather than by span position.

    //   index:        0       1       2       3       4
    //   intensity: 10000   80000   30000   60000   20000
    //   rank:        5th     1st     3rd     2nd     4th

    // With a budget of 2 the expected shadow casters are indices {1, 3}
    // (intensities 80000 and 60000).
    const float intensities[] = {10000.0f, 80000.0f, 30000.0f, 60000.0f, 20000.0f};
    constexpr std::size_t kN = sizeof(intensities) / sizeof(intensities[0]);

    std::vector<utils::Entity>   entities;
    std::vector<SceneLightInfo>  lights;
    entities.reserve(kN);
    lights.reserve(kN);

    filament::LightManager& lm = engine->getLightManager();

    for (std::size_t i = 0; i < kN; ++i) {
        utils::Entity e = utils::EntityManager::get().create();
        filament::LightManager::Builder(filament::LightManager::Type::POINT)
            .position({static_cast<float>(i), 0.0f, 0.0f})
            .color({1.0f, 0.85f, 0.6f})
            .intensity(intensities[i])
            .falloff(6.0f)
            .castShadows(false)   // start as non-casters (the pre-config state)
            .build(*engine, e);
        scene->addEntity(e);

        entities.push_back(e);

        SceneLightInfo info{};
        info.entity    = e;
        info.intensity = intensities[i];
        lights.push_back(info);
    }

    // Record the LightManager component count up front: the configuration path
    // must create/destroy no lights, so this stays constant.
    const std::size_t components_before = lm.getComponentCount();

    {
        // Non-owning core bundle exactly like FilamentBackend hands its
        // subsystems. Only the engine is needed for configure_point_light_shadows.
        BackendCore core{};
        core.engine   = engine;
        core.scene    = scene;
        core.view     = nullptr;
        core.renderer = nullptr;
        core.camera   = nullptr;

        LightingSystem lighting_system(core);

        const std::span<const SceneLightInfo> light_span{lights.data(), lights.size()};

        // ──  DISABLED ⇒ zero shadow casters (Shipping_Look parity) ─────
        // Enabled=false treats the budget as 0, so the selection is empty and
        // every candidate gets setShadowCaster(false). Pass a non-zero budget to
        // prove the disabled flag — not the budget — is what zeroes the casters.
        lighting_system.configure_point_light_shadows(
            light_span, /*budget=*/3, /*map_size=*/256, /*enabled=*/false);

        const std::size_t casters_disabled = count_shadow_casters(lm, lights);
        if (casters_disabled != 0) {
            std::printf("%s FAIL: with the feature DISABLED %zu of %zu point "
                        "lights report isShadowCaster; expected 0 "
                        "(Shipping_Look-equivalent local lighting, R3.5)\n",
                        kTag, casters_disabled, kN);
            ok = false;
        }

        // ──  ENABLED, budget < n ⇒ exactly `budget` highest-intensity ──
        constexpr int kBudget = 2;
        lighting_system.configure_point_light_shadows(
            light_span, kBudget, /*map_size=*/256, /*enabled=*/true);

        const std::size_t casters_enabled = count_shadow_casters(lm, lights);
        if (casters_enabled != static_cast<std::size_t>(kBudget)) {
            std::printf("%s FAIL: with budget=%d (< n=%zu) %zu lights cast "
                        "shadows; expected exactly %d (R3.6)\n",
                        kTag, kBudget, kN, casters_enabled, kBudget);
            ok = false;
        }

        // The two casters must be the two HIGHEST-intensity lights: indices 1
        // (80000) and 3 (60000). Every other light must NOT cast.
        for (std::size_t i = 0; i < kN; ++i) {
            const bool expected_cast = (i == 1 || i == 3);
            auto inst = lm.getInstance(lights[i].entity);
            const bool actual_cast = inst && lm.isShadowCaster(inst);
            if (actual_cast != expected_cast) {
                std::printf("%s FAIL: light %zu (intensity %g) isShadowCaster=%d "
                            "but expected %d — selection is not highest-"
                            "intensity-first (R3.6)\n",
                            kTag, i, intensities[i], actual_cast, expected_cast);
                ok = false;
            }
        }

        // No lights created or destroyed by either call: the
        // LightManager component count is unchanged.
        const std::size_t components_after = lm.getComponentCount();
        if (components_after != components_before) {
            std::printf("%s FAIL: LightManager component count changed from %zu "
                        "to %zu — configure_point_light_shadows created/destroyed "
                        "lights instead of only toggling shadow casting (R3.6)\n",
                        kTag, components_before, components_after);
            ok = false;
        }

        // ── (again) Toggle back DISABLED ⇒ casters return to zero ───────
        // A second flip must land too, proving the toggle is honored every frame
        // rather than latched once.
        lighting_system.configure_point_light_shadows(
            light_span, kBudget, /*map_size=*/256, /*enabled=*/false);
        const std::size_t casters_redisabled = count_shadow_casters(lm, lights);
        if (casters_redisabled != 0) {
            std::printf("%s FAIL: after re-disabling, %zu lights still cast "
                        "shadows; expected 0 (R3.5)\n",
                        kTag, casters_redisabled);
            ok = false;
        }
    }

    // Tear down our private lights + engine (user objects before the engine).
    for (utils::Entity e : entities) {
        scene->remove(e);
        engine->destroy(e);
        utils::EntityManager::get().destroy(e);
    }
    engine->destroy(scene);
    filament::Engine::destroy(&engine);

    if (!ok) return CaseResult::Fail;

    std::printf("%s point-light shadow config-path honored: DISABLED ⇒ zero "
                "shadow casters (Shipping_Look parity, R3.5); ENABLED with "
                "budget < n ⇒ exactly the budgeted highest-intensity lights cast "
                "and no lights were created/destroyed (R3.6). The pure "
                "highest-intensity-first ranking is owned by the Property 6 "
                "property test.\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
