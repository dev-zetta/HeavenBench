/*
 * HeavenBench - shared analytic sky model
 */
#include "rhi/sky_atmosphere.h"
#include "render_tuning.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace heaven {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kSkyRadianceScale = render_tuning::sky.radiance_scale;
constexpr float kHighAltitudeExtinction =
    render_tuning::sky.high_altitude_extinction;

float saturate(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

float smoothstep(float a, float b, float x) {
    x = saturate((x - a) / (b - a));
    return x * x * (3.0f - 2.0f * x);
}

glm::vec3 exp_vec(glm::vec3 v) {
    return {std::exp(v.x), std::exp(v.y), std::exp(v.z)};
}

glm::vec3 pow_vec(glm::vec3 v, float p) {
    return {std::pow(std::max(v.x, 0.0f), p),
            std::pow(std::max(v.y, 0.0f), p),
            std::pow(std::max(v.z, 0.0f), p)};
}

float rayleigh_phase(float cos_theta) {
    return (3.0f / 16.0f) * (1.0f + cos_theta * cos_theta);
}

float hg_phase(float cos_theta, glm::vec2 params) {
    const float one_minus_g2 = 2.0f - params.x;
    const float denom = std::pow(std::max(params.x + params.y * cos_theta, 1e-4f), 1.5f);
    return 0.25f * one_minus_g2 / denom;
}

float sun_fade(float sun_z) {
    constexpr float cutoff = 1.0f - 96.0f / 180.0f;
    return saturate(1.0f - std::exp(-(cutoff + sun_z) / 1.5f));
}

glm::vec3 mul(glm::vec3 a, glm::vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

} // namespace

glm::vec3 sky_moon_direction(const LightingParams& lighting) {
    return moon_lighting(lighting).direction;
}

SkyAtmosphereParams make_sky_atmosphere_params(
    const LightingParams& lighting)
{
    SkyAtmosphereParams p;

    const float rayleigh = render_tuning::sky.rayleigh;
    const float turbidity = render_tuning::sky.turbidity;
    const float mie_coefficient = render_tuning::sky.mie_coefficient;
    const float mie_g = render_tuning::sky.mie_g;
    const float ozone = render_tuning::sky.ozone;

    const glm::vec3 lambda(680e-9f, 550e-9f, 440e-9f);
    const float n = 1.0003f;
    const float N = 2.545e25f;
    const float pn = 0.035f;
    const float rayleigh_const =
        (8.0f * std::pow(kPi, 3.0f) * std::pow(n * n - 1.0f, 2.0f)) /
        (3.0f * N) * ((6.0f + 3.0f * pn) / (6.0f - 7.0f * pn));

    p.depth_r = glm::vec3(
        rayleigh_const / std::pow(lambda.x, 4.0f),
        rayleigh_const / std::pow(lambda.y, 4.0f),
        rayleigh_const / std::pow(lambda.z, 4.0f)) * 8000.0f * rayleigh;

    const float mie_base = 2.0e-5f * turbidity * mie_coefficient;
    const float mie_alpha = 1.3f;
    p.depth_m = glm::vec3(
        mie_base * std::pow(lambda.x / 550e-9f, -mie_alpha),
        mie_base * std::pow(lambda.y / 550e-9f, -mie_alpha),
        mie_base * std::pow(lambda.z / 550e-9f, -mie_alpha)) * 1200.0f;
    p.ozone = glm::vec3(0.0f, ozone * 0.1f, 0.0f);
    p.mie_phase_params = {1.0f + mie_g * mie_g, -2.0f * mie_g};

    const float sun_z = glm::normalize(lighting.sun_dir).z;
    const float fade = sun_fade(sun_z);
    p.sun_radiance = std::max(0.0f, lighting.sun_intensity) * fade;

    // Use a cinematic apparent disk: physically the sun is ~0.266 degrees in
    // radius, but Heaven is an airborne fantasy benchmark and the visible sky
    // needs a readable solar object rather than a sub-pixel astronomy dot.
    constexpr float sun_radius_rad =
        render_tuning::sky.sun_radius_degrees * kPi / 180.0f;
    p.sun_cos_radius = std::cos(sun_radius_rad);
    p.sun_limb_darkening = render_tuning::atmosphere.sun_limb_darkening;
    const float normalized_daylight =
        saturate(p.sun_radiance /
                 render_tuning::atmosphere.daylight_reference_lux);
    p.sun_disk_radiance =
        render_tuning::sky.sun_disk_radiance * normalized_daylight;

    const auto day_elevation = render_tuning::sky.day_scattering_elevation;
    const float day_weight = smoothstep(day_elevation.x, day_elevation.y, sun_z);
    p.sun_radiance *= std::lerp(
        1.0f, render_tuning::sky.day_scattering_scale, day_weight);

    const glm::vec3 total_ext = p.depth_r + p.depth_m + p.ozone;
    const glm::vec3 iso_scatter = p.depth_r * (3.0f / 16.0f)
                                + p.depth_m * (1.0f / (4.0f * kPi));
    const auto multi = render_tuning::atmosphere.multi_scattering_tint;
    p.multi_scattering = glm::vec3(multi.x, multi.y, multi.z) * iso_scatter /
                         glm::max(total_ext, glm::vec3(1e-6f));
    p.horizon_glow = render_tuning::sky.horizon_glow;
    p.contrast =
        1.0f +
        (1.0f - saturate(
            std::abs(sun_z) * render_tuning::atmosphere.contrast.x)) *
        render_tuning::atmosphere.contrast.y;

    const float night = 1.0f - smoothstep(-0.20f, 0.10f, sun_z);
    const auto night_color = render_tuning::atmosphere.night_color;
    p.night_color = glm::vec3(night_color.x, night_color.y, night_color.z) *
                    (render_tuning::atmosphere.night_mix.x +
                     night * render_tuning::atmosphere.night_mix.y);

    p.moon_dir = sky_moon_direction(lighting);
    constexpr float moon_radius_rad =
        render_tuning::sky.moon_radius_degrees * kPi / 180.0f;
    p.moon_cos_radius = std::cos(moon_radius_rad);
    p.moon_sin_radius = std::sin(moon_radius_rad);
    p.moon_intensity = moon_lighting(lighting).weight *
        render_tuning::sky.moon_intensity;

    // Equatorial-to-galactic rows from the Filament sample, composed with a
    // light local sidereal spin driven by the benchmark time of day.
    const float angle = (lighting.time_of_day / 24.0f) * 2.0f * kPi;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const glm::mat3 spin(
        c,  s, 0.0f,
       -s,  c, 0.0f,
        0.0f, 0.0f, 1.0f);
    const glm::mat3 eq_to_gal(
        -0.054876f,  0.494109f, -0.867666f,
        -0.873437f, -0.444830f, -0.198076f,
        -0.483835f,  0.746982f,  0.455984f);
    const glm::mat3 mw = eq_to_gal * spin;
    p.milky_way_row0 = {mw[0][0], mw[1][0], mw[2][0]};
    p.milky_way_row1 = {mw[0][1], mw[1][1], mw[2][1]};
    p.milky_way_row2 = {mw[0][2], mw[1][2], mw[2][2]};
    const auto milky_way = render_tuning::atmosphere.milky_way;
    p.milky_way_control = {
        night * milky_way.x, milky_way.y, milky_way.z};

    return p;
}

glm::vec3 evaluate_sky_atmosphere(
    const SkyAtmosphereParams& p, glm::vec3 view_dir,
    glm::vec3 sun_dir, float cloud_density)
{
    glm::vec3 V = glm::normalize(view_dir);
    glm::vec3 L = glm::normalize(sun_dir);
    const auto lower_blend_range =
        render_tuning::atmosphere.lower_hemisphere_blend;
    const auto lower_shape =
        render_tuning::atmosphere.lower_hemisphere_shape;
    const float lower_blend =
        1.0f - smoothstep(lower_blend_range.x, lower_blend_range.y, V.z);
    const float lifted_z = glm::mix(std::max(V.z, lower_shape.x),
        lower_shape.y + std::min(std::max(-V.z, 0.0f), 1.0f) * lower_shape.z,
        lower_blend);
    const glm::vec3 A = glm::normalize(glm::vec3(V.x, V.y, lifted_z));

    const float cos_theta = glm::dot(A, L);
    const float r_phase = rayleigh_phase(cos_theta * 0.5f + 0.5f);
    const float m_phase =
        std::min(hg_phase(cos_theta, p.mie_phase_params),
                 render_tuning::atmosphere.mie_phase_ceiling);

    const float zenith_cos = std::clamp(A.z, lower_shape.x, 1.0f);
    const float zenith_angle = std::acos(zenith_cos);
    const float zenith_deg = zenith_angle * (180.0f / kPi);
    const float optical_mass =
        1.0f / std::max(zenith_cos + 0.15f * std::pow(
            std::max(93.885f - zenith_deg, 1e-3f), -1.253f), 1e-3f);

    const glm::vec3 total_ext = p.depth_r + p.depth_m + p.ozone;
    const glm::vec3 transmittance = exp_vec(-total_ext * optical_mass * kHighAltitudeExtinction);
    const glm::vec3 scattering = p.depth_r * r_phase
                               + p.depth_m * m_phase
                               + p.multi_scattering;
    const glm::vec3 in_scatter = p.sun_radiance * (scattering / glm::max(total_ext, glm::vec3(1e-6f)));
    glm::vec3 color = mul(in_scatter, glm::vec3(1.0f) - transmittance);

    const float horizon_mix = saturate(std::pow(1.0f - L.z, 5.0f)) * p.horizon_glow;
    const glm::vec3 horizon_glow = glm::sqrt(glm::max(mul(in_scatter, transmittance), glm::vec3(0.0f)));
    color = mul(color, glm::mix(glm::vec3(1.0f), horizon_glow, horizon_mix));
    const glm::vec3 night_radiance = p.night_color *
        smoothstep(0.25f, -0.08f, L.z) * smoothstep(-0.08f, 0.35f, V.z);

    const float angle = std::sqrt(std::max(0.0f, 2.0f * (1.0f - cos_theta)));
    const float disk_angle = std::acos(std::clamp(p.sun_cos_radius, -1.0f, 1.0f));
    const auto sun_shape = render_tuning::atmosphere.ibl_sun_shape;
    const auto sun_glare = render_tuning::atmosphere.ibl_sun_glare;
    const auto sun_strength = render_tuning::atmosphere.ibl_sun_strength;
    const float disk = 1.0f - smoothstep(
        disk_angle * sun_shape.x, disk_angle * sun_shape.y, angle);
    const float core = 1.0f - smoothstep(
        disk_angle * sun_shape.z, disk_angle * sun_shape.w, angle);
    const float aureole = std::exp(-std::pow(
        angle / std::max(disk_angle * sun_glare.x, 1e-5f), sun_glare.y));
    const float veil = std::exp(-std::pow(
        angle / std::max(disk_angle * sun_glare.z, 1e-5f), sun_glare.w));
    if (disk > 0.0f || veil > 1e-3f) {
        const glm::vec3 disk_transmittance = glm::max(
            transmittance,
            glm::vec3(render_tuning::atmosphere.ibl_sun_transmittance_floor));
        const auto hot = render_tuning::atmosphere.ibl_sun_hot_color;
        const auto warm = render_tuning::atmosphere.ibl_sun_warm_color;
        const glm::vec3 hot_core(hot.x, hot.y, hot.z);
        const glm::vec3 warm_glare(warm.x, warm.y, warm.z);
        color += p.sun_disk_radiance * disk_transmittance *
            (hot_core * (core * sun_strength.x + disk * sun_strength.y) +
             warm_glare *
                 (aureole * sun_strength.z + veil * sun_strength.w));
    }

    const float cloud_transmission = 1.0f - saturate(cloud_density) *
        render_tuning::atmosphere.cloud_extinction;
    color *= cloud_transmission;
    color = pow_vec(color, p.contrast);
    // Keep the IBL's night floor in the same scene-linear units as the visible
    // sky. The solar-lux conversion applies only to daylight scattering.
    return glm::clamp(color * kSkyRadianceScale +
        night_radiance * cloud_transmission, 0.0f, 1.0f);
}

} // namespace heaven
