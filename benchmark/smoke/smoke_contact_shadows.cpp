// Contact-shadow configuration regression. A private NOOP engine checks the
// real LightingSystem off/on/off path and the quality-controlled ray count.
// The small synthetic byte arrays below only test the pixel-difference helper;
// they do not establish rendered shadow presence or original-engine parity.
// smoke_contact_shadow_stability.cpp separately reads actual GPU pixels from a
// sloped receiver and a nearby occluder, and the full-scene frame sequences in
// docs/VISUAL_AUDIT.md cover the reported village flicker and moving geometry.

#include "smoke_cases.h"

#include "rhi/lighting_system.h"
#include "rhi/backend_core.h"
#include "rhi/rhi.h"            // LightingParams / lighting_for_time_of_day
#include "engine/types.h"
#include "benchmark/pixel_parity.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Camera.h>
#include <filament/LightManager.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_contact_shadows]";

// Find the directional SUN light in the scene and return its ShadowOptions plus
// whether screen-space contact shadows are enabled on it. `found` is false if
// no SUN light is present (the scene has none). LightingSystem builds exactly
// one SUN, so the first match is authoritative.
struct SunShadowState {
    bool                                 found = false;
    bool                                 contact_enabled = false;
    uint8_t                              step_count = 0;
};

SunShadowState read_sun_shadow_state(filament::Engine& engine,
                                     filament::Scene& scene) {
    filament::LightManager& lm = engine.getLightManager();
    SunShadowState state;
    scene.forEach([&](utils::Entity e) {
        if (state.found) return;
        if (!lm.hasComponent(e)) return;
        auto inst = lm.getInstance(e);
        if (!inst) return;
        if (lm.getType(inst) != filament::LightManager::Type::SUN) return;
        const filament::LightManager::ShadowOptions& opts =
            lm.getShadowOptions(inst);
        state.found            = true;
        state.contact_enabled  = opts.screenSpaceContactShadows;
        state.step_count       = opts.stepCount;
    });
    return state;
}

// Exercise the shared pixel-difference helper on synthetic buffers. Actual
// rendered shadow presence is checked by the separate GPU stability case.
bool exercise_diff_helper() {
    constexpr std::size_t kW = 8, kH = 8;
    // "Shipping_Look" reference: a flat lit receiving surface (no contact line).
    std::vector<std::uint8_t> shipping(kW * kH * 4, 200);
    for (std::size_t px = 0; px < kW * kH; ++px) shipping[px * 4 + 3] = 255;

    bool ok = true;

    // Contact shadows DISABLED ⇒ output is bit-identical to the Shipping_Look
    // reference ⇒ within tolerance, max delta 0 (parity).
    {
        std::vector<std::uint8_t> disabled = shipping;
        const PixelDiff d = compare_rgba8(disabled, shipping, kW, kH);
        if (!within_shipping_look_tolerance(d) || d.max_channel_delta != 0) {
            std::printf("%s FAIL: disabled-parity buffers not within tolerance "
                        "(size_match=%d max_delta=%d) — R2.4\n", kTag,
                        d.size_match, d.max_channel_delta);
            ok = false;
        }
    }

    // Contact shadows ENABLED ⇒ a darkened contact line appears: one row of
    // pixels pulled well below the surface tone. The diff must exceed the
    // 1-of-255 tolerance, i.e. the contact line is DETECTABLY present and absent
    // in the Shipping_Look.
    {
        std::vector<std::uint8_t> enabled = shipping;
        for (std::size_t x = 0; x < kW; ++x) {
            const std::size_t base = (3 * kW + x) * 4;  // one "contact" row
            enabled[base + 0] = 60;   // darkened RGB at the contact seam
            enabled[base + 1] = 60;
            enabled[base + 2] = 60;
        }
        const PixelDiff d = compare_rgba8(enabled, shipping, kW, kH);
        if (within_shipping_look_tolerance(d) || d.max_channel_delta <= 1) {
            std::printf("%s FAIL: a darkened contact line was treated as within "
                        "Shipping_Look tolerance (max_delta=%d within=%d) — the "
                        "presence diff (R2.2) would not be detected\n", kTag,
                        d.max_channel_delta, within_shipping_look_tolerance(d));
            ok = false;
        }
    }

    return ok;
}

} // namespace

CaseResult run_contact_shadows_case(const CaseContext& /*ctx*/) {
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

    utils::Entity     cam_entity = utils::EntityManager::get().create();
    filament::Camera* camera     = engine->createCamera(cam_entity);
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
        // intensity floor used by LightingSystem::setup, where contact shadows
        // matter).
        const LightingParams lighting = lighting_for_time_of_day(12.0f);

        LightingSystem lighting_system(core);
        lighting_system.init_environment(lighting);

        AppConfig config;

        // ──  Disabled ⇒ screenSpaceContactShadows = false (parity) ─────
        // Run the configuration path with the feature OFF first. This is the
        // baseline / Shipping_Look state the 1-of-255 directional-lighting bar
        // is defined over.
        config.features.contact_shadows = false;
        lighting_system.setup(lighting, /*sky_sphere_enabled=*/true,
                              config.quality, config.features);

        SunShadowState off = read_sun_shadow_state(*engine, *scene);
        if (!off.found) {
            std::printf("%s FAIL: no SUN light found in the scene after setup() "
                        "— cannot assert contact-shadow state\n", kTag);
            ok = false;
        } else if (off.contact_enabled) {
            std::printf("%s FAIL: with contact_shadows OFF the sun's "
                        "ShadowOptions.screenSpaceContactShadows is true "
                        "(Shipping_Look parity, R2.4)\n", kTag);
            ok = false;
        }

        // ──  Enabled ⇒ re-applied by the next setup() pass ───
        // Flip the toggle and re-run the SAME configuration path (this is what
        // render_frame routes a runtime flip through — lighting_->setup(...)).
        // The sun must now report screenSpaceContactShadows = true with the
        // stepCount driven by the contact-shadow Quality_Knob.
        config.features.contact_shadows = true;
        lighting_system.setup(lighting, /*sky_sphere_enabled=*/true,
                              config.quality, config.features);

        SunShadowState on = read_sun_shadow_state(*engine, *scene);
        const int expected_steps =
            contact_shadow_steps(config.quality.contact_shadow_level,
                                 config.quality);
        if (!on.found) {
            std::printf("%s FAIL: no SUN light found in the scene after enabling "
                        "contact shadows\n", kTag);
            ok = false;
        } else {
            if (!on.contact_enabled) {
                std::printf("%s FAIL: with contact_shadows ON the sun's "
                            "screenSpaceContactShadows was not re-applied to "
                            "true by the configuration path (R2.1/R2.5)\n", kTag);
                ok = false;
            }
            if (on.step_count != static_cast<uint8_t>(expected_steps)) {
                std::printf("%s FAIL: contact-shadow stepCount is %u, expected %d "
                            "(contact_shadow_steps(Medium)) — the Quality_Knob "
                            "did not drive the config (R2.3)\n", kTag,
                            on.step_count, expected_steps);
                ok = false;
            }
        }

        // ──  Toggle back OFF ⇒ re-applied again by the next setup() ────
        // A second flip must land on the next configuration pass too, proving
        // the toggle is honored every frame rather than latched once.
        config.features.contact_shadows = false;
        lighting_system.setup(lighting, /*sky_sphere_enabled=*/true,
                              config.quality, config.features);

        SunShadowState off2 = read_sun_shadow_state(*engine, *scene);
        if (off2.found && off2.contact_enabled) {
            std::printf("%s FAIL: after toggling contact_shadows OFF again the "
                        "sun's screenSpaceContactShadows was not re-applied to "
                        "false by the next configuration pass (R2.5)\n", kTag);
            ok = false;
        }

        // LightingSystem destructor (end of scope) frees its sun/moon entities,
        // skybox, IBL and cubemap through `engine` while it is still alive.
    }

    // This exercises the diff helper only. The separate stability case checks
    // actual GPU shadow presence; these synthetic arrays do not.
    if (!exercise_diff_helper()) ok = false;

    // Tear down our private engine resources (user objects before the engine),
    // after the LightingSystem above has released what it owns.
    engine->destroy(view);
    engine->destroy(scene);
    engine->destroyCameraComponent(cam_entity);
    utils::EntityManager::get().destroy(cam_entity);
    filament::Engine::destroy(&engine);

    if (!ok) return CaseResult::Fail;

    std::printf("%s contact-shadow config-path honored: toggling "
                "contact_shadows is re-applied by the next LightingSystem::setup()"
                " pass (OFF⇒screenSpaceContactShadows=false Shipping_Look parity, "
                "ON⇒true with the Quality_Knob-driven stepCount), and the live "
                "pixel-diff helper detects the synthetic darkened row. Actual "
                "GPU shadow presence and temporal stability are checked by "
                "contact_shadow_stability; full-scene review is separate.\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
