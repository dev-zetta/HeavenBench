#include "smoke_cases.h"
#include "rhi/backend_core.h"
#include "rhi/lighting_system.h"
#include "rhi/sky_atmosphere.h"

#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/Scene.h>
#include <filament/View.h>

#include <cmath>
#include <cstdio>

namespace heaven::smoke {

// Exercise the actual light and sky parameter producers. Native fixed-camera
// images separately qualify the disk, star pixels and shadow appearance.
CaseResult run_night_lighting_case(const CaseContext& /*unused*/) {
    auto* engine = filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) return CaseResult::Skip;
    auto* scene = engine->createScene();
    auto* view = engine->createView();
    view->setScene(scene);
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) std::printf("[smoke_night_lighting] FAIL: %s\n", message);
        ok &= condition;
    };
    {
        BackendCore core{.engine = engine, .scene = scene, .view = view};
        LightingSystem system(core);
        FeatureToggles features;
        features.soft_shadows = true;
        features.contact_shadows = true;
        QualityKnobs quality;
        quality.sun_shadow_map_size = 2048;
        auto& lights = engine->getLightManager();
        const auto night = lighting_for_time_of_day(22.0f);
        const auto sky = make_sky_atmosphere_params(night);
        system.setup(night, true, quality, features);
        int moons = 0;
        scene->forEach([&](utils::Entity entity) {
            const auto light = lights.getInstance(entity);
            if (!light || lights.getType(light) != filament::LightManager::Type::DIRECTIONAL) return;
            ++moons;
            const auto direction = lights.getDirection(light);
            check(glm::length(glm::vec3(direction.x, direction.y, direction.z) +
                      sky.moon_dir) < 1e-6f,
                  "visible Moon and actual light use different directions");
            check(lights.isShadowCaster(light), "Moon remains an unshadowed ambient fill");
            const auto options = lights.getShadowOptions(light);
            check(options.mapSize == 2048 && options.shadowCascades == 4 &&
                      options.screenSpaceContactShadows,
                  "Moon lost user shadow quality/contact choices");
            check(std::abs(options.shadowBulbRadius - std::tan(
                      glm::radians(render_tuning::sky.moon_radius_degrees))) < 1e-7f,
                  "lunar shadow bulb does not match the visible angular radius");
            check(lights.getIntensity(light) > 0.0f && sky.moon_intensity > 0.0f,
                  "above-horizon Moon lacks disk or illumination");
        });
        check(moons == 1, "night does not contain exactly one lunar directional light");
        system.update(lighting_for_time_of_day(18.5f), true, features);
        scene->forEach([&](utils::Entity entity) {
            const auto light = lights.getInstance(entity);
            if (light && lights.getType(light) == filament::LightManager::Type::SUN) {
                check(lights.getIntensity(light) == 0.0f && !lights.isShadowCaster(light),
                      "below-horizon sunset tail suppresses the dominant Moon");
            }
        });
        auto storm_noon = lighting_for_time_of_day(12.0f);
        storm_noon.sun_intensity = 0.0f;
        system.update(storm_noon, true, features);
        int directional = 0;
        scene->forEach([&](utils::Entity entity) {
            const auto light = lights.getInstance(entity);
            if (light && lights.getType(light) == filament::LightManager::Type::DIRECTIONAL) ++directional;
        });
        check(directional == 0 && make_sky_atmosphere_params(storm_noon).moon_intensity == 0.0f,
              "dark daytime weather creates a Moon or lunar light");
        system.update(night, true, features);
        check(scene->getEntityCount() == 2, "night/day/night duplicates celestial entities");
    }
    view->setScene(nullptr);
    engine->destroy(view);
    engine->destroy(scene);
    filament::Engine::destroy(&engine);
    return ok ? CaseResult::Pass : CaseResult::Fail;
}

} // namespace heaven::smoke
