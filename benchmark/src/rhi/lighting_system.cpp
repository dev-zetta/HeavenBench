/*
 * HeavenBench — Lighting system
 *
 * Extracted verbatim out of FilamentBackend::setup_lighting and the
 * init-time skybox/IBL block in init_pipelines().
 */
#include "rhi/lighting_system.h"
#include "rhi/sky_ibl.h"
#include "rhi/bloom_strength.h"
#include "rhi/filament_util.h"   // select_shadow_casting_lights
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Camera.h>
#include <filament/Skybox.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Texture.h>
#include <filament/Options.h>
#include <filament-iblprefilter/IBLPrefilterContext.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <math/vec3.h>

#include <cstdio>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace heaven {

struct EnvironmentProbes {
    explicit EnvironmentProbes(filament::Engine& engine)
        : context(engine), specular(context), irradiance_filter(context) {}

    IBLPrefilterContext context;
    IBLPrefilterContext::SpecularFilter specular;
    IBLPrefilterContext::IrradianceFilter irradiance_filter;
    std::array<filament::Texture*, 4> radiance{};
    std::array<filament::Texture*, 4> reflections{};
    std::array<filament::Texture*, 4> irradiance{};
};

namespace {

float environment_intensity(const LightingParams& lighting) {
    return render_tuning::lighting.environment_floor_lux +
        (lighting.sun_intensity /
         render_tuning::lighting.reference_sun_lux) *
        render_tuning::lighting.environment_day_span_lux;
}

} // namespace

LightingSystem::LightingSystem(const BackendCore& core) : core_(core) {}

LightingSystem::~LightingSystem() {
    auto* engine = core_.engine;
    if (!engine) return;
    if (skybox_) engine->destroy(skybox_);
    if (indirect_light_) engine->destroy(indirect_light_);
    if (environment_probes_) {
        for (auto* texture : environment_probes_->irradiance)
            if (texture) engine->destroy(texture);
        for (auto* texture : environment_probes_->reflections)
            if (texture) engine->destroy(texture);
        for (auto* texture : environment_probes_->radiance)
            if (texture) engine->destroy(texture);
        environment_probes_.reset();
    } else if (sky_cubemap_) {
        engine->destroy(sky_cubemap_);
    }
    if (sun_entity_) {
        engine->destroy(sun_entity_);
        utils::EntityManager::get().destroy(sun_entity_);
    }
    if (moon_entity_) {
        engine->destroy(moon_entity_);
        utils::EntityManager::get().destroy(moon_entity_);
    }
}

void LightingSystem::init_environment(const LightingParams& initial_light) {
    auto* engine_ = core_.engine;
    auto* scene_  = core_.scene;

    // ── Skybox + IBL backed by a procedural Z-up cubemap ────────
    // Earlier attempts used a hand-rolled sky-sphere with a custom
    // shader, and a Filament Skybox driven by the original Heaven
    // 32×32 cubemaps. Both produced a visible vertical seam:
    //   * the sphere/cube approach interpolates `dir.z` per fragment
    //     across triangle edges and produced a discontinuity at the
    //     screen-space silhouette of the cube;
    //   * the original cubemaps were authored Y-up but we render
    //     Z-up, so sampling them with world-space directions
    //     rotated the sky 90° about the X axis, producing a bright
    //     half on the sun-facing side.

    // Building a 256×256 cubemap on the CPU per time-of-day band
    // sidesteps both: every texel is computed from a true Z-up
    // world-space direction, and Filament's standard cubemap
    // sampler renders it with no possibility of seams. The result
    // is also used as the IBL reflections probe so the lighting
    // stays in sync with the visible sky.
    environment_probes_ = std::make_unique<EnvironmentProbes>(*engine_);
    constexpr auto kBandHours = render_tuning::lighting.probe_band_hours;
    bool filtering_ok = true;
    for (size_t band = 0; band < kBandHours.size(); ++band) {
        const LightingParams band_light =
            band == 3 &&
                    initial_light.time_of_day >=
                        render_tuning::lighting.time_band_start_hours[3] &&
                    initial_light.time_of_day <
                        render_tuning::lighting.evening_end_hour
                ? initial_light
                : lighting_for_time_of_day(kBandHours[band]);
        auto* radiance = build_sky_cubemap(*engine_, band_light);
        if (!radiance) {
            filtering_ok = false;
            break;
        }
        environment_probes_->radiance[band] = radiance;
        environment_probes_->reflections[band] =
            environment_probes_->specular(radiance);
        environment_probes_->irradiance[band] =
            environment_probes_->irradiance_filter(radiance);
        if (!environment_probes_->reflections[band] ||
            !environment_probes_->irradiance[band]) {
            filtering_ok = false;
            break;
        }
    }
    if (!filtering_ok) {
        std::printf(
            "  IBL: HDR prefiltering unavailable; using flat ambient fallback\n");
        for (auto* texture : environment_probes_->irradiance)
            if (texture) engine_->destroy(texture);
        for (auto* texture : environment_probes_->reflections)
            if (texture) engine_->destroy(texture);
        for (auto* texture : environment_probes_->radiance)
            if (texture) engine_->destroy(texture);
        environment_probes_.reset();
    }
    sky_cubemap_ = environment_probes_
        ? environment_probes_->radiance[3]
        : build_sky_cubemap(*engine_, initial_light);
    sky_cubemap_band_ = 3;

    skybox_ = filament::Skybox::Builder()
        .environment(sky_cubemap_)
        .showSun(false)
        .build(*engine_);
    scene_->setSkybox(skybox_);

    const auto fallback = render_tuning::lighting.fallback_irradiance;
    filament::math::float3 sh_flat[] = {
        {fallback.x, fallback.y, fallback.z}, {}, {}, {}, {}, {}, {}, {}, {},
    };
    auto indirect_builder = filament::IndirectLight::Builder()
        .reflections(environment_probes_
            ? environment_probes_->reflections[3]
            : sky_cubemap_)
        .intensity(environment_intensity(initial_light));
    if (environment_probes_)
        indirect_builder.irradiance(environment_probes_->irradiance[3]);
    else
        indirect_builder.irradiance(3, sh_flat);
    indirect_light_ = indirect_builder.build(*engine_);
    scene_->setIndirectLight(indirect_light_);
}

void LightingSystem::configure_soft_shadows(filament::LightManager& lm,
                                            utils::Entity sun,
                                            const QualityKnobs& quality,
                                            bool enabled,
                                            bool colored_penumbra) {
    auto* view_ = core_.view;
    if (!view_) return;

    auto sun_instance = lm.getInstance(sun);

    if (enabled) {
        // Contact-hardening soft shadows: switch the View to PCSS
        // and feed the soft-shadow Quality_Knob into the penumbra scale. The
        // sample count from soft_shadow_sample_count(level) is a strictly
        // increasing ladder (8 / 16 / 24); we normalise it against the Medium
        // (16-sample) baseline so a higher level widens/softens the penumbra
        // monotonically  while leaving Medium at the natural
        // PCSS default.
        view_->setShadowType(filament::ShadowType::PCSS);

        // Widen the PCSS penumbra so the shadow edge has a visible soft
        // transition rather than a near-hard line. soft_shadow_penumbra_scale()
        // maps the Quality_Knob level to penumbraScale monotonically (Low 1.5 /
        // Medium 3.0 / High 4.5) — the single source of truth shared with the
        // soft-shadow smoke test. penumbraRatioScale stays at 1.0 (the contact-
        // hardening ratio is left at Filament's default).
        filament::SoftShadowOptions soft;
        soft.penumbraScale = soft_shadow_penumbra_scale(quality.soft_shadow_level);
        soft.penumbraRatioScale = 1.0f;
        view_->setSoftShadowOptions(soft);

        // ── Colored penumbra  ────────────────────────────────────
        // Colored penumbra tints the soft-shadow penumbra and is therefore
        // only meaningful while this DPCF/PCSS path is active — hence it is
        // configured here on the existing soft-shadow path rather than as a
        // separate View-option group.

        // IMPORTANT: in the vendored Filament, colored penumbra is NOT a
        // runtime View option — `filament::SoftShadowOptions` has no
        // `coloredPenumbra` field. It is a COMPILE-TIME material property
        // (`filamat::MaterialBuilder::coloredPenumbra(bool)`), surfaced to the
        // shaders as the `HAS_COLORED_PENUMBRA` define and consumed by the
        // penumbra-tint block in `surface_shading_model_standard.fs` /
        // `_cloth.fs`. The committed HAS_COLORED_PENUMBRA define fix (see
        // AGENTS.md) makes that define — and the tint block — compile in ONLY
        // for materials authored with `coloredPenumbra: true`; every shipped
        // lit material leaves it off, so there is nothing to set on `soft`
        // here and the disabled state stays bit-identical to the Shipping_Look
        // Carrying the toggle onto this path keeps the
        // colored-penumbra configuration co-located with the soft-shadow setup
        // the visible tint is governed by which base material a
        // surface is built from (compiled via the incremental `--materials`
        // path — no new lib/filament patch).
        if (colored_penumbra) {
            static bool colored_penumbra_noted = false;
            if (!colored_penumbra_noted) {
                colored_penumbra_noted = true;
                std::printf("  Colored penumbra enabled on the soft-shadow "
                            "path (tint driven by HAS_COLORED_PENUMBRA "
                            "materials; default-off materials unaffected).\n");
            }
        }

        // ── Directional shadow-map resolution (the actual smoothness fix) ──
        // PCSS only softens the penumbra; it cannot add detail a low-resolution
        // shadow map never captured. Filament's default directional ShadowOptions
        // is a SINGLE 1024² cascade whose frustum is fit to the whole camera far
        // distance (2000 m here), so each texel covers a large patch of world and
        // near-camera shadows stair-step badly. Switch the sun to 4-cascade CSM
        // at 2048² per cascade and clamp the shadow distance to the visible near
        // range so the cascades pack their resolution where the camera actually
        // is. computePracticalSplits biases the splits toward a log scheme so the
        // nearest cascade — which covers props on the ground right in front of the
        // camera — gets the tightest, sharpest texels. LiSPSM is disabled because
        // it perturbs the PCSS blur kernel and reintroduces edge artifacts; the
        // cascades give the resolution win LiSPSM would otherwise provide.
        if (sun_instance) {
            filament::LightManager::ShadowOptions opts =
                lm.getShadowOptions(sun_instance);
            opts.mapSize        = static_cast<uint32_t>(
                std::clamp(quality.sun_shadow_map_size, 1024, 4096));
            opts.shadowCascades = 4;
            opts.shadowFar = render_tuning::shadows.sun_shadow_far;
            opts.lispsm         = false;
            opts.stable         =
                quality.sun_shadow_map_size >= 4096;
            filament::LightManager::ShadowCascades::computePracticalSplits(
                opts.cascadeSplitPositions, opts.shadowCascades,
                /*near=*/render_tuning::shadows.cascade_near,
                /*far=*/opts.shadowFar,
                /*lambda=*/render_tuning::shadows.cascade_lambda);
            lm.setShadowOptions(sun_instance, opts);
        }
    } else {
        // Shipping_Look parity: basic PCF with Filament's default
        // soft-shadow options. This is bit-identical to the baseline path,
        // which never touched the shadow type (PCF is the Filament default)
        // and never set SoftShadowOptions.
        view_->setShadowType(filament::ShadowType::PCF);
        view_->setSoftShadowOptions(filament::SoftShadowOptions{});

        // Colored penumbra only applies to the DPCF/PCSS soft-shadow path
        // so when soft shadows are disabled it has no effect even if
        // its own toggle is on. Note it once so the no-op is not mistaken for
        // a parity break — the disabled soft-shadow path is unchanged from the
        // Shipping_Look regardless of the colored-penumbra toggle.
        if (colored_penumbra) {
            static bool colored_penumbra_no_soft_noted = false;
            if (!colored_penumbra_no_soft_noted) {
                colored_penumbra_no_soft_noted = true;
                std::printf("  Colored penumbra requested but soft shadows are "
                            "disabled; colored penumbra only applies to the "
                            "DPCF/PCSS soft-shadow path and has no effect.\n");
            }
        }

        // Restore the default directional ShadowOptions (single 1024² cascade,
        // LiSPSM on) so the disabled state matches the baseline shadow map.
        // The contact-shadow fields are re-applied right after by
        // configure_contact_shadows(), which reads these back and toggles only
        // its own screenSpaceContactShadows/stepCount.
        if (sun_instance) {
            filament::LightManager::ShadowOptions opts =
                lm.getShadowOptions(sun_instance);
            opts.mapSize        = 1024;
            opts.shadowCascades = 1;
            opts.shadowFar      = 0.0f;
            opts.lispsm         = true;
            opts.stable         = false;
            lm.setShadowOptions(sun_instance, opts);
        }
    }

    // The sun's contact-shadow ShadowOptions (screenSpaceContactShadows /
    // stepCount) are applied separately by configure_contact_shadows(), called
    // right after this in setup(); it reads back the options written above and
    // toggles only its own fields.
}

void LightingSystem::configure_contact_shadows(filament::LightManager& lm,
                                               utils::Entity sun,
                                               const QualityKnobs& quality,
                                               bool enabled) {
    auto sun_instance = lm.getInstance(sun);
    if (!sun_instance) return;

    // Read the sun's existing ShadowOptions and toggle only the contact-shadow
    // fields, leaving every other shadow setting (map size, cascades, etc.)
    // exactly as built. This keeps the directional lighting bit-identical to
    // the Shipping_Look when contact shadows are disabled.
    filament::LightManager::ShadowOptions options = lm.getShadowOptions(sun_instance);

    if (enabled) {
        // Screen-space contact shadows on the directional sun. The
        // ray-march step count comes from the contact-shadow Quality_Knob,
        // mapped through contact_shadow_steps() which is a strictly increasing
        // ladder bounded by [contact_shadow_step_min, contact_shadow_step_max]
        // so the per-pixel cost is bounded at the highest level.
        const int steps = contact_shadow_steps(quality.contact_shadow_level, quality);
        options.screenSpaceContactShadows = true;
        options.stepCount = static_cast<uint8_t>(
            std::clamp(steps, 1, 255));
    } else {
        // Shipping_Look parity: contact shadows off, leaving the
        // directional lighting bit-identical to the baseline default (the
        // ShadowOptions default has screenSpaceContactShadows = false).
        options.screenSpaceContactShadows = false;
    }

    lm.setShadowOptions(sun_instance, options);
}

void LightingSystem::configure_moon_shadow_options() {
    auto& lm = core_.engine->getLightManager();
    const auto sun = lm.getInstance(sun_entity_);
    const auto moon = lm.getInstance(moon_entity_);
    if (!sun || !moon) return;
    auto options = lm.getShadowOptions(sun);
    options.shadowBulbRadius = std::tan(
        glm::radians(render_tuning::sky.moon_radius_degrees));
    lm.setShadowOptions(moon, options);
}

void LightingSystem::configure_point_light_shadows(
        std::span<const SceneLightInfo> scene_lights, int budget,
        int map_size, bool enabled) {
    auto* engine_ = core_.engine;
    if (!engine_) return;
    auto& lm = engine_->getLightManager();

    // B1′ shadow-cost cap (metres). A shadow-casting point light renders every
    // caster inside its falloff sphere into 6 cube faces every frame; capping
    // the falloff of the (few) casters bounds how much tessellated geometry each
    // face pulls in. Chosen generously so the lamps still light their immediate
    // surroundings convincingly while distant geometry stops casting from them.
    // Only casters whose authored falloff exceeds this are shrunk; their reach
    // is restored when they stop casting (e.g. budget lowered at runtime).
    constexpr float kShadowCasterFalloffCap =
        render_tuning::shadows.point_falloff_cap;

    // ── Apply-on-change guard ───────────────────────────────────
    // This runs every frame, but the shadow-caster selection is deterministic.
    // Hash the fields that drive selection instead of merely comparing the
    // candidate count: a same-size light-set replacement or an authored
    // intensity edit must invalidate the cached choice. Deliberately exclude
    // position because moving/bobbing a selected light is handled by Filament
    // without changing its shadow-caster policy.
    uint64_t light_set_sig = 1469598103934665603ull;
    auto mix_sig = [&light_set_sig](uint64_t value) {
        light_set_sig ^= value;
        light_set_sig *= 1099511628211ull;
    };
    for (const SceneLightInfo& light : scene_lights) {
        mix_sig(light.entity.getId());
        mix_sig(std::bit_cast<uint32_t>(light.intensity));
        mix_sig(static_cast<uint64_t>(light.shadow_eligible));
    }

    // Re-issuing setShadowCaster / setShadowOptions every
    // frame forces Filament to invalidate and rebuild its shadow atlas each
    // frame — that is what produced the per-frame lighting flicker AND the
    // camera-movement lag (a CPU/render-thread stall, not a GPU cost: FPS stays
    // huge while input feels sticky). So we compute the selection and touch the
    // LightManager ONLY when one of those inputs actually changed; on every
    // other frame this is a couple of integer comparisons and an early return.
    // (The light positions still bob per frame via their transforms — that is
    // cheap and does not require re-touching shadow-caster state.)
    if (ptlight_applied_ &&
        enabled == ptlight_enabled_sig_ &&
        budget  == ptlight_budget_sig_ &&
        map_size == ptlight_map_size_sig_ &&
        light_set_sig == ptlight_set_sig_) {
        return;
    }

    // When the feature is disabled we treat the budget as 0, so the selection
    // is empty and every candidate gets setShadowCaster(false) below — leaving
    // local lighting bit-identical to the Shipping_Look. The budget
    // is clamped to ≥ 0 defensively; the CLI already clamps it to its valid
    // range, but the selection helper also treats ≤ 0 as "select none".
    const int effective_budget = enabled ? std::max(0, budget) : 0;

    // Highest-intensity-first selection up to the budget, ties broken by the
    // stable per-light entity id, deterministic across runs.
    // Returns ascending original indices into scene_lights.
    const std::vector<size_t> selected =
        select_shadow_casting_lights(scene_lights, effective_budget);

    // First, clear shadow casting on the lights that WERE casting last apply
    // but are no longer selected. Tracking the previous set means we only
    // touch lights whose state actually flips, instead of writing false to all
    // ~197 candidates every apply.
    std::vector<utils::Entity> new_casters;
    new_casters.reserve(selected.size());

    // Build the new caster set + configure each selected light.
    size_t next_sel = 0;
    for (size_t i = 0; i < scene_lights.size(); ++i) {
        const bool cast = (next_sel < selected.size() && selected[next_sel] == i);
        if (!cast) continue;
        ++next_sel;

        auto inst = lm.getInstance(scene_lights[i].entity);
        if (!inst) continue;

        // Modest per-light ShadowOptions: point lights render a 6-face cube
        // shadow, so the default 1024 map per light overcommits Filament's
        // shadow atlas; a small map keeps every budgeted caster resident. The
        // raised bias suppresses self-shadow acne on the emission lights, which
        // sit on the emissive surface and bob with the floating islands.
        filament::LightManager::ShadowOptions opts = lm.getShadowOptions(inst);
        opts.mapSize      = static_cast<uint32_t>(
            std::clamp(map_size, 256, 1024));
        opts.constantBias =
            render_tuning::shadows.point_constant_bias;
        opts.normalBias = render_tuning::shadows.point_normal_bias;
        lm.setShadowOptions(inst, opts);
        lm.setShadowCaster(inst, true);

        // B1′: bound the shadow cost. A point light renders every shadow caster
        // inside its falloff sphere into all 6 cube faces; the brightest lamps
        // (the ones selected here) have the largest radii, so their influence
        // sphere sweeps in distant tessellated geometry that dominates the
        // shadow pass. Cap the falloff while this light casts so each face
        // frustum-culls everything past the cap, then remember the original so
        // it can be restored if the light later stops casting. We only shrink
        // (never grow) the radius, and only record the original on the first
        // clamp so a re-apply doesn't overwrite it with the already-capped value.
        const float orig_falloff = lm.getFalloff(inst);
        if (orig_falloff > kShadowCasterFalloffCap) {
            bool already = false;
            for (const auto& [e, f] : ptlight_orig_falloff_) {
                if (e == scene_lights[i].entity) { already = true; break; }
            }
            if (!already) {
                ptlight_orig_falloff_.emplace_back(scene_lights[i].entity, orig_falloff);
            }
            lm.setFalloff(inst, kShadowCasterFalloffCap);
            std::printf("  Point-light shadow caster: falloff %.1f m -> %.1f m "
                        "(B1' shadow-cost cap)\n",
                        orig_falloff, kShadowCasterFalloffCap);
        }

        new_casters.push_back(scene_lights[i].entity);
    }

    // Turn OFF every light that was casting last apply but isn't now.
    for (utils::Entity prev : ptlight_shadow_casters_) {
        bool still = false;
        for (utils::Entity now : new_casters) {
            if (now == prev) { still = true; break; }
        }
        if (still) continue;
        auto inst = lm.getInstance(prev);
        if (inst) lm.setShadowCaster(inst, false);

        // This light no longer casts, so it no longer pays the shadow cost —
        // restore its original (unclamped) falloff so its lighting reach comes
        // back to the authored value, and drop the bookkeeping entry.
        for (size_t k = 0; k < ptlight_orig_falloff_.size(); ++k) {
            if (ptlight_orig_falloff_[k].first != prev) continue;
            if (inst) lm.setFalloff(inst, ptlight_orig_falloff_[k].second);
            ptlight_orig_falloff_[k] = ptlight_orig_falloff_.back();
            ptlight_orig_falloff_.pop_back();
            break;
        }
    }

    ptlight_shadow_casters_ = std::move(new_casters);
    ptlight_enabled_sig_    = enabled;
    ptlight_budget_sig_     = budget;
    ptlight_map_size_sig_   = map_size;
    ptlight_set_sig_        = light_set_sig;
    ptlight_applied_        = true;
}

void LightingSystem::setup(const LightingParams& lighting, bool sky_sphere_enabled,
                           const QualityKnobs& quality, const FeatureToggles& features,
                           float requested_bloom_strength) {
    auto* engine_     = core_.engine;
    auto* scene_      = core_.scene;
    // Keep the directional entity stable while the tour animates the sun.
    // Rebuilding it every time of day tick would invalidate shadow resources.
    if (!sun_entity_) {
        sun_entity_ = utils::EntityManager::get().create();
        filament::LightManager::Builder(filament::LightManager::Type::SUN)
            .sunAngularRadius(render_tuning::shadows.sun_angular_radius_degrees)
            .build(*engine_, sun_entity_);
        scene_->addEntity(sun_entity_);
    }

    // Soft directional shadows  + colored penumbra. Apply the
    // PCSS / PCF shadow-mode and penumbra configuration through the View /
    // LightManager option path now that the sun is built, carrying the
    // colored-penumbra toggle onto the same soft-shadow path. When the soft-
    // shadow feature is disabled this restores the Shipping_Look basic PCF mode
    // (bit-identical parity) and colored penumbra has no effect, so it is safe
    // to call unconditionally every setup() — a runtime toggle lands next frame.
    configure_soft_shadows(engine_->getLightManager(), sun_entity_,
                           quality, features.soft_shadows,
                           features.colored_penumbra);

    // Screen-space contact shadows. Configure the sun's ShadowOptions
    // through the same LightManager option path now that the sun is built.
    // When the feature is disabled this leaves screenSpaceContactShadows = false
    // (bit-identical Shipping_Look directional lighting), so it is safe to call
    // unconditionally every setup() — a runtime toggle lands next frame.
    configure_contact_shadows(engine_->getLightManager(), sun_entity_,
                              quality, features.contact_shadows);
    configure_moon_shadow_options();

    // Post-processing configuration can replace bloom options even when the
    // lighting did not change, so force its time-dependent retune after setup.
    previous_lighting_.reset();
    update(lighting, sky_sphere_enabled, features, requested_bloom_strength);
}

void LightingSystem::update(const LightingParams& lighting, bool sky_sphere_enabled,
                            const FeatureToggles& features,
                            float requested_bloom_strength) {
    requested_bloom_strength = clamp_bloom_strength(requested_bloom_strength).value;
    if (previous_lighting_) {
        const auto& previous = *previous_lighting_;
        if (previous.sun_dir == lighting.sun_dir &&
            previous.sun_color == lighting.sun_color &&
            previous.sun_intensity == lighting.sun_intensity &&
            previous.ambient == lighting.ambient &&
            previous.sky_color == lighting.sky_color &&
            previous.time_of_day == lighting.time_of_day &&
            previous_sky_sphere_enabled_ == sky_sphere_enabled &&
            previous_bloom_enabled_ == features.bloom &&
            previous_requested_bloom_strength_ == requested_bloom_strength &&
            previous_clouds_enabled_ == features.clouds) return;
    }
    auto* engine_ = core_.engine;
    auto* scene_ = core_.scene;
    auto* view_ = core_.view;
    auto& light_manager = engine_->getLightManager();
    const auto sun = light_manager.getInstance(sun_entity_);
    if (!sun) return;
    // Filament uses the direction of travel; LightingParams points TO the sun.
    light_manager.setDirection(sun, {-lighting.sun_dir.x, -lighting.sun_dir.y, -lighting.sun_dir.z});
    light_manager.setColor(sun, {lighting.sun_color.x, lighting.sun_color.y, lighting.sun_color.z});
    // The time-of-day atmosphere retains a sunset illuminance tail below the
    // horizon. It is scattered twilight, not a direct solar emitter: leaving
    // it on this entity can outrank the Moon and suppress its illumination.
    const float direct_sun = lighting.sun_dir.z > 0.0f
        ? std::max(0.0f, lighting.sun_intensity) : 0.0f;
    light_manager.setIntensity(sun, direct_sun);
    light_manager.setShadowCaster(sun,
        direct_sun > render_tuning::shadows.sun_shadow_min_lux);

    // Select among the four startup-filtered probes. Continuous tour lighting
    // only changes IBL intensity within a band; it never rebuilds the probes.
    //   00–05 → 0 (night)
    //   05–10 → 1 (morning)
    //   10–16 → 2 (day)
    //   16–21 → 3 (evening)
    //   21–24 → 0 (night)
    int band;
    if (lighting.time_of_day <
            render_tuning::lighting.time_band_start_hours[1] ||
        lighting.time_of_day >= render_tuning::lighting.evening_end_hour)
        band = 0;
    else if (lighting.time_of_day <
             render_tuning::lighting.time_band_start_hours[2])
        band = 1;
    else if (lighting.time_of_day <
             render_tuning::lighting.time_band_start_hours[3])
        band = 2;
    else                                                              band = 3;

    if (band != sky_cubemap_band_) {
        // Rebuild the IBL probe cubemap for the new time-of-day band.
        // The visible sky is normally painted by the sky-sphere
        // feature, so we keep Filament's cubemap Skybox DETACHED while
        // that feature is enabled. When the sky feature is disabled
        // (`--no-sky`), we re-attach the cubemap Skybox so the
        // background isn't a flat clear colour.
        if (skybox_) {
            scene_->setSkybox(nullptr);
            engine_->destroy(skybox_);
            skybox_ = nullptr;
        }
        if (indirect_light_) {
            scene_->setIndirectLight(nullptr);
            engine_->destroy(indirect_light_);
            indirect_light_ = nullptr;
        }
        // The fallback owns its current texture instead of borrowing a probe.
        if (!environment_probes_ && sky_cubemap_) engine_->destroy(sky_cubemap_);
        sky_cubemap_ = environment_probes_
            ? environment_probes_->radiance[static_cast<size_t>(band)]
            : build_sky_cubemap(*engine_, lighting, features.clouds);
        sky_cubemap_band_ = band;

        // Retain a skybox even while hidden so toggling the visible sphere
        // off within the same time band can attach it without a probe rebuild.
        skybox_ = filament::Skybox::Builder()
            .environment(sky_cubemap_)
            .showSun(false)
            .build(*engine_);

        const float ibl = environment_intensity(lighting);
        auto builder = filament::IndirectLight::Builder()
            .reflections(environment_probes_
                ? environment_probes_->reflections[static_cast<size_t>(band)]
                : sky_cubemap_)
            .intensity(ibl);
        if (environment_probes_) {
            builder.irradiance(
                environment_probes_->irradiance[static_cast<size_t>(band)]);
        } else {
            const auto fallback = render_tuning::lighting.fallback_irradiance;
            filament::math::float3 sh_flat[] = {
                {fallback.x, fallback.y, fallback.z},
                {}, {}, {}, {}, {}, {}, {}, {},
            };
            builder.irradiance(3, sh_flat);
        }
        indirect_light_ = builder.build(*engine_);
        scene_->setIndirectLight(indirect_light_);
    } else if (indirect_light_) {
        // Same band — just retune IBL strength with sun intensity.
        // IBL intensity floor at night was 50 lux (~moonlight); not
        // enough to lift unlit walls off pure-black against the sky.
        // 800 lux roughly matches "deep dusk" ambient so cottages
        // outside any lamp falloff still read as 3D objects rather
        // than solid silhouettes against the bright skybox.
        const float ibl = environment_intensity(lighting);
        indirect_light_->setIntensity(ibl);
    }

    // Background visibility is independent of probe-band changes. In
    // particular, day -> sky off -> sky on must never leave a black backdrop
    // or retain the cubemap behind a newly enabled visible sky sphere.
    scene_->setSkybox(sky_sphere_enabled ? nullptr : skybox_);

    const auto lunar = moon_lighting(lighting);
    if (lunar.weight > render_tuning::lighting.moon_min_weight) {
        if (!moon_entity_) {
            moon_entity_ = utils::EntityManager::get().create();
            const auto moon_color = render_tuning::lighting.moon_color;
            filament::LightManager::Builder(filament::LightManager::Type::DIRECTIONAL)
                .color({moon_color.x, moon_color.y, moon_color.z})
                .castShadows(true)
                .build(*engine_, moon_entity_);
            configure_moon_shadow_options();
        }
        if (!moon_in_scene_) {
            scene_->addEntity(moon_entity_);
            moon_in_scene_ = true;
        }
        const auto moon = light_manager.getInstance(moon_entity_);
        light_manager.setDirection(moon, {
            -lunar.direction.x, -lunar.direction.y, -lunar.direction.z});
        light_manager.setIntensity(moon,
            render_tuning::lighting.moon_intensity_lux * lunar.weight);
    } else if (moon_in_scene_) {
        scene_->remove(moon_entity_);
        moon_in_scene_ = false;
    }

    // Filament's built-in distance fog is disabled — see the
    // configure_post_processing comment for why. We deliberately don't
    // reset its parameters here either; they were a per-frame
    // restamping when we wanted dusk-tinted distance fog, and that
    // role now belongs to heaven_volumetric_fog.mat. The volumetric
    // material picks up the sun colour through its own per-frame
    // `sunColor` uniform, so dusk-warmth still lives in the air.

    // Bloom should feel modest at dusk — the daylight 0.3 strength makes
    // window emission bleed across the screen when the rest of the scene
    // is dim. Scale the caller's strength by this existing day/night response,
    // preserving quality and lens-dirt options configured by post-processing.
    {
        // Preserve quality, dirt texture and other post-process settings.
        auto bloom = view_->getBloomOptions();
        bloom.enabled = features.bloom;
        bloom.strength = bloom_strength_for_time_of_day(
            requested_bloom_strength, lighting.sun_intensity);
        view_->setBloomOptions(bloom);
    }
    previous_lighting_ = lighting;
    previous_sky_sphere_enabled_ = sky_sphere_enabled;
    previous_bloom_enabled_ = features.bloom;
    previous_requested_bloom_strength_ = requested_bloom_strength;
    previous_clouds_enabled_ = features.clouds;
}

} // namespace heaven
