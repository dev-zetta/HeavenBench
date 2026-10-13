// Feature: in-ui-configuration,: Controls seed from the Working_Configuration
/*
 * HeavenBench — Property test for Settings Catalog control seeding.
 *
 * Feature: in-ui-configuration
 *   •: Controls seed from the Working_Configuration
 *
 *
 * (controls seed from the Working_Configuration):
 *   For any BenchConfig `cfg`, for every CatalogEntry in settings_catalog(),
 *   `entry.read(cfg)` equals the actual value of the governed BenchConfig field
 *   that entry reads. OverlayUI seeds each Options_Panel control from the
 *   Working_Configuration by calling `entry.read(working)`,
 *   and refreshes those controls after a successful load, so
 *   `read()` MUST be a faithful, transformation-free projection of the governed
 *   field — otherwise a panel would open showing a value the configuration does
 *   not hold.
 *
 *   The test generates a BenchConfig with every governed field randomized
 *   across valid ranges (finite floats so the variant value-equality below is
 *   well-defined; QualityLevel drawn from {Low, Medium, High}; arbitrary ints,
 *   bools, and strings). For each catalog entry it computes the EXPECTED
 *   SettingValue by reading the governed field DIRECTLY (an explicit per-id
 *   switch mirroring the catalog's read() targets in settings_catalog.cpp) and
 *   asserts `entry.read(cfg)` is variant-equal to it. Variant equality requires
 *   both the same active alternative (bool / int / float / QualityLevel /
 *   std::string) AND an equal contained value, so this catches both a wrong
 *   field and a wrong type.
 *
 * The Settings Catalog is GPU-free (settings_catalog.{h,cpp} depend only on
 * engine/types.h, engine/cli_parser.h, and config_edit.h — no Filament), so
 * this runs on the host. The file self-registers with the RapidCheck harness in
 * test_main.cpp via heaven::testing::Register, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" requirement (raise via RC_PARAMS=max_success=…).
 *
 * NOTE: this test deliberately defines its own self-contained BenchConfig
 * generator inline. The shared tests/bench_config_gen.h generator is introduced
 * by a LATER task (4.3) and is not depended on here.
 */
#include <cmath>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include <rapidcheck.h>

#include "engine/render_presets.h"
#include "rhi/overlay/settings_catalog.h"

// Mirror the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using heaven::BenchConfig;
using heaven::CatalogEntry;
using heaven::QualityLevel;
using heaven::SettingId;
using heaven::SettingValue;

// Draw a finite float so the variant value-equality assertion is well-defined
// (NaN never compares equal to itself, which would be a spurious failure
// unrelated to read()'s behaviour). read() returns the field verbatim with no
// transformation, so any finite magnitude is a valid seed value.
[[nodiscard]] float gen_finite_float() {
    return *rc::gen::suchThat<float>([](float f) { return std::isfinite(f); });
}

[[nodiscard]] QualityLevel gen_quality_level() {
    return *rc::gen::element(QualityLevel::Low, QualityLevel::Medium,
                             QualityLevel::High);
}

// Build a BenchConfig with every catalog-governed field independently
// randomized across valid ranges. Fields the catalog does not govern are left
// at their defaults; only the governed fields matter to.
[[nodiscard]] BenchConfig gen_governed_config() {
    BenchConfig cfg;

    // ── Camera (Live; not persisted) ──
    cfg.features.physical_camera = *rc::gen::arbitrary<bool>();
    cfg.physical_camera.aperture      = gen_finite_float();
    cfg.physical_camera.shutter_speed = gen_finite_float();
    cfg.physical_camera.sensitivity   = gen_finite_float();

    // ── Environment (Live) ──
    cfg.time_of_day              = gen_finite_float();
    cfg.bloom_strength           = gen_finite_float();
    cfg.weather.rain_intensity   = gen_finite_float();
    cfg.weather.dry_down_seconds = gen_finite_float();
    cfg.features.rain            = *rc::gen::arbitrary<bool>();
    cfg.features.wet_reflections = *rc::gen::arbitrary<bool>();
    cfg.features.sky             = *rc::gen::arbitrary<bool>();

    // ── Tessellation (Reload) ──
    cfg.tessellation.enabled  = *rc::gen::arbitrary<bool>();
    cfg.tessellation.scale    = gen_finite_float();
    cfg.tessellation.factor   = gen_finite_float();
    cfg.tessellation.distance = gen_finite_float();

    // ── Settings (Reload) ──
    cfg.width                = *rc::gen::arbitrary<int>();
    cfg.height               = *rc::gen::arbitrary<int>();
    cfg.fullscreen           = *rc::gen::arbitrary<bool>();
    cfg.hdr                  = *rc::gen::arbitrary<bool>();
    cfg.hdr_peak_nits        = gen_finite_float();
    cfg.hdr_paper_white_nits = gen_finite_float();
    cfg.cloth_sim_hz         = gen_finite_float();

    // ── Quality — feature toggles ──
    cfg.features.grass               = *rc::gen::arbitrary<bool>();
    cfg.features.particles           = *rc::gen::arbitrary<bool>();
    cfg.features.god_rays            = *rc::gen::arbitrary<bool>();
    cfg.features.volumetric_fog      = *rc::gen::arbitrary<bool>();
    cfg.features.clouds              = *rc::gen::arbitrary<bool>();
    cfg.features.lens_flares         = *rc::gen::arbitrary<bool>();
    cfg.features.volumetric_clouds   = *rc::gen::arbitrary<bool>();
    cfg.features.ssgi                = *rc::gen::arbitrary<bool>();
    cfg.features.massive_foliage     = *rc::gen::arbitrary<bool>();
    cfg.features.ground_relief       = *rc::gen::arbitrary<bool>();
    cfg.features.soft_shadows        = *rc::gen::arbitrary<bool>();
    cfg.features.contact_shadows     = *rc::gen::arbitrary<bool>();
    cfg.features.point_light_shadows = *rc::gen::arbitrary<bool>();
    cfg.features.reflections         = *rc::gen::arbitrary<bool>();
    cfg.wind_debug                   = *rc::gen::arbitrary<bool>();
    cfg.features.msaa                = *rc::gen::arbitrary<bool>();
    cfg.features.bloom               = *rc::gen::arbitrary<bool>();
    cfg.features.ambient_occlusion   = *rc::gen::arbitrary<bool>();
    cfg.features.taa                 = *rc::gen::arbitrary<bool>();
    cfg.features.depth_of_field      = *rc::gen::arbitrary<bool>();
    cfg.features.refraction          = *rc::gen::arbitrary<bool>();
    cfg.features.clear_coat          = *rc::gen::arbitrary<bool>();
    cfg.features.anisotropic         = *rc::gen::arbitrary<bool>();
    cfg.features.subsurface          = *rc::gen::arbitrary<bool>();
    cfg.features.cloth               = *rc::gen::arbitrary<bool>();
    cfg.features.transparent_shadows = *rc::gen::arbitrary<bool>();
    cfg.features.colored_penumbra    = *rc::gen::arbitrary<bool>();
    cfg.features.specular_aa         = *rc::gen::arbitrary<bool>();
    cfg.heat_haze_enabled            = *rc::gen::arbitrary<bool>();

    // ── Quality — knobs ──
    cfg.quality.soft_shadow_level    = gen_quality_level();
    cfg.quality.contact_shadow_level = gen_quality_level();
    cfg.quality.shadow_light_budget  = *rc::gen::arbitrary<int>();
    cfg.quality.cloud_quality        = gen_quality_level();
    cfg.quality.ssgi_quality         = gen_quality_level();
    cfg.quality.ao_quality           = gen_quality_level();
    cfg.quality.bloom_quality        = gen_quality_level();
    cfg.quality.dof_quality          = gen_quality_level();
    cfg.quality.taa_quality          = gen_quality_level();
    cfg.quality.foliage_density      = gen_finite_float();
    cfg.quality.msaa_samples         = *rc::gen::arbitrary<int>();
    cfg.quality.sun_shadow_map_size  = *rc::gen::arbitrary<int>();
    cfg.quality.point_shadow_map_size = *rc::gen::arbitrary<int>();
    cfg.material_workflow = *rc::gen::arbitrary<bool>()
        ? heaven::MaterialWorkflow::MetallicRoughnessCompat
        : heaven::MaterialWorkflow::SpecularGlossinessExperimental;
    cfg.quality_preset_label         = *rc::gen::arbitrary<std::string>();
    cfg.cpu_stress.enabled           = *rc::gen::arbitrary<bool>();
    cfg.cpu_stress.workers           = *rc::gen::arbitrary<int>();
    cfg.cpu_stress.duration_seconds  = gen_finite_float();

    // ── Sound ──
    cfg.music.enabled = *rc::gen::arbitrary<bool>();
    cfg.music.volume  = gen_finite_float();

    return cfg;
}

// The EXPECTED value a control seeds from: the governed BenchConfig field read
// DIRECTLY. This explicit switch mirrors, one-to-one, the read() targets in
// settings_catalog.cpp; the property asserts entry.read(cfg) equals this. Each
// arm constructs the SettingValue with the SAME alternative read() returns, so
// variant equality verifies both the field and its type.
[[nodiscard]] SettingValue expected_seed(SettingId id, const BenchConfig& cfg) {
    switch (id) {
        // ── Camera ──
        case SettingId::PhysicalCameraEnabled: return cfg.features.physical_camera;
        case SettingId::CameraAperture:        return cfg.physical_camera.aperture;
        case SettingId::CameraShutter:         return cfg.physical_camera.shutter_speed;
        case SettingId::CameraIso:             return cfg.physical_camera.sensitivity;

        // ── Environment ──
        case SettingId::TimeOfDay:             return cfg.time_of_day;
        case SettingId::RainIntensity:         return cfg.weather.rain_intensity;
        case SettingId::DryDownSeconds:        return cfg.weather.dry_down_seconds;
        case SettingId::FeatureRain:           return cfg.features.rain;
        case SettingId::FeatureWetReflections: return cfg.features.wet_reflections;
        case SettingId::FeatureSky:            return cfg.features.sky;

        // ── Tessellation ──
        case SettingId::TessEnabled:  return cfg.tessellation.enabled;
        case SettingId::TessScale:    return cfg.tessellation.scale;
        case SettingId::TessFactor:   return cfg.tessellation.factor;
        case SettingId::TessDistance: return cfg.tessellation.distance;

        // ── Settings ──
        case SettingId::Width:             return cfg.width;
        case SettingId::Height:            return cfg.height;
        case SettingId::Fullscreen:        return cfg.fullscreen;
        case SettingId::FrameGeneration:   return cfg.frame_generation;
        case SettingId::Hdr:               return cfg.hdr;
        case SettingId::HdrPeakNits:       return cfg.hdr_peak_nits;
        case SettingId::HdrPaperWhiteNits: return cfg.hdr_paper_white_nits;
        case SettingId::ClothSimHz:        return cfg.cloth_sim_hz;

        // ── Quality — feature toggles ──
        case SettingId::TextureQuality:           return cfg.texture_quality;
        case SettingId::FeatureGrass:             return cfg.features.grass;
        case SettingId::FeatureParticles:         return cfg.features.particles;
        case SettingId::FeatureGodRays:           return cfg.features.god_rays;
        case SettingId::FeatureVolumetricFog:     return cfg.features.volumetric_fog;
        case SettingId::FeatureClouds:            return cfg.features.clouds;
        case SettingId::FeatureLensFlares:        return cfg.features.lens_flares;
        case SettingId::FeatureVolumetricClouds:  return cfg.features.volumetric_clouds;
        case SettingId::FeatureSsgi:              return cfg.features.ssgi;
        case SettingId::FeatureMassiveFoliage:    return cfg.features.massive_foliage;
        case SettingId::FeatureGroundRelief:      return cfg.features.ground_relief;
        case SettingId::FeatureSoftShadows:       return cfg.features.soft_shadows;
        case SettingId::FeatureContactShadows:    return cfg.features.contact_shadows;
        case SettingId::FeaturePointLightShadows: return cfg.features.point_light_shadows;
        case SettingId::FeatureReflections:       return cfg.features.reflections;
        case SettingId::WindDebug:                return cfg.wind_debug;
        case SettingId::FeatureMsaa:              return cfg.features.msaa;
        case SettingId::FeatureBloom:             return cfg.features.bloom;
        case SettingId::BloomStrength:            return cfg.bloom_strength;
        case SettingId::FeatureAmbientOcclusion:  return cfg.features.ambient_occlusion;
        case SettingId::FeatureTaa:               return cfg.features.taa;
        case SettingId::FeatureDepthOfField:      return cfg.features.depth_of_field;
        case SettingId::FeatureRefraction:        return cfg.features.refraction;
        case SettingId::FeatureClearCoat:         return cfg.features.clear_coat;
        case SettingId::FeatureAnisotropic:       return cfg.features.anisotropic;
        case SettingId::FeatureSubsurface:        return cfg.features.subsurface;
        case SettingId::FeatureCloth:             return cfg.features.cloth;
        case SettingId::FeatureTransparentShadows: return cfg.features.transparent_shadows;
        case SettingId::FeatureColoredPenumbra:   return cfg.features.colored_penumbra;
        case SettingId::FeatureSpecularAa:        return cfg.features.specular_aa;
        case SettingId::FeatureGpuInstancing:     return cfg.features.gpu_instancing;
        case SettingId::FeatureGpuSkinning:       return cfg.features.gpu_skinning;
        case SettingId::HeatHazeEnabled:          return cfg.heat_haze_enabled;

        // ── Quality — knobs ──
        case SettingId::SoftShadowLevel:    return cfg.quality.soft_shadow_level;
        case SettingId::ContactShadowLevel: return cfg.quality.contact_shadow_level;
        case SettingId::ShadowLightBudget:  return cfg.quality.shadow_light_budget;
        case SettingId::CloudQuality:       return cfg.quality.cloud_quality;
        case SettingId::SsgiQuality:        return cfg.quality.ssgi_quality;
        case SettingId::AoQuality:          return cfg.quality.ao_quality;
        case SettingId::BloomQuality:       return cfg.quality.bloom_quality;
        case SettingId::DofQuality:         return cfg.quality.dof_quality;
        case SettingId::TaaQuality:         return cfg.quality.taa_quality;
        case SettingId::FoliageDensity:     return cfg.quality.foliage_density;
        case SettingId::MsaaSamples:        return cfg.quality.msaa_samples;
        case SettingId::SunShadowMapSize:   return cfg.quality.sun_shadow_map_size;
        case SettingId::PointShadowMapSize: return cfg.quality.point_shadow_map_size;
        case SettingId::RenderPreset:       return heaven::detect_render_preset(cfg);
        case SettingId::MaterialWorkflow:   return cfg.material_workflow;
        case SettingId::CpuStressEnabled:   return cfg.cpu_stress.enabled;
        case SettingId::CpuStressWorkers:   return cfg.cpu_stress.workers;
        case SettingId::CpuStressDuration:  return cfg.cpu_stress.duration_seconds;
        case SettingId::QualityPresetLabel: return cfg.quality_preset_label;

        // ── Sound ──
        case SettingId::MusicEnabled: return cfg.music.enabled;
        case SettingId::MusicVolume:  return cfg.music.volume;
        case SettingId::RenderTestScene: return cfg.render_test_scene;
    }
    // Unreachable: the switch is total over SettingId. Returning a neutral value
    // keeps the function total for the compiler.
    return false;
}

bool run_p2_controls_seed_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 2: controls seed from the "
        "Working_Configuration (Validates: Requirements 1.2, 6.8)",
        [] {
            const BenchConfig cfg = gen_governed_config();

            // Every entry's read() is a faithful, transformation-free
            // projection of the governed field.
            for (const CatalogEntry& entry : heaven::settings_catalog()) {
                const SettingValue actual   = entry.read(cfg);
                const SettingValue expected = expected_seed(entry.id, cfg);
                RC_ASSERT(actual == expected);
            }
        });
}

const heaven::testing::Register g_register_catalog_seed{
    &run_p2_controls_seed_property};

} // namespace
