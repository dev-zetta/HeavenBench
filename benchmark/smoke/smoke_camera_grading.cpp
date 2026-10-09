/*
 * HeavenBench — camera and grading configuration smoke case.
 *
 * An isolated NOOP Filament engine verifies color-grading construction,
 * post-processing View options, and preservation of camera-owned DOF state
 * through weather reconfiguration. Pure photometric checks cover exposure
 * and aperture direction. NOOP produces no pixels: these checks do not assess
 * full-scene visual parity, and production --benchmark does not compare
 * reference images. Diagnostic --capture / --camera-tour-time / --camera-pose
 * and scripts/visual_audit.py provide real images for separate visual review.
 */

#include "smoke_cases.h"

#include "rhi/post_process.h"   // configure_post_processing
#include "rhi/depth_prepass.h"
#include "rhi/filament_backend.h"
#include "rhi/rhi.h"            // TonemapParams
#include "render_tuning.h"
#include "engine/types.h"       // ToneMapperSelection / ColorGradingConfig /
                                // PhysicalCameraConfig / exposure_value / dof_metric

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/ColorGrading.h>
#include <filament/Camera.h>
#include <filament/LightManager.h>
#include <filament/IndirectLight.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_camera_grading]";

// A non-neutral grade: shifted away from every neutral default so the chain
// genuinely applies different controls (the "differs from neutral"
// precondition). Cool white balance, lifted contrast/saturation, and a
// non-identity ASC CDL slope.
ColorGradingConfig make_non_neutral_grade() {
    ColorGradingConfig g;
    g.temperature = -0.5f;            // cool / blue shift (neutral 0)
    g.tint        =  0.2f;            // (neutral 0)
    g.contrast    =  1.4f;            // (neutral 1)
    g.saturation  =  1.3f;            // (neutral 1)
    g.cdl_slope   = glm::vec3(1.1f);  // (neutral 1,1,1)
    g.cdl_offset  = glm::vec3(0.02f); // (neutral 0,0,0)
    return g;
}

// True iff the grade differs from the neutral ColorGradingConfig default on at
// least one control the chain reads (the host-level precondition for the
// >1/255 pixel delta requiring separate capture review).
bool grade_differs_from_neutral(const ColorGradingConfig& g) {
    const ColorGradingConfig neutral{};
    return g.temperature != neutral.temperature ||
           g.tint        != neutral.tint        ||
           g.contrast    != neutral.contrast    ||
           g.saturation  != neutral.saturation  ||
           g.cdl_slope   != neutral.cdl_slope   ||
           g.cdl_offset  != neutral.cdl_offset  ||
           g.cdl_power   != neutral.cdl_power    ||
           g.night_adaptation != neutral.night_adaptation;
}

bool check_color_grading_cache(filament::Engine& engine) {
    auto* view = engine.createView();
    if (!view) {
        std::printf("%s FAIL: cache fixture view unavailable\n", kTag);
        return false;
    }
    const auto resource_count = engine.getColorGradingCount();
    ColorGradingCache cache;
    filament::ColorGrading* owned = nullptr;
    TonemapParams tonemap;
    QualityKnobs quality;
    FeatureToggles features;
    ColorGradingConfig grading;
    bool hdr = false;
    bool grading_enabled = false;
    bool dynamic_resolution = false;
    bool msaa = false;
    bool ssr = false;
    float dynamic_scale = 1.0f;
    float fog_density = -1.0f;
    auto mapper = ToneMapperSelection::ACES;
    bool ok = true;
    const auto require = [&](bool condition, const char* label) {
        if (!condition) {
            std::printf("%s FAIL: color-grading cache %s\n", kTag, label);
            ok = false;
        }
    };
    const auto apply = [&](bool rebuild, const char* label) {
        const auto* previous = owned;
        const auto builds = cache.builds;
        owned = configure_post_processing(engine, *view, tonemap, owned,
            fog_density, quality, msaa, ssr, hdr, mapper, grading_enabled,
            grading, dynamic_resolution, dynamic_scale, features, &cache);
        // Addresses can be recycled on replacement; the resource count and
        // explicit build count distinguish rebuilding from pointer reuse.
        require(owned && engine.isValid(owned) && view->getColorGrading() == owned &&
            cache.object == owned && cache.inputs.has_value() &&
            engine.getColorGradingCount() == resource_count + 1 &&
            cache.builds == builds + (rebuild ? 1u : 0u) &&
            (rebuild || owned == previous), label);
    };
    apply(true, "initial resource construction");

    // Weather and quality changes still update the View on a cache hit.
    // Reset camera-owned DOF before each refresh, as the real camera does.
    for (unsigned step = 0; step < 4; ++step) {
        const bool enabled = (step & 1u) != 0;
        tonemap.bloom_strength = 0.15f + static_cast<float>(step) * 0.1f;
        features.bloom = enabled;
        features.ambient_occlusion = enabled;
        features.taa = enabled;
        features.depth_of_field = true;
        features.contact_shadows = enabled;
        quality.ao_quality = enabled ? QualityLevel::High : QualityLevel::Low;
        quality.dof_quality = enabled ? QualityLevel::High : QualityLevel::Low;
        quality.bloom_quality = enabled ? QualityLevel::High : QualityLevel::Low;
        quality.msaa_samples = 4;
        dynamic_resolution = enabled;
        // Equal min/max allows this toggle even on a NOOP driver without
        // dynamic-resolution timing support.
        dynamic_scale = 1.0f;
        msaa = enabled;
        ssr = enabled;
        fog_density = enabled ? 0.001f + static_cast<float>(step) * 0.0001f : -1.0f;
        filament::DepthOfFieldOptions camera_dof;
        camera_dof.enabled = step != 2;
        camera_dof.cocScale = 2.75f + static_cast<float>(step);
        view->setDepthOfFieldOptions(camera_dof);
        apply(false, "irrelevant weather/view inputs rebuilt the LUT");
        const auto fog = view->getFogOptions();
        const auto bloom = view->getBloomOptions();
        const auto ao = view->getAmbientOcclusionOptions();
        const auto dof = view->getDepthOfFieldOptions();
        const auto samples = view->getMultiSampleAntiAliasingOptions();
        require(fog.enabled == enabled && (!enabled || fog.density == fog_density) &&
            bloom.enabled == enabled && bloom.strength == tonemap.bloom_strength &&
            ao.enabled == enabled && ao.resolution == (enabled ? 1.0f : 0.5f) &&
            view->getTemporalAntiAliasingOptions().enabled == enabled &&
            view->getScreenSpaceReflectionsOptions().enabled == enabled &&
            view->getDynamicResolutionOptions().enabled == enabled &&
            samples.enabled == enabled && samples.sampleCount == (enabled ? 4 : 1) &&
            dof.enabled == camera_dof.enabled && dof.cocScale == camera_dof.cocScale,
            "hit skipped View settings or overwrote camera DOF");
    }
    dynamic_scale = 0.625f;
    apply(false, "dynamic-resolution scale rebuilt the LUT");
    const auto dynamic = view->getDynamicResolutionOptions();
    require(dynamic.minScale.x == dynamic_scale && dynamic.minScale.y == dynamic_scale,
        "hit did not apply the new dynamic-resolution scale");

    // The cache must compare exact exposure inputs, not an approximate
    // weather threshold or just the selected tone mapper.
    tonemap.exposure = std::nextafter(tonemap.exposure, 2.0f);
    apply(true, "a changed exposure reused the LUT");
    for (const auto selection : {ToneMapperSelection::AgX, ToneMapperSelection::PBRNeutral,
             ToneMapperSelection::GT7, ToneMapperSelection::Generic}) {
        mapper = selection;
        apply(true, "a changed SDR tone mapper reused the LUT");
        apply(false, "identical SDR tone-mapper inputs rebuilt the LUT");
    }
    grading_enabled = true;
    apply(true, "enabling grading reused the disabled LUT");

    // Exercise each public grading component independently. A non-neutral
    // starting grade avoids a presence-only test of an unused neutral chain.
    const auto grade_components = [&](bool rebuild) {
        struct Scalar { const char* name; float ColorGradingConfig::* member; };
        constexpr std::array scalars{
            Scalar{"temperature", &ColorGradingConfig::temperature},
            Scalar{"tint", &ColorGradingConfig::tint},
            Scalar{"contrast", &ColorGradingConfig::contrast},
            Scalar{"saturation", &ColorGradingConfig::saturation}};
        for (const auto& control : scalars) {
            grading.*control.member += 0.05f;
            apply(rebuild, control.name);
        }
        struct Vector3 { const char* name; glm::vec3 ColorGradingConfig::* member; };
        constexpr std::array vectors3{
            Vector3{"mixer_red", &ColorGradingConfig::mixer_red},
            Vector3{"mixer_green", &ColorGradingConfig::mixer_green},
            Vector3{"mixer_blue", &ColorGradingConfig::mixer_blue},
            Vector3{"cdl_slope", &ColorGradingConfig::cdl_slope},
            Vector3{"cdl_offset", &ColorGradingConfig::cdl_offset},
            Vector3{"cdl_power", &ColorGradingConfig::cdl_power}};
        for (const auto& control : vectors3) {
            for (int component = 0; component < 3; ++component) {
                (grading.*control.member)[component] += 0.05f;
                char label[64];
                std::snprintf(label, sizeof(label), "%s[%d]", control.name, component);
                apply(rebuild, label);
            }
        }
        struct Vector4 { const char* name; glm::vec4 ColorGradingConfig::* member; };
        constexpr std::array vectors4{
            Vector4{"shadows", &ColorGradingConfig::shadows},
            Vector4{"midtones", &ColorGradingConfig::midtones},
            Vector4{"highlights", &ColorGradingConfig::highlights}};
        for (const auto& control : vectors4) {
            for (int component = 0; component < 4; ++component) {
                (grading.*control.member)[component] += 0.05f;
                char label[64];
                std::snprintf(label, sizeof(label), "%s[%d]", control.name, component);
                apply(rebuild, label);
            }
        }
    };
    grading = make_non_neutral_grade();
    apply(true, "non-neutral grading reused the previous LUT");
    grade_components(true);
    for (const bool night : {true, false}) {
        grading.night_adaptation = night;
        apply(true, "changed active night adaptation reused the LUT");
    }
    apply(false, "identical active grading rebuilt the LUT");

    grading_enabled = false;
    apply(true, "disabling grading reused the active LUT");
    grade_components(false);
    grading.night_adaptation = true;
    apply(false, "disabled night adaptation rebuilt the LUT");
    hdr = true;
    apply(true, "HDR/SDR output switch reused the LUT");
    grading_enabled = true;
    grade_components(false);
    for (const auto selection : {ToneMapperSelection::ACES, ToneMapperSelection::AgX,
             ToneMapperSelection::PBRNeutral, ToneMapperSelection::GT7}) {
        mapper = selection;
        grading.night_adaptation = !grading.night_adaptation;
        grading_enabled = !grading_enabled;
        apply(false, "HDR rebuilt for an ignored SDR input");
    }
    tonemap.exposure += 0.25f;
    apply(true, "changed HDR exposure reused the LUT");

    // A stale non-owning cache cannot substitute for missing ownership. Keep
    // the old cache populated while explicitly retiring its actual resource.
    view->setColorGrading(nullptr);
    engine.destroy(owned);
    owned = nullptr;
    apply(true, "null previous reused a retired cached resource");
    cache = {};
    apply(true, "reset cache reused the old resource without rebuilding");

    // Measure the LUT kernel independently of GPU rendering. Both runs apply
    // the same varying fog/bloom controls with identical LUT inputs. Forcing
    // cache misses provides a real rebuild control without a timing threshold.
    constexpr unsigned reconfigurations = 64;
    const auto measure_weather_reconfiguration = [&](bool force_rebuild) {
        view->setColorGrading(nullptr);
        engine.destroy(owned);
        owned = nullptr;
        cache = {};
        engine.flushAndWait();
        tonemap = {};
        quality = {};
        features = {};
        grading = {};
        hdr = false;
        grading_enabled = false;
        dynamic_resolution = false;
        dynamic_scale = 1.0f;
        msaa = false;
        ssr = false;
        mapper = ToneMapperSelection::ACES;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned step = 0; step < reconfigurations; ++step) {
            tonemap.bloom_strength = 0.1f + static_cast<float>(step) * 0.001f;
            fog_density = 0.001f + static_cast<float>(step) * 0.00001f;
            if (force_rebuild) cache.inputs.reset();
            apply(force_rebuild || step == 0, "weather timing build count");
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto builds = cache.builds;
        require(builds == (force_rebuild ? reconfigurations : 1),
            "weather timing did not execute the expected LUT builds");
        // Driver draining and resource retirement are outside the measured
        // CPU reconfiguration interval for both runs.
        engine.flushAndWait();
        std::printf("%s %u weather reconfigurations (%s): %.3f ms, %llu LUT builds\n",
            kTag, reconfigurations, force_rebuild ? "forced rebuilds" : "cached",
            std::chrono::duration<double, std::milli>(elapsed).count(),
            static_cast<unsigned long long>(builds));
    };
    measure_weather_reconfiguration(false);
    measure_weather_reconfiguration(true);

    view->setColorGrading(nullptr);
    engine.destroy(owned);
    cache = {};
    engine.destroy(view);
    require(engine.getColorGradingCount() == resource_count,
        "fixture leaked or destroyed another grading resource");
    return ok;
}

bool check_live_controls_and_history(const CaseContext& ctx, FilamentBackend& backend) {
    bool ok = true;
    const auto resolver = [](const std::string&, const std::string&) { return GpuMaterial{}; };
    auto lighting = lighting_for_time_of_day(12.0f);
    const WeatherState weather;
    const TonemapParams tonemap;
    auto& rctx = backend.debug_render_context();
    if (!rctx.engine || !rctx.scene || !rctx.view) {
        std::printf("%s FAIL: live control/history context unavailable\n", kTag);
        return false;
    }
    const auto present = [&](const Camera& camera, int width, int height) {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const bool accepted = backend.render_frame({}, {}, {}, resolver, camera,
                lighting, weather, tonemap, width, height).presented;
            rctx.engine->flushAndWait();
            if (accepted) return true;
        }
        std::printf("%s FAIL: no frame accepted for live control/history check\n", kTag);
        return false;
    };
    Camera camera;
    for (const bool enabled : {false, true, false}) {
        auto features = ctx.config.features;
        auto quality = ctx.config.quality;
        features.soft_shadows = enabled;
        features.contact_shadows = enabled;
        features.point_light_shadows = enabled;
        features.reflections = enabled;
        quality.soft_shadow_level = enabled ? QualityLevel::High : QualityLevel::Low;
        quality.contact_shadow_level = enabled ? QualityLevel::High : QualityLevel::Low;
        quality.shadow_light_budget = enabled ? 4 : 0;
        backend.set_live_render_config(features, quality);
        ok = present(camera, ctx.width, ctx.height) && ok;
        const float expected_structure_resolution = enabled ||
            ctx.config.quality.ao_quality == QualityLevel::High ? 1.0f : 0.5f;
        if (rctx.view->getAmbientOcclusionOptions().resolution !=
            expected_structure_resolution) {
            std::printf("%s FAIL: live contact-shadow toggle did not update structure depth resolution\n", kTag);
            ok = false;
        }
        if (rctx.view->getScreenSpaceReflectionsOptions().enabled != enabled ||
            rctx.view->getShadowType() != (enabled ? filament::ShadowType::PCSS : filament::ShadowType::PCF) ||
            (enabled && std::abs(rctx.view->getSoftShadowOptions().penumbraScale -
                soft_shadow_penumbra_scale(quality.soft_shadow_level)) > 1e-5f)) {
            std::printf("%s FAIL: live SSR/soft-shadow controls did not reach the view\n", kTag);
            ok = false;
        }
        bool saw_sun = false;
        auto& lights = rctx.engine->getLightManager();
        rctx.scene->forEach([&](utils::Entity entity) {
            const auto light = lights.getInstance(entity);
            if (!light || !lights.isDirectional(light) || !lights.isShadowCaster(light)) return;
            saw_sun = true;
            const auto& options = lights.getShadowOptions(light);
            if (options.screenSpaceContactShadows != enabled ||
                (enabled && options.stepCount != contact_shadow_steps(quality.contact_shadow_level, quality))) {
                std::printf("%s FAIL: live contact-shadow controls did not reach the sun\n", kTag);
                ok = false;
            }
        });
        if (!saw_sun) { std::printf("%s FAIL: no directional shadow caster checked\n", kTag); ok = false; }
    }
    backend.set_live_render_config(ctx.config.features, ctx.config.quality);
    ok = present(camera, ctx.width, ctx.height) && ok;

    // Time changes independently of weather. The same sun entity must follow
    // day -> night -> day (and within-band changes), while the grading object
    // and weather-application counter remain unchanged.
    const auto* grade_before = rctx.view->getColorGrading();
    const auto weather_applies = backend.debug_build_counters().weather_reapplies;
    utils::Entity persistent_sun;
    for (const float hour : {12.0f, 0.0f, 12.0f, 15.0f, 12.0f}) {
        lighting = lighting_for_time_of_day(hour);
        ok = present(camera, ctx.width, ctx.height) && ok;
        auto& lights = rctx.engine->getLightManager();
        bool saw_sun = false;
        bool saw_moon = false;
        rctx.scene->forEach([&](utils::Entity entity) {
            const auto light = lights.getInstance(entity);
            if (!light) return;
            if (lights.getType(light) == filament::LightManager::Type::SUN) {
                saw_sun = true;
                if (!persistent_sun) persistent_sun = entity;
                const auto direction = lights.getDirection(light);
                if (entity != persistent_sun ||
                    std::abs(lights.getIntensity(light) - lighting.sun_intensity) > 0.1f ||
                    std::abs(direction.x + lighting.sun_dir.x) > 1e-5f ||
                    std::abs(direction.y + lighting.sun_dir.y) > 1e-5f ||
                    std::abs(direction.z + lighting.sun_dir.z) > 1e-5f ||
                    lights.isShadowCaster(light) !=
                        (lighting.sun_intensity > render_tuning::shadows.sun_shadow_min_lux)) {
                    std::printf("%s FAIL: sun lighting/caster or stable entity at hour %.1f\n", kTag, hour);
                    ok = false;
                }
            } else if (lights.getType(light) == filament::LightManager::Type::DIRECTIONAL &&
                       lights.getIntensity(light) > 0.0f) {
                saw_moon = true;
            }
        });
        const auto* indirect = rctx.scene->getIndirectLight();
        const float expected_ibl = render_tuning::lighting.environment_floor_lux +
            lighting.sun_intensity / render_tuning::lighting.reference_sun_lux *
                render_tuning::lighting.environment_day_span_lux;
        const float expected_bloom = render_tuning::lighting.bloom_strength.x +
            std::clamp(lighting.sun_intensity / render_tuning::lighting.reference_sun_lux, 0.0f, 1.0f) *
                render_tuning::lighting.bloom_strength.y;
        if (!saw_sun || saw_moon != (hour == 0.0f) || !indirect ||
            std::abs(indirect->getIntensity() - expected_ibl) > 0.1f ||
            std::abs(rctx.view->getBloomOptions().strength - expected_bloom) > 1e-5f ||
            rctx.view->getColorGrading() != grade_before ||
            backend.debug_build_counters().weather_reapplies != weather_applies) {
            std::printf("%s FAIL: time-of-day lighting/IBL/bloom cache at hour %.1f\n", kTag, hour);
            ok = false;
        }
    }
    const auto* sky = backend.debug_find_feature("sky");
    const bool sky_was_enabled = sky && sky->enabled();
    const auto* probe_before = rctx.scene->getIndirectLight();
    const filament::Skybox* cached_skybox = nullptr;
    for (const bool enabled : {false, true, false, true}) {
        backend.set_feature_enabled("sky", enabled);
        ok = present(camera, ctx.width, ctx.height) && ok;
        const auto* skybox = rctx.scene->getSkybox();
        if (enabled == (skybox != nullptr) || rctx.scene->getIndirectLight() != probe_before ||
            (!enabled && cached_skybox && skybox != cached_skybox)) {
            std::printf("%s FAIL: sky toggle lost fallback or rebuilt the same-band probe\n", kTag);
            ok = false;
        }
        if (!enabled) cached_skybox = skybox;
    }
    backend.set_feature_enabled("sky", sky_was_enabled);

    const auto* arrows = backend.debug_find_feature("wind-arrows");
    const bool arrows_were_enabled = arrows && arrows->enabled();
    const std::array<IRhiBackend::WindArrow, 1> wind{{{{0, 0, 2}, {1, 0, 0}}}};
    backend.set_feature_enabled("wind-arrows", true);
    backend.set_wind_debug_arrows(wind);
    ok = present(camera, ctx.width, ctx.height) && ok;
    const auto with_arrows = rctx.scene->getRenderableCount();
    backend.set_feature_enabled("wind-arrows", false);
    if (rctx.scene->getRenderableCount() + 1 != with_arrows) {
        std::printf("%s FAIL: disabling wind debug retained its arrow\n", kTag);
        ok = false;
    }
    backend.set_wind_debug_arrows({});
    backend.set_feature_enabled("wind-arrows", arrows_were_enabled);

    const auto* ssgi = backend.debug_find_feature("ssgi");
    const auto* particles = backend.debug_find_feature("particles");
    const bool ssgi_was_enabled = ssgi && ssgi->enabled();
    const bool particles_were_enabled = particles && particles->enabled();
    backend.set_feature_enabled("particles", false);
    backend.set_feature_enabled("ssgi", true);
    const auto matches = [](const std::optional<glm::mat4>& actual, const glm::mat4& expected) {
        if (!actual) return false;
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                if (std::abs((*actual)[column][row] - expected[column][row]) > 1e-4f) return false;
        return true;
    };
    const auto projection = [](const Camera& pose, int width, int height) {
        return glm::perspective(glm::radians(pose.fov),
            static_cast<float>(width) / static_cast<float>(height), 0.5f, 2000.0f) * pose.view_matrix();
    };
    camera.fov = 35.0f;
    ok = present(camera, ctx.width, ctx.height) && ok;
    const auto first_projection = projection(camera, ctx.width, ctx.height);
    ok = matches(backend.debug_scene_color_view_proj(), first_projection) && ok;
    // Disabled / skipped capture paths may move the camera but must retain
    // the transform belonging to the last actual history image.
    backend.set_feature_enabled("ssgi", false);
    camera.position += glm::vec3(3, 2, 1);
    camera.fov = 100.0f;
    ok = present(camera, ctx.width, ctx.height) && ok;
    if (!matches(backend.debug_scene_color_view_proj(), first_projection)) {
        std::printf("%s FAIL: uncaptured camera advanced SSGI history\n", kTag); ok = false;
    }
    backend.set_feature_enabled("ssgi", true);
    for (const int width : {std::max(1, ctx.width / 2), ctx.width}) {
        ok = present(camera, width, ctx.height) && ok;
        if (!matches(backend.debug_scene_color_view_proj(), projection(camera, width, ctx.height))) {
            std::printf("%s FAIL: copied/resized SSGI history lost its camera projection\n", kTag); ok = false;
        }
    }
    backend.set_feature_enabled("ssgi", ssgi_was_enabled);
    backend.set_feature_enabled("particles", particles_were_enabled);
    return ok;
}

} // namespace

CaseResult run_camera_grading_case(const CaseContext& ctx) {
    // Hermetic NOOP engine: no GPU/display needed, isolated from the shared
    // backend. If even NOOP can't be created, SKIP (exit 0 via the harness).
    filament::Engine* engine =
        filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) {
        std::printf("%s SKIP: could not create a NOOP Filament engine\n", kTag);
        return CaseResult::Skip;
    }

    filament::Scene* scene = engine->createScene();
    filament::View*  view  = engine->createView();
    view->setScene(scene);

    const TonemapParams tonemap{};   // Shipping_Look defaults
    QualityKnobs        quality{};   // defaults: MSAA off, dynres off

    // The single backend-owned ColorGrading, threaded through every call so the
    // previous instance is destroyed by configure_post_processing each time.
    filament::ColorGrading* color_grading = nullptr;

    bool ok = true;

    // ──  Tone-mapper selection: SDR build path per operator ────────
    // Each ToneMapperSelection must build a valid ColorGrading on the SDR path
    // (hdr_output=false). This is the build path the non-ACES SDR look depends
    // on; visible output requires separate capture review.
    {
        const std::array<ToneMapperSelection, 5> selections{
            ToneMapperSelection::ACES,
            ToneMapperSelection::AgX,
            ToneMapperSelection::PBRNeutral,
            ToneMapperSelection::GT7,
            ToneMapperSelection::Generic,
        };
        for (ToneMapperSelection sel : selections) {
            color_grading = configure_post_processing(
                *engine, *view, tonemap, color_grading,
                /*fog_density_override=*/-1.0f, quality, /*msaa_enabled=*/false,
                /*ssr_enabled=*/true, /*hdr_output=*/false, sel,
                /*color_grading_enabled=*/false);
            if (!color_grading) {
                std::printf("%s FAIL: SDR ColorGrading build returned null for "
                            "tone-mapper selection %d (Req 9.2)\n", kTag,
                            static_cast<int>(sel));
                ok = false;
            }
        }
    }

    // ──  HDR path unaffected by the SDR tone-mapper argument ───────
    // The HDR output branch always uses GT7 and ignores the SDR tone_mapper
    // arg. Feeding two DIFFERENT SDR selections under hdr_output=true both build
    // successfully — the SDR selection does not alter the HDR build path. (The
    // HDR pixels require separate capture review.)
    {
        color_grading = configure_post_processing(
            *engine, *view, tonemap, color_grading,
            /*fog_density_override=*/-1.0f, quality, /*msaa_enabled=*/false,
            /*ssr_enabled=*/true, /*hdr_output=*/true,
            /*tone_mapper=*/ToneMapperSelection::AgX,
            /*color_grading_enabled=*/false);
        const bool hdr_ok_agx = color_grading != nullptr;

        color_grading = configure_post_processing(
            *engine, *view, tonemap, color_grading,
            /*fog_density_override=*/-1.0f, quality, /*msaa_enabled=*/false,
            /*ssr_enabled=*/true, /*hdr_output=*/true,
            /*tone_mapper=*/ToneMapperSelection::Generic,
            /*color_grading_enabled=*/false);
        const bool hdr_ok_generic = color_grading != nullptr;

        if (!hdr_ok_agx || !hdr_ok_generic) {
            std::printf("%s FAIL: HDR ColorGrading build was affected by the SDR "
                        "tone-mapper arg (AgX build=%d, Generic build=%d); the "
                        "HDR branch must always use GT7 regardless of the SDR "
                        "selection (Req 9.5)\n", kTag,
                        static_cast<int>(hdr_ok_agx),
                        static_cast<int>(hdr_ok_generic));
            ok = false;
        }
    }

    // ──  Non-neutral grade + night adaptation differ from neutral
    // The non-neutral grade and the night-adaptation preset must both build
    // (build path succeeds) and DIFFER from neutral at the host level so the
    // chain genuinely applies distinct controls. The >1/255-from-neutral pixel
    // delta itself requires separate capture review.
    {
        const ColorGradingConfig non_neutral = make_non_neutral_grade();
        if (!grade_differs_from_neutral(non_neutral)) {
            std::printf("%s FAIL: the 'non-neutral' grade does not differ from "
                        "neutral — the Req 10.2 presence check would be vacuous\n",
                        kTag);
            ok = false;
        }
        color_grading = configure_post_processing(
            *engine, *view, tonemap, color_grading,
            /*fog_density_override=*/-1.0f, quality, /*msaa_enabled=*/false,
            /*ssr_enabled=*/true, /*hdr_output=*/false,
            /*tone_mapper=*/ToneMapperSelection::ACES,
            /*color_grading_enabled=*/true, non_neutral);
        if (!color_grading) {
            std::printf("%s FAIL: non-neutral color-grading build returned null "
                        "(Req 10.2)\n", kTag);
            ok = false;
        }

        // Night adaptation: a neutral grade + the night preset. The
        // preset blue-shifts white balance and reduces saturation on top of the
        // controls, so it must differ from the daytime neutral grade.
        ColorGradingConfig night{};
        night.night_adaptation = true;
        if (!grade_differs_from_neutral(night)) {
            std::printf("%s FAIL: the night-adaptation config does not differ "
                        "from neutral — the Req 10.3 presence check would be "
                        "vacuous\n", kTag);
            ok = false;
        }
        color_grading = configure_post_processing(
            *engine, *view, tonemap, color_grading,
            /*fog_density_override=*/-1.0f, quality, /*msaa_enabled=*/false,
            /*ssr_enabled=*/true, /*hdr_output=*/false,
            /*tone_mapper=*/ToneMapperSelection::ACES,
            /*color_grading_enabled=*/true, night);
        if (!color_grading) {
            std::printf("%s FAIL: night-adaptation color-grading build returned "
                        "null (Req 10.3)\n", kTag);
            ok = false;
        }
    }

    // ──  Disabled grading reproduces the neutral Shipping_Look build
    // With color_grading_enabled=false the builder holds exactly the selected
    // ACES tone mapper + exposure — the same build path as the baseline
    // Shipping_Look. It must build successfully; the ≤1/255 pixel parity is
    // not assessed by this NOOP configuration test.
    {
        color_grading = configure_post_processing(
            *engine, *view, tonemap, color_grading,
            /*fog_density_override=*/-1.0f, quality, /*msaa_enabled=*/false,
            /*ssr_enabled=*/true, /*hdr_output=*/false,
            /*tone_mapper=*/ToneMapperSelection::ACES,
            /*color_grading_enabled=*/false);
        if (!color_grading) {
            std::printf("%s FAIL: disabled-grading (neutral Shipping_Look) build "
                        "returned null (Req 10.4)\n", kTag);
            ok = false;
        }
    }

    // ──  Exposure monotonicity via the pure mirror ───────────
    // The physical camera drives exposure; the pure exposure_value mirror is the
    // same photometric math Camera::setExposure performs. A strictly higher
    // computed exposure maps to a brighter render and a strictly lower one to
    // darker. exposure_value returns the brightness MULTIPLIER, so brighter ==
    // larger value.
    {
        const PhysicalCameraConfig base{}; // f/16, 1/125 s, ISO 100

        const float ev_base = exposure_value(base.aperture, base.shutter_speed,
                                             base.sensitivity);

        // Higher ISO ⇒ brighter (larger multiplier).
        const float ev_iso_up =
            exposure_value(base.aperture, base.shutter_speed, base.sensitivity * 4.0f);
        // Longer shutter (larger seconds) ⇒ brighter.
        const float ev_shutter_up =
            exposure_value(base.aperture, base.shutter_speed * 4.0f, base.sensitivity);
        // Larger aperture f-number ⇒ darker (smaller multiplier).
        const float ev_aperture_up =
            exposure_value(base.aperture * 2.0f, base.shutter_speed, base.sensitivity);

        if (!(ev_iso_up > ev_base)) {
            std::printf("%s FAIL: increasing ISO did not brighten the render "
                        "(exposure %g not > base %g) — Req 11.1/11.2\n", kTag,
                        ev_iso_up, ev_base);
            ok = false;
        }
        if (!(ev_shutter_up > ev_base)) {
            std::printf("%s FAIL: lengthening the shutter did not brighten the "
                        "render (exposure %g not > base %g) — Req 11.1/11.2\n",
                        kTag, ev_shutter_up, ev_base);
            ok = false;
        }
        if (!(ev_aperture_up < ev_base)) {
            std::printf("%s FAIL: widening the aperture f-number did not darken "
                        "the render (exposure %g not < base %g) — Req 11.1/11.2\n",
                        kTag, ev_aperture_up, ev_base);
            ok = false;
        }
    }

    // ──  DOF vs aperture via the pure mirror ──────────────────────
    // A wider aperture (smaller f-number) yields a LARGER blur metric (shallower
    // DOF) than a narrower aperture (larger f-number), matching Filament's
    // physical CoC relation. Authored benchmark DOF supplies its target
    // aperture independently through cocScale.
    {
        const float wide_dof   = dof_metric(2.8f);  // wide aperture, shallow DOF
        const float narrow_dof = dof_metric(16.0f); // narrow aperture, deep DOF
        if (!(wide_dof > narrow_dof)) {
            std::printf("%s FAIL: a wider aperture (f/2.8 metric %g) did not "
                        "yield a shallower DOF than a narrower one (f/16 metric "
                        "%g) — Req 11.3\n", kTag, wide_dof, narrow_dof);
            ok = false;
        }
    }

    // ──  Disabled defaults reproduce the Shipping_Look ───
    // The disabled state is the default-constructed config: ACES tone mapper,
    // grading disabled with a neutral ColorGradingConfig, and the physical
    // camera at f/16, 1/125 s, ISO 100 (the exposure a camera that never had
    // setExposure called uses). These defaults must equal their neutral /
    // Shipping_Look values; the ≤1/255 pixel parity bar is not assessed
    // by this NOOP configuration test.
    {
        if (ToneMapperSelection{} != ToneMapperSelection::ACES) {
            std::printf("%s FAIL: the default tone-mapper selection is not ACES "
                        "(Req 9.3)\n", kTag);
            ok = false;
        }

        const ColorGradingConfig neutral{};
        if (grade_differs_from_neutral(neutral) || neutral.night_adaptation) {
            std::printf("%s FAIL: the default ColorGradingConfig is not neutral "
                        "(Req 10.4)\n", kTag);
            ok = false;
        }

        const PhysicalCameraConfig cam{};
        if (cam.aperture != 16.0f || cam.shutter_speed != 1.0f / 125.0f ||
            cam.sensitivity != 100.0f) {
            std::printf("%s FAIL: the default PhysicalCameraConfig is not the "
                        "Shipping_Look exposure (f/16, 1/125 s, ISO 100): "
                        "aperture=%g shutter=%g iso=%g (Req 11.4)\n", kTag,
                        cam.aperture, cam.shutter_speed, cam.sensitivity);
            ok = false;
        }
    }

    // Weather reapplies post-processing after the camera sets authored DOF.
    // Exercise the real configuration path for both SDR/HDR, each independent
    // toggle, and the all-disabled state. In particular, the weather refresh
    // must retain the camera's optical scale and enabled state.
    for (bool hdr : {false, true}) {
        for (unsigned mask = 0; mask < 16; ++mask) {
            FeatureToggles features;
            features.bloom = (mask & 1u) != 0;
            features.ambient_occlusion = (mask & 2u) != 0;
            features.taa = (mask & 4u) != 0;
            features.depth_of_field = (mask & 8u) != 0;
            filament::DepthOfFieldOptions camera_dof;
            camera_dof.enabled = true;
            camera_dof.cocScale = 2.75f;
            view->setDepthOfFieldOptions(camera_dof);
            quality.ao_quality = QualityLevel::High;
            color_grading = configure_post_processing(
                *engine, *view, tonemap, color_grading, 0.001f, quality,
                false, false, hdr, ToneMapperSelection::ACES, false,
                {}, false, 1.0f, features);
            if (!color_grading ||
                view->getBloomOptions().enabled != features.bloom ||
                view->getAmbientOcclusionOptions().enabled != features.ambient_occlusion ||
                view->getTemporalAntiAliasingOptions().enabled != features.taa ||
                view->getDepthOfFieldOptions().enabled != features.depth_of_field ||
                view->getDepthOfFieldOptions().cocScale != 2.75f ||
                view->getGuardBandOptions().enabled != features.ambient_occlusion) {
                std::printf("%s FAIL: post-processing toggle mask %u (HDR=%d) "
                            "or camera DOF preservation failed\n", kTag, mask,
                            static_cast<int>(hdr));
                ok = false;
            }
        }
        // Contact shadows consume the same structure depth as AO, including
        // when AO itself is disabled. Its quality setting must not silently
        // lower contact-shadow depth, and switching contact shadows back off
        // must restore the requested AO resolution.
        for (const auto ao_quality : {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High}) {
            for (const bool ao_enabled : {false, true}) {
                for (const bool contact_enabled : {false, true, false}) {
                    FeatureToggles features;
                    features.ambient_occlusion = ao_enabled;
                    features.contact_shadows = contact_enabled;
                    quality.ao_quality = ao_quality;
                    color_grading = configure_post_processing(
                        *engine, *view, tonemap, color_grading, -1.0f, quality,
                        false, false, hdr, ToneMapperSelection::ACES, false,
                        {}, false, 1.0f, features);
                    const float expected_resolution = contact_enabled ||
                        ao_quality == QualityLevel::High ? 1.0f : 0.5f;
                    const auto& actual = view->getAmbientOcclusionOptions();
                    if (!color_grading || actual.enabled != ao_enabled ||
                        actual.resolution != expected_resolution) {
                        std::printf("%s FAIL: contact/AO structure depth resolution (HDR=%d, AO=%d, contact=%d)\n",
                                    kTag, static_cast<int>(hdr), static_cast<int>(ao_enabled),
                                    static_cast<int>(contact_enabled));
                        ok = false;
                    }
                }
            }
        }
        view->setDepthOfFieldOptions(filament::DepthOfFieldOptions{});
        color_grading = configure_post_processing(
            *engine, *view, tonemap, color_grading, -1.0f, quality,
            false, true, hdr);
        if (view->getDepthOfFieldOptions().enabled) {
            std::printf("%s FAIL: post-processing enabled DOF without camera opt-in\n", kTag);
            ok = false;
        }
    }

    // The depth camera must use the authored/diagnostic FOV, including a
    // change between frames. Reading the actual Filament camera catches a
    // hardcoded 60-degree projection independently of screenshot metadata.
    {
        BackendCore core;
        core.engine = engine;
        DepthPrePass depth(core);
        depth.ensure(640, 480);
        if (!depth.view()) {
            std::printf("%s FAIL: depth view unavailable for projection check\n", kTag);
            ok = false;
        } else {
            for (float fov : {30.0f, 90.0f}) {
                Camera camera;
                camera.fov = fov;
                depth.aim(camera, 640, 480);
                const double actual = depth.view()->getCamera().getFieldOfViewInDegrees(
                    filament::Camera::Fov::VERTICAL);
                if (std::abs(actual - static_cast<double>(fov)) > 1e-4) {
                    std::printf("%s FAIL: depth FOV %g differs from requested %g\n",
                                kTag, actual, static_cast<double>(fov));
                    ok = false;
                }
            }
        }
    }

    ok = check_color_grading_cache(*engine) && ok;

    // Tear down our private engine resources (borrowers before their owners).
    view->setColorGrading(nullptr);
    if (color_grading) engine->destroy(color_grading);
    engine->destroy(view);
    engine->destroy(scene);
    filament::Engine::destroy(&engine);

    if (ctx.backend.api() == GraphicsApi::Filament) {
        auto& backend = static_cast<FilamentBackend&>(ctx.backend);
        if (auto result = backend.init_pipelines(ctx.config); !result) {
            std::printf("%s FAIL: main camera pipeline unavailable: %s\n",
                        kTag, result.error().message.c_str());
            ok = false;
        } else {
            const MaterialResolver resolver =
                [](const std::string&, const std::string&) { return GpuMaterial{}; };
            const LightingParams lighting = lighting_for_time_of_day(ctx.config.time_of_day);
            const WeatherState weather;
            for (float fov : {30.0f, 90.0f, 60.0f}) {
                Camera camera;
                camera.fov = fov;
                backend.render_frame({}, {}, {}, resolver, camera, lighting,
                                     weather, tonemap, ctx.width, ctx.height);
                const auto* active_camera = backend.debug_render_context().camera;
                const double actual = active_camera
                    ? active_camera->getFieldOfViewInDegrees(filament::Camera::Fov::VERTICAL)
                    : 0.0;
                if (std::abs(actual - static_cast<double>(fov)) > 1e-4) {
                    std::printf("%s FAIL: main FOV %g differs from requested %g\n",
                                kTag, actual, static_cast<double>(fov));
                    ok = false;
                }
            }
        }
    }

    if (ctx.backend.api() == GraphicsApi::Filament) {
        ok = check_live_controls_and_history(ctx,
            static_cast<FilamentBackend&>(ctx.backend)) && ok;
    }
    if (!ok) return CaseResult::Fail;

    std::printf("%s tone-mapper SDR build succeeds for ACES/AgX/PBRNeutral/GT7/"
                "Generic and the HDR path is unaffected by the SDR selection "
                "(Req 9.2/9.3); non-neutral grade + night adaptation build and "
                "differ from neutral while disabled grading reproduces the "
                "neutral Shipping_Look build (Req 10.2/10.3/10.4); exposure is "
                "monotonic (brighter ISO/shutter, darker aperture) and a wider "
                "aperture yields a shallower DOF (Req 11.1/11.3); disabled "
                "defaults are the Shipping_Look ACES / neutral grade / f16-1/125-"
                "ISO100 exposure (Req 11.4). Genuine pixel deltas are "
                "not assessed by this configuration test.\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
