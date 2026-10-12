/*
 * HeavenBench — centralized renderer tuning
 *
 * This header contains compile-time artistic, quality, workload, distance and
 * stability controls shared by the CPU renderer code and the parameters pushed
 * into Filament materials.  It deliberately contains no Filament, GLM, SDL or
 * engine types so engine and RHI code can both include it without creating a
 * dependency cycle.
 *
 * Mathematical identities, physical standards (for example ST.2084), mesh
 * topology and material pipeline state remain at their point of use.
 */
#ifndef HEAVENBENCH_SRC_RENDER_TUNING_H_
#define HEAVENBENCH_SRC_RENDER_TUNING_H_

#include <algorithm>
#include <array>
#include <cstdint>

namespace heaven::render_tuning {

struct Float2 {
    float x;
    float y;
};

struct Float3 {
    float x;
    float y;
    float z;
};

struct Float4 {
    float x;
    float y;
    float z;
    float w;
};

template <typename T>
[[nodiscard]] constexpr T clamp_active_count(T requested, T maximum) noexcept {
    return std::clamp(requested, T{1}, maximum);
}

struct FullscreenPassTuning {
    float scene_far_depth = 2000.0f;
    float camera_vertical_fov_degrees = 60.0f;
    float cloud_distance = 1500.0f;
    float cloud_half_fov_degrees = 80.0f;
    float godrays_distance = 1990.0f;
    float ssgi_distance = 1990.0f;
    float volumetric_cloud_distance = 1990.0f;
    float volumetric_fog_distance = 1995.0f;
    float splash_distance = 1985.0f;
    float uncullable_bounds_extent = 1.0e6f;
};

struct DepthTuning {
    int downsample = 2;
    float near_plane = 0.50f;
    float far_plane = 2000.0f;
    float vertical_fov_degrees = 60.0f;
};

struct HdrTuning {
    float fullscreen_bounds_extent = 1.0e6f;
};

struct IblTuning {
    uint32_t face_size = 256;
    uint8_t mip_levels = 9;
    Float4 cloud_projection{0.05f, 0.30f, 6.0f, 0.0f};
    Float4 cloud_shape{0.55f, 0.85f, 0.65f, 0.20f};
    Float4 sun_glow{48.0f, 0.25f, 8.0f, 0.06f};
    Float3 cloud_color{0.95f, 0.97f, 1.0f};
};

struct ShaderResourceLimits {
    int godray_samples = 64;
    int volumetric_fog_lights = 16;
    int volumetric_fog_steps = 32;
    int volumetric_cloud_steps = 128;
    int particle_fbm_octaves = 4;
    int rain_streaks = 60000;
    int rain_splashes = 4096;
    int grass_blade_segments = 8;
};

struct PostProcessTuning {
    float shared_exposure_ev = -0.25f;
    float sdr_exposure_ev = -0.50f;
    float night_temperature_shift = -0.50f;
    float night_saturation_scale = 0.60f;
    Float4 tonal_ranges{0.0f, 0.333f, 0.550f, 1.0f};

    Float4 ao{0.80f, 0.80f, 1.0f, 0.05f};
    int ao_ultra_slices = 8;
    int ao_ultra_steps_per_slice = 6;

    Float4 weather_fog_distances{12.0f, 600.0f, 0.80f, 0.02f};
    Float3 weather_fog_color{0.62f, 0.65f, 0.70f};

    Float4 ssr{0.20f, 0.01f, 25.0f, 1.50f};
    Float2 wet_ssr{35.0f, 1.30f};
    Float3 taa{0.12f, -1.0f, 0.20f};
    Float2 dynamic_light_range{0.50f, 60.0f};

    std::array<int, 3> bloom_resolution{384, 512, 1024};
    std::array<int, 3> bloom_levels{6, 8, 10};
};

struct LightingTuning {
    std::array<float, 4> probe_band_hours{0.0f, 7.5f, 13.0f, 18.5f};
    std::array<float, 4> time_band_start_hours{0.0f, 5.0f, 10.0f, 16.0f};
    float evening_end_hour = 21.0f;

    float environment_floor_lux = 800.0f;
    float reference_sun_lux = 110000.0f;
    // Keep the broad sky probe from washing out shaded surfaces in daylight.
    // The independent night floor preserves Moon and local-light balance.
    float environment_day_span_lux = 19200.0f;
    Float3 fallback_irradiance{0.20f, 0.23f, 0.30f};

    // Restore readable warm lamp pools on the decoded legacy masonry without
    // lifting the night environment or expanding punctual-light bounds.
    float enclosed_lamp_power_scale = 48.0f;
    // Golden hue referenced to the authored street source, normalized to red.
    // Keep open flames on their separate orange calibration.
    Float3 enclosed_lamp_color{1.0f, 0.71665f, 0.15f};

    float moon_daylight_cutoff_lux = 30000.0f;
    float moon_min_weight = 0.01f;
    float moon_elevation = 0.40f;
    Float3 moon_color{0.55f, 0.65f, 0.95f};
    float moon_intensity_lux = 2500.0f;

    Float2 bloom_strength{0.06f, 0.20f};
    float bloom_reference_strength = 0.30f;
    int bloom_levels = 8;
};

struct ShadowTuning {
    float sun_angular_radius_degrees = 3.0f;
    float sun_shadow_min_lux = 5500.0f;
    float sun_shadow_far = 150.0f;
    float cascade_near = 0.50f;
    float cascade_lambda = 0.90f;
    float point_falloff_cap = 12.0f;
    float point_constant_bias = 0.05f;
    float point_normal_bias = 1.0f;
};

struct SkyTuning {
    int sphere_latitudes = 24;
    int sphere_longitudes = 48;
    float sphere_radius = 1000.0f;
    float radiance_scale = 2.0e-4f;
    // Reduce high-Sun atmospheric radiance before it reaches the tone mapper's
    // white shoulder. Dawn, sunset, the solar disk and night sources retain
    // their existing calibration; the visible sky and IBL share this gain.
    float day_scattering_scale = 0.50f;
    Float2 day_scattering_elevation{0.10f, 0.50f};
    float high_altitude_extinction = 0.65f;
    float rayleigh = 1.0f;
    float turbidity = 1.6f;
    float mie_coefficient = 0.45f;
    float mie_g = 0.72f;
    float ozone = 0.08f;
    float sun_radius_degrees = 0.95f;
    float moon_radius_degrees = 0.28f;
    float sun_disk_radiance = 24000.0f;
    float horizon_glow = 0.08f;
    float moon_intensity = 2.2f;
};

struct AtmosphereTuning {
    float moon_direction_elevation = 0.45f;
    float sun_limb_darkening = 0.50f;
    float daylight_reference_lux = 110000.0f;
    float night_reference_lux = 18000.0f;
    Float3 multi_scattering_tint{0.025f, 0.030f, 0.040f};
    Float3 night_color{0.0f, 3.0e-4f, 7.5e-4f};
    Float2 contrast{4.0f, 0.22f};
    Float2 night_mix{0.35f, 0.65f};
    Float3 milky_way{1.5e-3f, 1.15f, 0.055f};
    Float2 lower_hemisphere_blend{-0.18f, 0.16f};
    Float3 lower_hemisphere_shape{0.16f, 0.30f, 0.10f};
    float mie_phase_ceiling = 0.22f;
    float cloud_extinction = 0.25f;
    Float4 shader_optical{2.0e-4f, 0.65f, 0.22f, 0.16f};
    Float4 shader_lower_hemisphere{-0.18f, 0.16f, 0.30f, 0.10f};
    Float4 ibl_sun_shape{0.92f, 1.08f, 0.32f, 0.78f};
    Float4 ibl_sun_glare{2.20f, 2.25f, 5.0f, 1.65f};
    Float4 ibl_sun_strength{1.80f, 0.90f, 0.18f, 0.025f};
    Float3 ibl_sun_hot_color{1.0f, 0.985f, 0.90f};
    Float3 ibl_sun_warm_color{1.0f, 0.76f, 0.30f};
    float ibl_sun_transmittance_floor = 0.58f;
};

struct GrassTuning {
    float variation_scale = 0.05f;
    float variation_strength = 0.25f;
    float flower_variation_strength = 0.08f;
    float lod_distance = 650.0f;
    float bounds_extent = 200.0f;

    Float4 atlas_tuning{0.80f, 0.30f, 0.72f, 1.08f};
    Float4 shading_tuning{0.62f, 1.80f, 0.0f, 0.92f};
    Float3 tip_tint{1.15f, 1.10f, 0.80f};
    Float4 wetness_tuning{0.55f, 0.35f, 0.55f, 0.65f};
    Float4 wind_tuning{1.50f, 0.70f, 0.30f, 0.15f};
    Float2 kind_thresholds{0.05f, 0.65f};
    Float4 strand_shape{0.30f, 1.40f, 0.40f, 0.50f};
    Float4 seedhead_shape{0.55f, 0.85f, 0.20f, 2.50f};
    Float2 normal_bend{0.55f, 0.835f};
    Float3 strand_tip_tint{1.20f, 1.10f, 0.65f};
    Float3 seedhead_tip_tint{1.40f, 1.40f, 1.20f};
    Float4 grass_diffuse_uv{1.0f, 1.0f, 0.0f, 0.0f};
    // Top-left quarter of grass_variation_d.dds. Its V scale is negative so
    // the card's normalized height (base=0, tip=1) remains suitable for wind.
    Float4 flower_diffuse_uv{0.25f, -0.50f, 0.0f, 0.50f};
    Float4 flower_card_shape{0.58f, 0.18f, 1.25f, 0.35f};
    // strength, tint ramp start/end along card height, minimum target brightness
    Float4 flower_tint_shape{0.95f, 0.20f, 0.55f, 0.72f};
    Float2 flower_palette_split{0.72f, 0.86f};
    Float3 flower_tint_yellow{1.00f, 0.70f, 0.16f};
    Float3 flower_tint_blue{0.24f, 0.42f, 1.00f};
    Float3 flower_tint_red{1.00f, 0.22f, 0.16f};
    float height_noise_scale = 300.0f;
    float height_noise_power = 2.0f;
    float width_noise_seed = 123.0f;
    float bend_noise_seed = 1230.0f;
    float rotation_noise_scale = 321.0f;
    float identity_noise_scale = 333.0f;
    Float2 wind_frequency{0.20f, 0.50f};
    float wind_phase_scale = 0.50f;
    float wind_response = 0.60f;
    float base_alpha_cutoff = 0.05f;

    int island1_blades = 160000;
    int island2_blades = 40000;
    int island3_blades = 26000;
    float flower_fraction = 0.002f;
};

struct CloudTuning {
    float legacy_height = -1000.0f;
    Float3 initial_sun_direction{0.4f, 0.3f, 0.8f};
    Float3 initial_sun_color{1.5f, 1.4f, 1.2f};
    Float4 shader_shape{0.60f, 0.10f, 0.15f, 0.80f};
    Float4 shader_motion{0.003f, 0.001f, 0.002f, 0.70f};
    Float4 shader_detail{1.30f, 0.0015f, 0.01f, 0.0f};
    Float2 shader_density_lighting{0.30f, 0.70f};
};

struct VolumetricCloudTuning {
    float deck_center_z = -560.0f;
    float deck_thickness = 320.0f;
    float coverage = 0.40f;
    float density_scale = 0.008f;
    float noise_scale = 0.00115f;
    float max_march_distance = 5000.0f;
    Float3 initial_sky_color{0.62f, 0.68f, 0.78f};
    Float4 density_shape{1500.0f, 0.42f, 0.82f, 38.0f};
    Float4 lighting{0.65f, -0.20f, 0.12f, 0.035f};
    Float4 motion{0.02f, 1.0f, 0.30f, 0.0f};
};

struct GodraysTuning {
    int maximum_samples = 64;
    int active_samples = 64;
    Float4 scattering{0.70f, 0.010f, 0.97f, 0.20f};
    Float4 shader{1500.0f, 1900.0f, 0.10f, 0.30f};
    Float3 initial_sun_color{1.5f, 1.4f, 1.2f};
};

struct VolumetricFogTuning {
    int maximum_lights = 16;
    int march_steps = 32;
    int noise_size = 64;
    int noise_filter_passes = 4;
    Float4 medium{0.005f, 250.0f, 1.50f, 1.0f};
    // Clear daylight keeps depth cues without veiling nearby architecture.
    // The existing night medium retains the local-light scattering balance.
    float day_density = 0.0015f;
    Float2 phase{0.35f, 0.50f};
    Float3 color{0.55f, 0.62f, 0.72f};
    float light_reach_multiplier = 1.50f;
    float minimum_light_distance = 0.50f;
    float light_flux_reference = 200000.0f;
    float ambient_reference_lux = 30000.0f;
    float sun_min_lux = 5000.0f;
    float sun_flux_reference = 110000.0f;
    Float4 shader{1500.0f, 0.30f, 8.0f, 0.50f};
    // Original light_volume_big: finite additive glow, separate from both
    // optical-axis flares and the medium's ray-marched illumination.
    float lamp_volume_radius = 3.0f;
    Float4 lamp_volume{0.1f, 1.0f, 1.0f, 0.0f}; // density, power, strength, reserved
    Float3 lamp_volume_color{0.119996f, 0.055651f, 0.0f};
};

struct SsgiTuning {
    float sample_radius = 0.035f;
    float base_intensity = 0.12f;
    Float2 fallback_texel_size{1.0f / 960.0f, 1.0f / 540.0f};
    Float4 stability{1500.0f, 0.80f, 0.035f, 1.50f};
    Float4 rejection{8.0f, 0.60f, 0.10f, 1.0e-4f};
    Float4 color_rejection{0.55f, 0.08f, 5.0f, 0.50f};
};

struct ParticleTuning {
    int minimum_slots = 8;
    int maximum_slots = 64;
    int flame_planes = 3;
    int firefly_slots = 24;
    Float4 firefly_geometry{0.04f, 0.0f, 0.60f, 3.0f};
    float flame_speed = 0.90f;
    float bounds_halo = 30.0f;
    Float2 heat_haze_fade{8.0f, 25.0f};

    Float4 smoke_motion{0.25f, 0.85f, 0.60f, 0.35f};
    Float4 smoke_shape{0.65f, 0.40f, 0.35f, 2.50f};
    Float4 flame_shape{1.10f, 3.0f, 0.35f, 0.60f};
    Float4 glow_shape{0.30f, 0.15f, 0.20f, 2.0f};
    Float4 firefly_motion{0.60f, 1.0f, 0.30f, 4.0f};
    Float4 heat_haze_shape{1.30f, 0.10f, 0.05f, 0.35f};
};

struct RainTuning {
    float fall_speed = 24.0f;
    Float4 streak_shading{0.03f, 0.35f, 0.52f, 0.70f};
    Float4 splash_shape{1.20f, 0.90f, 1.065f, 0.08f};
    Float4 splash_shading{0.06f, 0.35f, 0.70f, 0.60f};
};

struct LensFlareTuning {
    float minimum_bounds_radius = 2.0f;
    float bounds_radius_scale = 2.0f;
    Float4 shader{2000.0f, 0.75f, 1.25f, 0.75f};
};

struct WindArrowTuning {
    float shaft_length = 1.0f;
    float half_width = 0.03f;
    float bounds_extent = 100.0f;
    float minimum_direction_length = 1.0e-4f;
    float basis_parallel_threshold = 0.95f;
    std::array<Float3, 4> palette{{
        {1.0f, 0.20f, 0.20f},
        {0.20f, 1.0f, 0.20f},
        {0.20f, 0.40f, 1.0f},
        {1.0f, 0.90f, 0.20f},
    }};
};

struct MaterialSurfaceTuning {
    Float4 sampling{0.70f, 0.50f, 0.01f, 1.0f / 512.0f};
    Float4 metallic{2.0f, 0.85f, 0.20f, 0.85f};
    Float4 dielectric{0.30f, 0.65f, 0.98f, 0.20f};
    Float4 reflectance{0.45f, 0.95f, 0.30f, 0.04f};
    Float4 emission{1.0f, 4.0f, 0.05f, 0.20f};
    Float4 masked_emission{1.0f, 0.60f, 0.10f, 0.45f};
    Float4 wetness{0.30f, 0.55f, 0.30f, 0.04f};
    float wetness_floor = 0.45f;
    float wet_gloss_scale = 1.50f;
    // Global tangent-space normal-map amplitude. Values above one steepen the
    // authored XY slope while reconstructing a unit normal in the shader.
    float normal_map_strength = 1.50f;

    // Village-gravel shading, selected explicitly by SurfaceDisplacementPolicy.
    // x = authored tangent-normal strength; yz = dry roughness range;
    // w = dry dielectric reflectance floor. Wetness is applied afterwards and
    // retains the shared 0.30 roughness target above.
    Float4 ground_relief{1.25f, 0.62f, 0.82f, 0.30f};
    // x = coarse height mip used as the local mean; y = concavity strength;
    // z = maximum indirect-light occlusion. This affects Filament's ambient
    // term only, preserving physically evaluated direct light and shadows.
    Float3 ground_relief_cavity{2.0f, 2.00f, 0.18f};

    float texture_anisotropy = 8.0f;
    float tessellation_pom_scale = 0.15f;
    float height_pom_scale = 0.05f;
    float forced_emission_scale = 3.50f;
    Float3 forced_emission_tint{1.0f, 0.62f, 0.30f};
};

struct AdvancedMaterialTuning {
    Float2 clear_coat{1.0f, 0.10f};
    float anisotropy = 0.60f;
    Float3 anisotropy_direction{1.0f, 0.0f, 0.0f};
    Float3 foliage_subsurface{0.14f, 0.38f, 0.10f};
    Float3 generic_subsurface{0.60f, 0.45f, 0.30f};
    Float2 subsurface{0.50f, 12.0f};
    Float3 cloth_sheen{0.20f, 0.20f, 0.20f};
};

struct WindowMaterialTuning {
    Float4 glass{0.18f, 0.42f, 0.04f, 0.60f};
    Float4 emission{0.08f, 0.30f, 1.0f, 3.0f};
    Float4 emission_color{0.35f, 1.0f, 0.82f, 0.60f};
};

struct GlassTuning {
    Float3 tint{0.90f, 0.94f, 1.0f};
    float transmission = 0.55f;
    Float3 absorption{0.06f, 0.05f, 0.04f};
    float ior = 1.50f;
    float thickness = 0.50f;
    float roughness = 0.05f;
};

inline constexpr FullscreenPassTuning fullscreen{};
inline constexpr DepthTuning depth{};
inline constexpr HdrTuning hdr{};
inline constexpr IblTuning ibl{};
inline constexpr ShaderResourceLimits shader_limits{};
inline constexpr PostProcessTuning post_process{};
inline constexpr LightingTuning lighting{};
inline constexpr ShadowTuning shadows{};
inline constexpr SkyTuning sky{};
inline constexpr AtmosphereTuning atmosphere{};
inline constexpr GrassTuning grass{};
inline constexpr CloudTuning clouds{};
inline constexpr VolumetricCloudTuning volumetric_clouds{};
inline constexpr GodraysTuning godrays{};
inline constexpr VolumetricFogTuning volumetric_fog{};
inline constexpr SsgiTuning ssgi{};
inline constexpr ParticleTuning particles{};
inline constexpr RainTuning rain{};
inline constexpr LensFlareTuning lens_flare{};
inline constexpr WindArrowTuning wind_arrows{};
inline constexpr MaterialSurfaceTuning material_surface{};
inline constexpr AdvancedMaterialTuning advanced_material{};
inline constexpr WindowMaterialTuning window_material{};
inline constexpr GlassTuning glass{};

static_assert(fullscreen.scene_far_depth > fullscreen.godrays_distance);
static_assert(fullscreen.scene_far_depth > fullscreen.volumetric_fog_distance);
static_assert(post_process.bloom_resolution[0] < post_process.bloom_resolution[1]);
static_assert(post_process.bloom_resolution[1] < post_process.bloom_resolution[2]);
static_assert(post_process.ao_ultra_slices > 0 &&
              post_process.ao_ultra_steps_per_slice > 0);
static_assert(depth.downsample > 0 && depth.near_plane < depth.far_plane);
static_assert((ibl.face_size & (ibl.face_size - 1u)) == 0u);
static_assert((1u << (ibl.mip_levels - 1u)) == ibl.face_size);
static_assert(atmosphere.lower_hemisphere_blend.x <
              atmosphere.lower_hemisphere_blend.y);
static_assert(grass.lod_distance > 0.0f && grass.flower_fraction >= 0.0f &&
              grass.flower_fraction <= 1.0f);
static_assert(grass.flower_palette_split.x > 0.5f &&
              grass.flower_palette_split.x < grass.flower_palette_split.y &&
              grass.flower_palette_split.y < 1.0f);
static_assert(grass.flower_tint_shape.x >= 0.0f &&
              grass.flower_tint_shape.x <= 1.0f &&
              grass.flower_tint_shape.y < grass.flower_tint_shape.z &&
              grass.flower_tint_shape.w > 0.0f);
static_assert(godrays.active_samples <= godrays.maximum_samples);
static_assert(godrays.maximum_samples == shader_limits.godray_samples);
static_assert(volumetric_fog.maximum_lights == 16,
              "heaven_volumetric_fog.mat array capacity must remain in sync");
static_assert(volumetric_fog.maximum_lights ==
              shader_limits.volumetric_fog_lights);
static_assert(volumetric_fog.march_steps ==
              shader_limits.volumetric_fog_steps);
static_assert(shader_limits.rain_streaks > 0 &&
              shader_limits.rain_splashes > 0);
static_assert(shader_limits.grass_blade_segments > 0);
static_assert(material_surface.wetness_floor >= 0.0f &&
              material_surface.wetness_floor <= 1.0f);
static_assert(material_surface.normal_map_strength >= 1.0f &&
              material_surface.normal_map_strength <= 2.0f);
static_assert(wind_arrows.shaft_length > 0.0f &&
              wind_arrows.half_width > 0.0f);

} // namespace heaven::render_tuning

#endif // HEAVENBENCH_SRC_RENDER_TUNING_H_
