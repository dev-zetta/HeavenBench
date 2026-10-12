#include "rhi/material_tuning_bindings.h"

#include "render_tuning.h"

#include <filament/MaterialInstance.h>

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>

namespace heaven::material_tuning {
namespace {

using render_tuning::Float2;
using render_tuning::Float3;
using render_tuning::Float4;

[[nodiscard]] filament::math::float2 to_filament(Float2 v) {
    return {v.x, v.y};
}

[[nodiscard]] filament::math::float3 to_filament(Float3 v) {
    return {v.x, v.y, v.z};
}

[[nodiscard]] filament::math::float4 to_filament(Float4 v) {
    return {v.x, v.y, v.z, v.w};
}

} // namespace

void bind_standard_surface(filament::MaterialInstance& instance) {
    using namespace render_tuning;
    instance.setParameter("normalStrength",
                          material_surface.normal_map_strength);
    instance.setParameter("surfaceSamplingTuning",
                          to_filament(material_surface.sampling));
    instance.setParameter("surfaceMetallicTuning",
                          to_filament(material_surface.metallic));
    instance.setParameter("surfaceDielectricTuning",
                          to_filament(material_surface.dielectric));
    instance.setParameter("surfaceReflectanceTuning",
                          to_filament(material_surface.reflectance));
    instance.setParameter("surfaceEmissionTuning",
                          to_filament(material_surface.emission));
    instance.setParameter("surfaceMaskedEmissionTuning",
                          to_filament(material_surface.masked_emission));
    instance.setParameter("surfaceWetnessTuning",
                          to_filament(material_surface.wetness));
    instance.setParameter("surfaceWetnessShape",
                          to_filament(Float2{material_surface.wetness_floor,
                                            material_surface.wet_gloss_scale}));
}

void bind_ground_relief(filament::MaterialInstance& instance) {
    instance.setParameter("groundReliefTuning",
                          to_filament(render_tuning::material_surface.ground_relief));
    instance.setParameter(
        "groundReliefCavityTuning",
        to_filament(render_tuning::material_surface.ground_relief_cavity));
}

void bind_window(filament::MaterialInstance& instance) {
    instance.setParameter("windowGlassTuning",
        to_filament(render_tuning::window_material.glass));
    instance.setParameter("windowEmissionTuning",
        to_filament(render_tuning::window_material.emission));
    instance.setParameter("windowEmissionColorTuning",
        to_filament(render_tuning::window_material.emission_color));
}

void bind_glass(filament::MaterialInstance& instance) {
    const auto tint = render_tuning::glass.tint;
    const auto absorption = render_tuning::glass.absorption;
    instance.setParameter(
        "glassTint", filament::math::float3{tint.x, tint.y, tint.z});
    instance.setParameter("transmission", render_tuning::glass.transmission);
    instance.setParameter(
        "absorption",
        filament::math::float3{absorption.x, absorption.y, absorption.z});
    instance.setParameter("ior", render_tuning::glass.ior);
    instance.setParameter("thickness", render_tuning::glass.thickness);
    instance.setParameter("roughness", render_tuning::glass.roughness);
    instance.setParameter(
        "normalStrength", render_tuning::material_surface.normal_map_strength);
}

void bind_sky(filament::MaterialInstance& instance) {
    instance.setParameter("skyAtmosphereTuning",
                          to_filament(render_tuning::atmosphere.shader_optical));
    instance.setParameter(
        "skyLowerHemisphereTuning",
        to_filament(render_tuning::atmosphere.shader_lower_hemisphere));
}

void bind_grass_visible(filament::MaterialInstance& instance) {
    instance.setParameter("grassAtlasTuning",
                          to_filament(render_tuning::grass.atlas_tuning));
    instance.setParameter("grassShadingTuning",
                          to_filament(render_tuning::grass.shading_tuning));
    instance.setParameter("grassTipTuning",
                          to_filament(render_tuning::grass.tip_tint));
    instance.setParameter("grassWetnessTuning",
                          to_filament(render_tuning::grass.wetness_tuning));
    instance.setParameter("grassWindTuning",
                          to_filament(render_tuning::grass.wind_tuning));
    const auto flower = render_tuning::grass.flower_tint_shape;
    instance.setParameter("flowerTintTuning",
        filament::math::float4{0.0f, flower.y, flower.z, flower.w});
}

void bind_grass_depth(filament::MaterialInstance& instance) {
    instance.setParameter("grassWindTuning",
                          to_filament(render_tuning::grass.wind_tuning));
}

void bind_clouds(filament::MaterialInstance& instance) {
    instance.setParameter("cloudShapeTuning",
                          to_filament(render_tuning::clouds.shader_shape));
    instance.setParameter("cloudMotionTuning",
                          to_filament(render_tuning::clouds.shader_motion));
    instance.setParameter("cloudDetailTuning",
                          to_filament(render_tuning::clouds.shader_detail));
    instance.setParameter(
        "cloudDensityLightingTuning",
        to_filament(render_tuning::clouds.shader_density_lighting));
}

void bind_volumetric_clouds(filament::MaterialInstance& instance) {
    instance.setParameter("cloudDensityTuning",
                          to_filament(render_tuning::volumetric_clouds.density_shape));
    instance.setParameter("cloudLightingTuning",
                          to_filament(render_tuning::volumetric_clouds.lighting));
    instance.setParameter("cloudMotionTuning",
                          to_filament(render_tuning::volumetric_clouds.motion));
}

void bind_godrays(filament::MaterialInstance& instance) {
    instance.setParameter("activeSamples",
        render_tuning::clamp_active_count(
            render_tuning::godrays.active_samples,
            render_tuning::godrays.maximum_samples));
    instance.setParameter("rayTuning",
                          to_filament(render_tuning::godrays.shader));
}

void bind_volumetric_fog(filament::MaterialInstance& instance) {
    instance.setParameter("lampVolumeTuning",
                          to_filament(render_tuning::volumetric_fog.lamp_volume));
    instance.setParameter("fogShaderTuning",
                          to_filament(render_tuning::volumetric_fog.shader));
    instance.setParameter("activeSteps",
        render_tuning::clamp_active_count(
            render_tuning::volumetric_fog.march_steps,
            render_tuning::shader_limits.volumetric_fog_steps));
}

void bind_ssgi(filament::MaterialInstance& instance) {
    instance.setParameter("ssgiStabilityTuning",
                          to_filament(render_tuning::ssgi.stability));
    instance.setParameter("ssgiRejectionTuning",
                          to_filament(render_tuning::ssgi.rejection));
    instance.setParameter("ssgiColorTuning",
                          to_filament(render_tuning::ssgi.color_rejection));
}

void bind_smoke(filament::MaterialInstance& instance) {
    instance.setParameter("particleMotionTuning",
                          to_filament(render_tuning::particles.smoke_motion));
    instance.setParameter("particleShapeTuning",
                          to_filament(render_tuning::particles.smoke_shape));
}

void bind_flame(filament::MaterialInstance& instance) {
    instance.setParameter("particleShapeTuning",
                          to_filament(render_tuning::particles.flame_shape));
}

void bind_glow(filament::MaterialInstance& instance) {
    instance.setParameter("particleShapeTuning",
                          to_filament(render_tuning::particles.glow_shape));
}

void bind_firefly(filament::MaterialInstance& instance) {
    instance.setParameter("particleMotionTuning",
                          to_filament(render_tuning::particles.firefly_motion));
}

void bind_heat_haze(filament::MaterialInstance& instance) {
    instance.setParameter("hazeShapeTuning",
                          to_filament(render_tuning::particles.heat_haze_shape));
}

void bind_rain(filament::MaterialInstance& instance) {
    instance.setParameter("rainShaderTuning",
                          to_filament(render_tuning::rain.streak_shading));
}

void bind_splash(filament::MaterialInstance& instance) {
    instance.setParameter("splashShapeTuning",
                          to_filament(render_tuning::rain.splash_shape));
    instance.setParameter("splashShadingTuning",
                          to_filament(render_tuning::rain.splash_shading));
}

void bind_lens_flare(filament::MaterialInstance& instance) {
    instance.setParameter("flareShaderTuning",
                          to_filament(render_tuning::lens_flare.shader));
}

} // namespace heaven::material_tuning
