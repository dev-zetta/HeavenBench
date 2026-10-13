/*
 * HeavenBench — Settings Catalog implementation (GPU-free)
 *
 * Builds the single catalog of editable settings once and exposes it by
 * reference. Each entry's apply_to() routes the committed value through the
 * exact shared engine/types.h helper the CLI and Config_File module use, so a
 * value validates identically regardless of source
 * validation parity). No Filament symbol is referenced.
 */
#include "rhi/overlay/settings_catalog.h"
#include "engine/render_test_scene_catalog.h"
#include "engine/render_presets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>

namespace heaven {

namespace {

// ── SettingValue extraction helpers ─────────────────────────────

// The variant a control commits may not always carry the field's natural
// alternative (a Combo can surface an int, a numeric InputText a float), so the
// accessors below coerce a SettingValue to the type the field needs. They are
// defensive: an alternative that cannot be sensibly coerced falls back to a
// neutral default rather than throwing, keeping apply_to total over any input
// the property tests throw at it.

[[nodiscard]] bool as_bool(const SettingValue& v) {
    if (const bool* b = std::get_if<bool>(&v)) return *b;
    if (const int* i = std::get_if<int>(&v)) return *i != 0;
    return false;
}

[[nodiscard]] int as_int(const SettingValue& v) {
    if (const int* i = std::get_if<int>(&v)) return *i;
    if (const QualityLevel* q = std::get_if<QualityLevel>(&v)) return static_cast<int>(*q);
    if (const float* f = std::get_if<float>(&v)) {
        if (std::isnan(*f)) return 0;
        return static_cast<int>(std::lround(*f));
    }
    if (const bool* b = std::get_if<bool>(&v)) return *b ? 1 : 0;
    return 0;
}

[[nodiscard]] float as_float(const SettingValue& v) {
    if (const float* f = std::get_if<float>(&v)) return *f;
    if (const int* i = std::get_if<int>(&v)) return static_cast<float>(*i);
    if (const QualityLevel* q = std::get_if<QualityLevel>(&v)) return static_cast<float>(static_cast<int>(*q));
    return 0.0f;
}

[[nodiscard]] QualityLevel as_quality(const SettingValue& v) {
    if (const QualityLevel* q = std::get_if<QualityLevel>(&v)) return *q;
    if (const int* i = std::get_if<int>(&v)) return static_cast<QualityLevel>(*i);
    return QualityLevel::Medium;
}

[[nodiscard]] MaterialWorkflow as_material_workflow(const SettingValue& v) {
    if (const MaterialWorkflow* workflow = std::get_if<MaterialWorkflow>(&v))
        return *workflow;
    return MaterialWorkflow::MetallicRoughnessCompat;
}

[[nodiscard]] TextureQuality as_texture_quality(const SettingValue& v) {
    if (const TextureQuality* quality = std::get_if<TextureQuality>(&v))
        return *quality;
    return TextureQuality::Original;
}

[[nodiscard]] std::string as_string(const SettingValue& v) {
    if (const std::string* s = std::get_if<std::string>(&v)) return *s;
    return std::string{};
}

// Render a float to the same textual token the Config_File module produces for
// a numeric value, then run it through the shared string-based parser. This
// keeps the UI's [0,1] clamp semantics byte-for-byte identical to both the CLI
// (`--rain-intensity` / `--music-volume`) and a loaded Config_File: the value
// is formatted, parsed, and clamped through the one shared helper, so a NaN or
// out-of-range magnitude lands exactly where the loader would put it.
[[nodiscard]] float parse_via_rain_intensity(float value) {
    const std::string token = std::format("{}", value);
    return parse_rain_intensity(token.c_str()).value;
}

[[nodiscard]] float parse_via_music_volume(float value) {
    const std::string token = std::format("{}", value);
    return parse_music_volume(token.c_str()).value;
}

// Presentation order follows the way users make benchmark decisions: broad
// presets and high-impact visual controls first, specialized material/runtime
// controls later. This is deliberately independent of SettingId's historical
// declaration order and of the order in which accessors are defined below.
constexpr std::array kCatalogUiOrder = {
    // Camera
    SettingId::PhysicalCameraEnabled,
    SettingId::CameraAperture,
    SettingId::CameraShutter,
    SettingId::CameraIso,

    // Environment
    SettingId::TimeOfDay,
    SettingId::FeatureSky,
    SettingId::FeatureRain,
    SettingId::RainIntensity,
    SettingId::DryDownSeconds,
    SettingId::FeatureWetReflections,

    // Tessellation
    SettingId::TessEnabled,
    SettingId::TessFactor,
    SettingId::TessDistance,
    SettingId::TessScale,

    // Display and runtime
    SettingId::Width,
    SettingId::Height,
    SettingId::Fullscreen,
    SettingId::FrameGeneration,
    SettingId::Hdr,
    SettingId::HdrPaperWhiteNits,
    SettingId::HdrPeakNits,
    SettingId::ClothSimHz,
    SettingId::RenderTestScene,

    // Rendering preset
    SettingId::RenderPreset,

    // Textures — independent of the rendering preset.
    SettingId::TextureQuality,

    // Image quality — high GPU impact
    SettingId::FeatureMsaa,
    SettingId::MsaaSamples,
    SettingId::FeatureTaa,
    SettingId::TaaQuality,
    SettingId::FeatureAmbientOcclusion,
    SettingId::AoQuality,
    SettingId::FeatureSsgi,
    SettingId::SsgiQuality,
    SettingId::FeatureReflections,

    // Scene complexity — high CPU / GPU impact
    SettingId::FeatureMassiveFoliage,
    SettingId::FeatureGroundRelief,
    SettingId::FeatureGrass,
    SettingId::FoliageDensity,
    SettingId::FeatureVolumetricClouds,
    SettingId::FeatureClouds,
    SettingId::CloudQuality,

    // Shadows — high GPU impact
    SettingId::FeatureSoftShadows,
    SettingId::SoftShadowLevel,
    SettingId::FeatureContactShadows,
    SettingId::ContactShadowLevel,
    SettingId::FeaturePointLightShadows,
    SettingId::ShadowLightBudget,
    SettingId::SunShadowMapSize,
    SettingId::PointShadowMapSize,
    SettingId::FeatureTransparentShadows,
    SettingId::FeatureColoredPenumbra,

    // Atmosphere and post-processing
    SettingId::FeatureVolumetricFog,
    SettingId::FeatureGodRays,
    SettingId::FeatureParticles,
    SettingId::HeatHazeEnabled,
    SettingId::FeatureLensFlares,
    SettingId::FeatureBloom,
    SettingId::BloomStrength,
    SettingId::BloomQuality,
    SettingId::FeatureDepthOfField,
    SettingId::DofQuality,

    // Materials and surface effects
    SettingId::MaterialWorkflow,
    SettingId::FeatureRefraction,
    SettingId::FeatureClearCoat,
    SettingId::FeatureAnisotropic,
    SettingId::FeatureSubsurface,
    SettingId::FeatureCloth,
    SettingId::FeatureSpecularAa,

    // GPU execution
    SettingId::FeatureGpuInstancing,
    SettingId::FeatureGpuSkinning,

    // CPU stress benchmark
    SettingId::CpuStressEnabled,
    SettingId::CpuStressWorkers,
    SettingId::CpuStressDuration,

    // Advanced / metadata
    SettingId::WindDebug,
    SettingId::QualityPresetLabel,

    // Audio
    SettingId::MusicEnabled,
    SettingId::MusicVolume,
};

[[nodiscard]] std::size_t catalog_ui_order(SettingId id) {
    const auto it = std::find(kCatalogUiOrder.begin(), kCatalogUiOrder.end(), id);
    return static_cast<std::size_t>(std::distance(kCatalogUiOrder.begin(), it));
}

[[nodiscard]] std::string_view section_for_setting(SettingId id) {
    switch (id) {
        case SettingId::PhysicalCameraEnabled:
        case SettingId::CameraAperture:
        case SettingId::CameraShutter:
        case SettingId::CameraIso:
            return "Physical exposure";

        case SettingId::TimeOfDay:
        case SettingId::FeatureSky:
            return "Lighting & sky";
        case SettingId::FeatureRain:
        case SettingId::RainIntensity:
        case SettingId::DryDownSeconds:
        case SettingId::FeatureWetReflections:
            return "Weather & wet surfaces";

        case SettingId::TessEnabled:
        case SettingId::TessFactor:
        case SettingId::TessDistance:
            return "Geometry detail";
        case SettingId::TessScale:
            return "Displacement";

        case SettingId::Width:
        case SettingId::Height:
        case SettingId::Fullscreen:
            return "Display";
        case SettingId::FrameGeneration:
            return "Frame generation";
        case SettingId::Hdr:
        case SettingId::HdrPaperWhiteNits:
        case SettingId::HdrPeakNits:
            return "HDR output";
        case SettingId::ClothSimHz:
            return "Simulation";
        case SettingId::RenderTestScene:
            return "Rendering test scene";

        case SettingId::RenderPreset:
            return "Rendering preset";

        case SettingId::TextureQuality:
            return "Textures - high VRAM impact";

        case SettingId::FeatureMsaa:
        case SettingId::MsaaSamples:
        case SettingId::FeatureTaa:
        case SettingId::TaaQuality:
        case SettingId::FeatureAmbientOcclusion:
        case SettingId::AoQuality:
        case SettingId::FeatureSsgi:
        case SettingId::SsgiQuality:
        case SettingId::FeatureReflections:
            return "Image quality - high GPU impact";

        case SettingId::FeatureMassiveFoliage:
        case SettingId::FeatureGroundRelief:
        case SettingId::FeatureGrass:
        case SettingId::FoliageDensity:
        case SettingId::FeatureVolumetricClouds:
        case SettingId::FeatureClouds:
        case SettingId::CloudQuality:
            return "Scene complexity - high CPU / GPU impact";

        case SettingId::FeatureSoftShadows:
        case SettingId::SoftShadowLevel:
        case SettingId::FeatureContactShadows:
        case SettingId::ContactShadowLevel:
        case SettingId::FeaturePointLightShadows:
        case SettingId::ShadowLightBudget:
        case SettingId::SunShadowMapSize:
        case SettingId::PointShadowMapSize:
        case SettingId::FeatureTransparentShadows:
        case SettingId::FeatureColoredPenumbra:
            return "Shadows - high GPU impact";

        case SettingId::FeatureVolumetricFog:
        case SettingId::FeatureGodRays:
        case SettingId::FeatureParticles:
        case SettingId::HeatHazeEnabled:
        case SettingId::FeatureLensFlares:
        case SettingId::FeatureBloom:
        case SettingId::BloomStrength:
        case SettingId::BloomQuality:
        case SettingId::FeatureDepthOfField:
        case SettingId::DofQuality:
            return "Atmosphere & post-processing";

        case SettingId::MaterialWorkflow:
        case SettingId::FeatureRefraction:
        case SettingId::FeatureClearCoat:
        case SettingId::FeatureAnisotropic:
        case SettingId::FeatureSubsurface:
        case SettingId::FeatureCloth:
        case SettingId::FeatureSpecularAa:
            return "Materials & surface effects";

        case SettingId::FeatureGpuInstancing:
        case SettingId::FeatureGpuSkinning:
            return "GPU execution";

        case SettingId::CpuStressEnabled:
        case SettingId::CpuStressWorkers:
        case SettingId::CpuStressDuration:
            return "CPU stress benchmark";

        case SettingId::WindDebug:
        case SettingId::QualityPresetLabel:
            return "Advanced / metadata";

        case SettingId::MusicEnabled:
        case SettingId::MusicVolume:
            return "Audio";
    }
    return {};
}

// ── Catalog construction ────────────────────────────────────────

// Built once on first use. `read`/`apply_to` are non-capturing lambdas (they
// convert to the plain function pointers CatalogEntry holds). Each apply_to
// routes through the shared validator named in the current Live/Reload map and
// returns the stored value.
const std::vector<CatalogEntry>& build_catalog() {
    static const std::vector<CatalogEntry> catalog = [] {
        std::vector<CatalogEntry> c;

        // ── Camera (Live; NOT persisted in config_schema) ──────
        c.push_back(CatalogEntry{
            SettingId::PhysicalCameraEnabled, Panel::Camera, Apply::Live,
            Widget::Checkbox, "Physical camera", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.physical_camera; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.physical_camera = as_bool(v);
                return cfg.features.physical_camera;
            }});
        c.push_back(CatalogEntry{
            SettingId::CameraAperture, Panel::Camera, Apply::Live,
            Widget::SliderFloat, "Aperture (f-number)",
            PhysicalCameraConfig::k_aperture_lo, PhysicalCameraConfig::k_aperture_hi,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.physical_camera.aperture; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.physical_camera.aperture = clamp_camera_aperture(as_float(v)).value;
                return cfg.physical_camera.aperture;
            }});
        c.push_back(CatalogEntry{
            SettingId::CameraShutter, Panel::Camera, Apply::Live,
            Widget::SliderFloat, "Shutter speed (s)",
            PhysicalCameraConfig::k_shutter_lo, PhysicalCameraConfig::k_shutter_hi,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.physical_camera.shutter_speed; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.physical_camera.shutter_speed = clamp_camera_shutter(as_float(v)).value;
                return cfg.physical_camera.shutter_speed;
            }});
        c.push_back(CatalogEntry{
            SettingId::CameraIso, Panel::Camera, Apply::Live,
            Widget::SliderFloat, "Sensitivity (ISO)",
            PhysicalCameraConfig::k_iso_lo, PhysicalCameraConfig::k_iso_hi,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.physical_camera.sensitivity; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.physical_camera.sensitivity = clamp_camera_iso(as_float(v)).value;
                return cfg.physical_camera.sensitivity;
            }});

        // ── Environment (Live) ─────────────────────────────────
        c.push_back(CatalogEntry{
            SettingId::TimeOfDay, Panel::Environment, Apply::Live,
            Widget::SliderFloat, "Time of day (h)", 0.0f, 24.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.time_of_day; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.time_of_day = wrap_24(as_float(v));
                return cfg.time_of_day;
            }});
        c.push_back(CatalogEntry{
            SettingId::RainIntensity, Panel::Environment, Apply::Live,
            Widget::SliderFloat, "Rain intensity", 0.0f, 1.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.weather.rain_intensity; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.weather.rain_intensity = parse_via_rain_intensity(as_float(v));
                return cfg.weather.rain_intensity;
            }});
        c.push_back(CatalogEntry{
            SettingId::DryDownSeconds, Panel::Environment, Apply::Live,
            Widget::SliderFloat, "Dry-down (s)", 0.5f, 30.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.weather.dry_down_seconds; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.weather.dry_down_seconds = clamp_dry_down_seconds(as_float(v));
                return cfg.weather.dry_down_seconds;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureRain, Panel::Environment, Apply::Live,
            Widget::Checkbox, "Rain", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.rain; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.rain = as_bool(v);
                return cfg.features.rain;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureWetReflections, Panel::Environment, Apply::Live,
            Widget::Checkbox, "Wet reflections", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.wet_reflections; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.wet_reflections = as_bool(v);
                return cfg.features.wet_reflections;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureSky, Panel::Environment, Apply::Live,
            Widget::Checkbox, "Sky", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.sky; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.sky = as_bool(v);
                return cfg.features.sky;
            }});

        // ── Tessellation (Reload; unbounded numerics) ──────────
        c.push_back(CatalogEntry{
            SettingId::TessEnabled, Panel::Tessellation, Apply::Reload,
            Widget::Checkbox, "Tessellation", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.tessellation.enabled; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.tessellation.enabled = as_bool(v);
                return cfg.tessellation.enabled;
            }});
        c.push_back(CatalogEntry{
            SettingId::TessScale, Panel::Tessellation, Apply::Reload,
            Widget::InputFloat, "Scale", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.tessellation.scale; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.tessellation.scale = as_float(v);
                return cfg.tessellation.scale;
            }});
        c.push_back(CatalogEntry{
            SettingId::TessFactor, Panel::Tessellation, Apply::Reload,
            Widget::InputFloat, "Factor", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.tessellation.factor; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.tessellation.factor = as_float(v);
                return cfg.tessellation.factor;
            }});
        c.push_back(CatalogEntry{
            SettingId::TessDistance, Panel::Tessellation, Apply::Reload,
            Widget::InputFloat, "Distance", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.tessellation.distance; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.tessellation.distance = as_float(v);
                return cfg.tessellation.distance;
            }});

        // ── Settings (Reload) ──────────────────────────────────
        c.push_back(CatalogEntry{
            SettingId::Width, Panel::Settings, Apply::Reload,
            Widget::InputInt, "Width", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.width; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.width = as_int(v);
                return cfg.width;
            }});
        c.push_back(CatalogEntry{
            SettingId::Height, Panel::Settings, Apply::Reload,
            Widget::InputInt, "Height", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.height; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.height = as_int(v);
                return cfg.height;
            }});
        c.push_back(CatalogEntry{
            SettingId::Fullscreen, Panel::Settings, Apply::Reload,
            Widget::Checkbox, "Fullscreen", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.fullscreen; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.fullscreen = as_bool(v);
                return cfg.fullscreen;
            }});
        c.push_back(CatalogEntry{
            SettingId::FrameGeneration, Panel::Settings, Apply::Live,
            Widget::Combo, "Frame generation", 0.0f, 2.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.frame_generation; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                if (const auto* mode = std::get_if<FrameGenerationMode>(&v)) {
                    if (*mode == FrameGenerationMode::Off || *mode == FrameGenerationMode::Fsr3 ||
                        *mode == FrameGenerationMode::Dlss) cfg.frame_generation = *mode;
                }
                return cfg.frame_generation;
            }});
        c.push_back(CatalogEntry{
            SettingId::Hdr, Panel::Settings, Apply::Reload,
            Widget::Checkbox, "HDR output", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.hdr; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.hdr = as_bool(v);
                return cfg.hdr;
            }});
        c.push_back(CatalogEntry{
            SettingId::HdrPeakNits, Panel::Settings, Apply::Reload,
            Widget::InputFloat, "HDR peak (nits)", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.hdr_peak_nits; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.hdr_peak_nits = as_float(v);
                return cfg.hdr_peak_nits;
            }});
        c.push_back(CatalogEntry{
            SettingId::HdrPaperWhiteNits, Panel::Settings, Apply::Reload,
            Widget::InputFloat, "HDR paper white (nits)", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.hdr_paper_white_nits; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.hdr_paper_white_nits = as_float(v);
                return cfg.hdr_paper_white_nits;
            }});
        c.push_back(CatalogEntry{
            SettingId::ClothSimHz, Panel::Settings, Apply::Reload,
            Widget::InputFloat, "Cloth sim rate (Hz)", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.cloth_sim_hz; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.cloth_sim_hz = as_float(v);
                return cfg.cloth_sim_hz;
            }});

        // ── Quality — Live feature toggles ─────────────────────
        c.push_back(CatalogEntry{
            SettingId::FeatureGrass, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Grass", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.grass; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.grass = as_bool(v);
                return cfg.features.grass;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureParticles, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Particles", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.particles; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.particles = as_bool(v);
                return cfg.features.particles;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureGodRays, Panel::Quality, Apply::Live,
            Widget::Checkbox, "God rays", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.god_rays; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.god_rays = as_bool(v);
                return cfg.features.god_rays;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureVolumetricFog, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Volumetric fog", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.volumetric_fog; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.volumetric_fog = as_bool(v);
                return cfg.features.volumetric_fog;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureClouds, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Clouds", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.clouds; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.clouds = as_bool(v);
                return cfg.features.clouds;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureLensFlares, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Lens flares", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.lens_flares; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.lens_flares = as_bool(v);
                return cfg.features.lens_flares;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureVolumetricClouds, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Volumetric clouds", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.volumetric_clouds; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.volumetric_clouds = as_bool(v);
                return cfg.features.volumetric_clouds;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureSsgi, Panel::Quality, Apply::Live,
            Widget::Checkbox, "SSGI", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.ssgi; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.ssgi = as_bool(v);
                return cfg.features.ssgi;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureMassiveFoliage, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Massive foliage", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.massive_foliage; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.massive_foliage = as_bool(v);
                return cfg.features.massive_foliage;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureGroundRelief, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Village ground relief", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.ground_relief; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.ground_relief = as_bool(v);
                return cfg.features.ground_relief;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureSoftShadows, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Soft shadows", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.soft_shadows; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.soft_shadows = as_bool(v);
                return cfg.features.soft_shadows;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureContactShadows, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Contact shadows", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.contact_shadows; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.contact_shadows = as_bool(v);
                return cfg.features.contact_shadows;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeaturePointLightShadows, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Point-light shadows", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.point_light_shadows; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.point_light_shadows = as_bool(v);
                return cfg.features.point_light_shadows;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureReflections, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Reflections (SSR)", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.reflections; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.reflections = as_bool(v);
                return cfg.features.reflections;
            }});
        c.push_back(CatalogEntry{
            SettingId::WindDebug, Panel::Quality, Apply::Live,
            Widget::Checkbox, "Wind debug arrows", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.wind_debug; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.wind_debug = as_bool(v);
                return cfg.wind_debug;
            }});

        // ── Quality — Reload feature toggles ───────────────────
        c.push_back(CatalogEntry{
            SettingId::FeatureBloom, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Bloom", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.bloom; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.bloom = as_bool(v);
                return cfg.features.bloom;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureAmbientOcclusion, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Ambient occlusion", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.ambient_occlusion; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.ambient_occlusion = as_bool(v);
                return cfg.features.ambient_occlusion;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureTaa, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Temporal anti-aliasing", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.taa; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.taa = as_bool(v);
                return cfg.features.taa;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureDepthOfField, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Depth of field", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.depth_of_field; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.depth_of_field = as_bool(v);
                return cfg.features.depth_of_field;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureMsaa, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "MSAA", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.msaa; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.msaa = as_bool(v);
                return cfg.features.msaa;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureRefraction, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Refraction", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.refraction; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.refraction = as_bool(v);
                return cfg.features.refraction;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureClearCoat, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Clear coat", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.clear_coat; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.clear_coat = as_bool(v);
                return cfg.features.clear_coat;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureAnisotropic, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Anisotropic", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.anisotropic; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.anisotropic = as_bool(v);
                return cfg.features.anisotropic;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureSubsurface, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Subsurface", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.subsurface; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.subsurface = as_bool(v);
                return cfg.features.subsurface;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureCloth, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Cloth / sheen", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.cloth; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.cloth = as_bool(v);
                return cfg.features.cloth;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureTransparentShadows, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Transparent shadows", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.transparent_shadows; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.transparent_shadows = as_bool(v);
                return cfg.features.transparent_shadows;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureColoredPenumbra, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Colored penumbra", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.colored_penumbra; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.colored_penumbra = as_bool(v);
                return cfg.features.colored_penumbra;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureSpecularAa, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Specular AA", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.specular_aa; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.specular_aa = as_bool(v);
                return cfg.features.specular_aa;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureGpuInstancing, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "GPU instancing", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.gpu_instancing; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.gpu_instancing = as_bool(v);
                return cfg.features.gpu_instancing;
            }});
        c.push_back(CatalogEntry{
            SettingId::FeatureGpuSkinning, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "GPU skinning", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.features.gpu_skinning; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.features.gpu_skinning = as_bool(v);
                return cfg.features.gpu_skinning;
            }});
        c.push_back(CatalogEntry{
            SettingId::HeatHazeEnabled, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "Heat haze", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.heat_haze_enabled; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                // Keep heat_haze_enabled and features.heat_haze in lockstep,
                // exactly as --no-heat-haze and the config_schema() key do.
                const bool b = as_bool(v);
                cfg.heat_haze_enabled  = b;
                cfg.features.heat_haze = b;
                return cfg.heat_haze_enabled;
            }});

        // ── Quality — Live quality knobs ───────────────────────
        c.push_back(CatalogEntry{
            SettingId::SoftShadowLevel, Panel::Quality, Apply::Live,
            Widget::Combo, "Soft-shadow quality", 0.0f, 2.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.soft_shadow_level; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality.soft_shadow_level = clamp_quality_level(as_int(v)).value;
                return cfg.quality.soft_shadow_level;
            }});
        c.push_back(CatalogEntry{
            SettingId::ContactShadowLevel, Panel::Quality, Apply::Live,
            Widget::Combo, "Contact-shadow quality", 0.0f, 2.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.contact_shadow_level; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality.contact_shadow_level = clamp_quality_level(as_int(v)).value;
                return cfg.quality.contact_shadow_level;
            }});
        c.push_back(CatalogEntry{
            SettingId::ShadowLightBudget, Panel::Quality, Apply::Live,
            Widget::SliderInt, "Shadow-casting light budget",
            0.0f, static_cast<float>(QualityKnobs{}.shadow_light_budget_max),
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.shadow_light_budget; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality.shadow_light_budget =
                    clamp_quality_shadow_light_budget(as_int(v), cfg.quality).value;
                return cfg.quality.shadow_light_budget;
            }});

        // ── Quality — Reload quality knobs ─────────────────────
        c.push_back(CatalogEntry{
            SettingId::CloudQuality, Panel::Quality, Apply::Reload,
            Widget::Combo, "Cloud quality", 0.0f, 2.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.cloud_quality; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality.cloud_quality = clamp_quality_level(as_int(v)).value;
                return cfg.quality.cloud_quality;
            }});
        c.push_back(CatalogEntry{
            SettingId::SsgiQuality, Panel::Quality, Apply::Reload,
            Widget::Combo, "SSGI quality", 0.0f, 2.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.ssgi_quality; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality.ssgi_quality = clamp_quality_level(as_int(v)).value;
                return cfg.quality.ssgi_quality;
            }});
        c.push_back(CatalogEntry{
            SettingId::AoQuality, Panel::Quality, Apply::Reload,
            Widget::Combo, "Ambient occlusion quality", 0.0f, 2.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.ao_quality; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality.ao_quality =
                    clamp_quality_level(static_cast<int>(as_quality(v))).value;
                return cfg.quality.ao_quality;
            }});
        c.push_back(CatalogEntry{
            SettingId::BloomQuality, Panel::Quality, Apply::Reload,
            Widget::Combo, "Bloom quality", 0.0f, 2.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.bloom_quality; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality.bloom_quality =
                    clamp_quality_level(static_cast<int>(as_quality(v))).value;
                return cfg.quality.bloom_quality;
            }});
        c.push_back(CatalogEntry{
            SettingId::BloomStrength, Panel::Quality, Apply::Live,
            Widget::SliderFloat, "Bloom strength", 0.0f, 1.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.bloom_strength; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.bloom_strength = clamp_bloom_strength(as_float(v)).value;
                return cfg.bloom_strength;
            }});
        c.push_back(CatalogEntry{
            SettingId::DofQuality, Panel::Quality, Apply::Reload,
            Widget::Combo, "Depth of field quality", 0.0f, 2.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.dof_quality; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality.dof_quality =
                    clamp_quality_level(static_cast<int>(as_quality(v))).value;
                return cfg.quality.dof_quality;
            }});
        c.push_back(CatalogEntry{
            SettingId::TaaQuality, Panel::Quality, Apply::Reload,
            Widget::Combo, "TAA history quality", 0.0f, 2.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.taa_quality; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality.taa_quality =
                    clamp_quality_level(static_cast<int>(as_quality(v))).value;
                return cfg.quality.taa_quality;
            }});
        c.push_back(CatalogEntry{
            SettingId::FoliageDensity, Panel::Quality, Apply::Reload,
            Widget::SliderFloat, "Foliage density",
            1.0f, QualityKnobs{}.foliage_density_max,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.foliage_density; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality.foliage_density =
                    clamp_quality_foliage_density(as_float(v), cfg.quality).value;
                return cfg.quality.foliage_density;
            }});
        c.push_back(CatalogEntry{
            SettingId::MsaaSamples, Panel::Quality, Apply::Reload,
            Widget::Combo, "MSAA samples", 1.0f, 4.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality.msaa_samples; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                // Reject-and-retain semantics: an out-of-set request keeps the
                // prior count, exactly as the CLI and Config_File loader do.
                cfg.quality.msaa_samples =
                    apply_msaa_samples(as_int(v), cfg.quality.msaa_samples).value;
                return cfg.quality.msaa_samples;
            }});
        c.push_back(CatalogEntry{
            SettingId::SunShadowMapSize, Panel::Quality, Apply::Reload,
            Widget::InputInt, "Sun shadow map", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue {
                return cfg.quality.sun_shadow_map_size;
            },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                const int value = as_int(v);
                if (value == 1024 || value == 2048 || value == 4096)
                    cfg.quality.sun_shadow_map_size = value;
                return cfg.quality.sun_shadow_map_size;
            }});
        c.push_back(CatalogEntry{
            SettingId::PointShadowMapSize, Panel::Quality, Apply::Reload,
            Widget::InputInt, "Point shadow map", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue {
                return cfg.quality.point_shadow_map_size;
            },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                const int value = as_int(v);
                if (value == 256 || value == 512 || value == 1024)
                    cfg.quality.point_shadow_map_size = value;
                return cfg.quality.point_shadow_map_size;
            }});
        c.push_back(CatalogEntry{
            SettingId::RenderPreset, Panel::Quality, Apply::Reload,
            Widget::Combo, "Rendering preset", 0.0f, 4.0f,
            [](const BenchConfig& cfg) -> SettingValue {
                return detect_render_preset(cfg);
            },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                const auto preset = std::get<RenderPreset>(v);
                apply_render_preset(cfg, preset);
                return preset;
            }});
        c.push_back(CatalogEntry{
            SettingId::TextureQuality, Panel::Quality, Apply::Reload,
            Widget::Combo, "Texture quality", 0.0f, 2.0f,
            [](const BenchConfig& cfg) -> SettingValue {
                return cfg.texture_quality;
            },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.texture_quality = as_texture_quality(v);
                return cfg.texture_quality;
            }});
        c.push_back(CatalogEntry{
            SettingId::RenderTestScene, Panel::Settings, Apply::Reload,
            Widget::Combo, "Scene", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.render_test_scene; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                const auto& id = std::get<std::string>(v);
                if (id.empty()) cfg.render_test_scene.clear();
                else static_cast<void>(apply_render_test_scene_defaults(cfg, id));
                return cfg.render_test_scene;
            }});
        c.push_back(CatalogEntry{
            SettingId::MaterialWorkflow, Panel::Quality, Apply::Reload,
            Widget::Combo, "Material workflow", 0.0f, 1.0f,
            [](const BenchConfig& cfg) -> SettingValue {
                return cfg.material_workflow;
            },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.material_workflow = as_material_workflow(v);
                return cfg.material_workflow;
            }});
        c.push_back(CatalogEntry{
            SettingId::CpuStressEnabled, Panel::Quality, Apply::Reload,
            Widget::Checkbox, "CPU stress phase", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue {
                return cfg.cpu_stress.enabled;
            },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.cpu_stress.enabled = as_bool(v);
                return cfg.cpu_stress.enabled;
            }});
        c.push_back(CatalogEntry{
            SettingId::CpuStressWorkers, Panel::Quality, Apply::Reload,
            Widget::InputInt, "CPU stress workers (0=auto)", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue {
                return cfg.cpu_stress.workers;
            },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.cpu_stress.workers = std::clamp(as_int(v), 0, 256);
                return cfg.cpu_stress.workers;
            }});
        c.push_back(CatalogEntry{
            SettingId::CpuStressDuration, Panel::Quality, Apply::Reload,
            Widget::InputFloat, "CPU stress seconds", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue {
                return cfg.cpu_stress.duration_seconds;
            },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                const float value = as_float(v);
                if (std::isfinite(value) && value > 0.0f)
                    cfg.cpu_stress.duration_seconds = value;
                return cfg.cpu_stress.duration_seconds;
            }});
        c.push_back(CatalogEntry{
            SettingId::QualityPresetLabel, Panel::Quality, Apply::Live,
            Widget::InputText, "Quality preset label", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.quality_preset_label; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.quality_preset_label = as_string(v);
                return cfg.quality_preset_label;
            }});

        // ── Sound ──────────────────────────────────────────────
        c.push_back(CatalogEntry{
            SettingId::MusicEnabled, Panel::Sound, Apply::Reload,
            Widget::Checkbox, "Music", 0.0f, 0.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.music.enabled; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.music.enabled = as_bool(v);
                return cfg.music.enabled;
            }});
        c.push_back(CatalogEntry{
            SettingId::MusicVolume, Panel::Sound, Apply::Live,
            Widget::SliderFloat, "Music volume", 0.0f, 1.0f,
            [](const BenchConfig& cfg) -> SettingValue { return cfg.music.volume; },
            [](BenchConfig& cfg, const SettingValue& v) -> SettingValue {
                cfg.music.volume = parse_via_music_volume(as_float(v));
                return cfg.music.volume;
            }});

        std::stable_sort(c.begin(), c.end(),
                         [](const CatalogEntry& lhs, const CatalogEntry& rhs) {
                             return catalog_ui_order(lhs.id) <
                                    catalog_ui_order(rhs.id);
                         });
        return c;
    }();
    return catalog;
}

} // namespace

const std::vector<CatalogEntry>& settings_catalog() {
    return build_catalog();
}

const CatalogEntry& catalog_entry(SettingId id) {
    // Index by SettingId on first use for O(1) lookup. The catalog is total
    // over SettingId, so every id resolves; a missing id would be a build bug.
    static const std::unordered_map<SettingId, const CatalogEntry*> index = [] {
        std::unordered_map<SettingId, const CatalogEntry*> m;
        for (const CatalogEntry& e : settings_catalog()) {
            m.emplace(e.id, &e);
        }
        return m;
    }();
    return *index.at(id);
}

std::string_view catalog_section(SettingId id) {
    return section_for_setting(id);
}

std::optional<PerformanceImpact> catalog_performance_impact(SettingId id) noexcept {
    switch (id) {
        case SettingId::PhysicalCameraEnabled:
        case SettingId::CameraAperture:
        case SettingId::CameraShutter:
        case SettingId::CameraIso:
            return PerformanceImpact{"CPU / GPU: Minimal", "Exposure is inexpensive parameter math. Aperture can change depth-of-field work when that separate effect is enabled."};
        case SettingId::TimeOfDay:
            return PerformanceImpact{"CPU: Low | GPU: Scene dependent", "Switches prefiltered lighting probes. Visible lights, shadows and atmospheric work can differ between day and night."};
        case SettingId::FeatureSky:
            return PerformanceImpact{"GPU: Low", "Draws the visible sky. Environment lighting remains active when the sky sphere is disabled."};
        case SettingId::FeatureRain:
        case SettingId::RainIntensity:
            return PerformanceImpact{"GPU: Medium | CPU: Low", "Rain and splash particles add transparent overdraw while rain is falling. More rain can increase that work."};
        case SettingId::DryDownSeconds:
            return PerformanceImpact{"CPU / GPU: Minimal", "Changes how long wet surfaces persist; it does not increase rain particle counts or reflection samples."};
        case SettingId::FeatureWetReflections:
            return PerformanceImpact{"GPU: High on wet surfaces", "Wetness adds reflection work while surfaces are wet. Screen-space reflections still require the separate Reflections setting."};
        case SettingId::TessEnabled:
        case SettingId::TessFactor:
            return PerformanceImpact{"GPU: Very high | Loading: Very high", "Subdivision and bounded surface preparation can create millions of triangles. Cached loads avoid rebuilding topology, but rendering still processes the detailed geometry."};
        case SettingId::TessDistance:
            return PerformanceImpact{"GPU: Usually little change above zero | Loading: May increase", "Positive values change displacement fade, not geometry draw distance or base subdivision. Changed values can rebuild the prepared cache. Zero disables displacement and bounded surface preparation, retaining neutral subdivision."};
        case SettingId::TessScale:
            return PerformanceImpact{"GPU: Usually little change above zero | Loading: May increase", "Positive values change relief amplitude rather than base subdivision. Changed values can rebuild the prepared cache. Zero disables relief and bounded surface preparation, retaining neutral subdivision."};
        case SettingId::Width:
        case SettingId::Height:
            return PerformanceImpact{"GPU / VRAM: High as resolution rises", "More output pixels increase shading, screen-space effects and render-target memory. Doubling both dimensions creates four times as many pixels."};
        case SettingId::Fullscreen:
            return PerformanceImpact{"GPU: Depends on pixel resolution", "The actual drawable resolution determines the rendering workload; fullscreen alone has no fixed cost."};
        case SettingId::FrameGeneration:
            return PerformanceImpact{"GPU / VRAM: Provider dependent", "Interpolation adds GPU work and presentation latency. It does not accelerate simulation or real rendering. Requested benchmark tours are diagnostic and unscored."};
        case SettingId::Hdr:
            return PerformanceImpact{"GPU: Low to medium | VRAM: Medium", "HDR uses floating-point scene/UI targets and a final output conversion. Supported display output is required."};
        case SettingId::HdrPeakNits:
        case SettingId::HdrPaperWhiteNits:
            return PerformanceImpact{"CPU / GPU: Minimal", "Changes HDR brightness parameters without increasing render resolution or sample counts."};
        case SettingId::ClothSimHz:
            return PerformanceImpact{"CPU: Medium as rate rises", "A higher cloth simulation rate performs more physics steps. Visible cloth geometry is still updated for rendering."};
        case SettingId::RenderTestScene:
            return PerformanceImpact{"CPU / GPU / VRAM: Scene dependent", "Object counts, materials, effects and texture usage determine the workload. Small feature scenes isolate one rendering behavior."};
        case SettingId::RenderPreset:
            return PerformanceImpact{"CPU / GPU / VRAM: Varies by preset", "A preset changes several workloads together. Inspect the individual settings for their estimated impact."};
        case SettingId::TextureQuality:
            return PerformanceImpact{"VRAM: High to very high | Loading: High", "Larger texture tiers increase upload time, memory and bandwidth. Exceeding available VRAM can also reduce frame rate."};
        case SettingId::FeatureMsaa:
        case SettingId::MsaaSamples:
            return PerformanceImpact{"GPU: High | VRAM: High", "Multisampling increases sample storage, bandwidth and resolve work. The selected sample count matters; TAA is a separate pass."};
        case SettingId::FeatureTaa:
        case SettingId::TaaQuality:
            return PerformanceImpact{"GPU: Medium | VRAM: Medium", "Temporal anti-aliasing adds history buffers and a reconstruction pass. Higher quality increases filtering work."};
        case SettingId::FeatureAmbientOcclusion:
        case SettingId::AoQuality:
            return PerformanceImpact{"GPU: High | VRAM: Medium", "GTAO samples and filters scene depth. Quality and resolution increase cost; contact shadows also require full-resolution structure depth."};
        case SettingId::FeatureSsgi:
        case SettingId::SsgiQuality:
            return PerformanceImpact{"GPU: High | VRAM: High", "Screen-space indirect lighting gathers previous-frame scene color and keeps full-resolution color history. Higher quality adds samples."};
        case SettingId::FeatureReflections:
            return PerformanceImpact{"GPU: High | VRAM: Medium", "Screen-space reflections trace scene depth and filter reflected color. Reflective screen coverage and resolution affect cost."};
        case SettingId::FeatureMassiveFoliage:
            return PerformanceImpact{"GPU: Potentially very high | Memory: High | Loading: High", "Dense blade budgets increase geometry memory and masked overdraw. Repeated islands multiply shading work; shared buffers save duplicate geometry storage, not visible blade processing."};
        case SettingId::FeatureGrass:
            return PerformanceImpact{"GPU: High | Memory: Medium to high", "More grass increases blade geometry and masked overdraw. Grass is generated during loading; wind animation runs on the GPU."};
        case SettingId::FoliageDensity:
            return PerformanceImpact{"GPU: Potentially very high | Memory: Medium to high", "Higher density increases blade budgets and masked overdraw, especially with massive foliage enabled. Shared geometry buffers do not reduce shading for repeated visible blades."};
        case SettingId::FeatureGroundRelief:
            return PerformanceImpact{"GPU: High | Loading: High", "Village ground relief adds displaced geometry and surface shading. Generated relief geometry requires the Tessellation master switch; detailed prepared surfaces also consume more memory."};
        case SettingId::FeatureVolumetricClouds:
        case SettingId::CloudQuality:
            return PerformanceImpact{"GPU: High", "Volumetric clouds ray march across the screen. More ray steps and larger output resolution increase work."};
        case SettingId::FeatureClouds:
            return PerformanceImpact{"GPU: Low to medium", "The legacy cloud deck adds transparent sky layers. It is separate from the more expensive volumetric cloud effect."};
        case SettingId::FeatureSoftShadows:
            return PerformanceImpact{"GPU: High", "Enabling soft sun shadows uses four cascades instead of one smaller map, plus blocker searches and filtering. Scene coverage and shadow-map size affect cost."};
        case SettingId::SoftShadowLevel:
            return PerformanceImpact{"GPU: Scene dependent", "Changes penumbra width without increasing the configured sampling count. A wider soft-shadow search can affect scene-dependent work."};
        case SettingId::FeatureContactShadows:
        case SettingId::ContactShadowLevel:
            return PerformanceImpact{"GPU: Very high | VRAM: Medium", "Contact visibility traces depth across the Sun's angular extent. It also requires full-resolution structure depth; quality increases per-ray steps."};
        case SettingId::FeaturePointLightShadows:
        case SettingId::ShadowLightBudget:
            return PerformanceImpact{"GPU: High to very high | VRAM: High", "Each shadow-casting light adds shadow-map rendering. More lights, visible casters and larger maps increase the workload."};
        case SettingId::SunShadowMapSize:
            return PerformanceImpact{"GPU: High | VRAM: High", "Larger cascade shadow maps increase rasterization and storage. Doubling map width and height quadruples the texel count."};
        case SettingId::PointShadowMapSize:
            return PerformanceImpact{"GPU: High | VRAM: High", "Larger per-light shadow maps increase rasterization and storage, multiplied by the active shadow-casting light budget."};
        case SettingId::FeatureTransparentShadows:
            return PerformanceImpact{"GPU: Medium", "Masked foliage and other transparent casters require extra shadow sampling and overdraw where those materials are present."};
        case SettingId::FeatureColoredPenumbra:
            return PerformanceImpact{"GPU: Low", "Adds color math to eligible material shadow shading. It does not add shadow maps or contact rays."};
        case SettingId::FeatureVolumetricFog:
            return PerformanceImpact{"GPU: Medium to high", "Depth-aware fog evaluates visible scene lights. Light count, screen coverage and output resolution affect cost."};
        case SettingId::FeatureGodRays:
            return PerformanceImpact{"GPU: Medium", "Light shafts sample shared scene depth and add a screen-space pass. Cost rises with output resolution."};
        case SettingId::FeatureParticles:
            return PerformanceImpact{"GPU: Medium | CPU: Low to medium", "Smoke, flames and glow add simulation and transparent overdraw. Cost depends on the visible emitter population."};
        case SettingId::HeatHazeEnabled:
            return PerformanceImpact{"GPU: Medium | VRAM: Medium", "Visible heat haze captures scene color and composites refracting quads. Hidden or absent haze emitters do not need that capture."};
        case SettingId::FeatureLensFlares:
            return PerformanceImpact{"GPU: Low", "Adds small transparent sprites around visible bright lights. Cost depends on the visible flare population."};
        case SettingId::FeatureBloom:
        case SettingId::BloomQuality:
            return PerformanceImpact{"GPU: Medium | VRAM: Medium", "Bloom filters bright pixels through multiple image levels. Higher quality increases its resolution and filtering work."};
        case SettingId::BloomStrength:
            return PerformanceImpact{"CPU / GPU: Minimal change", "Changes bloom intensity. Lowering strength is not the same as disabling the bloom pass."};
        case SettingId::FeatureDepthOfField:
        case SettingId::DofQuality:
            return PerformanceImpact{"GPU: Medium to high | VRAM: Medium", "Depth of field adds depth-aware blur and intermediate images. Resolution and selected blur quality determine work."};
        case SettingId::MaterialWorkflow:
            return PerformanceImpact{"GPU: Low change", "Changes material parameter interpretation on eligible surfaces. Specialized material models keep their own shading paths."};
        case SettingId::FeatureRefraction:
            return PerformanceImpact{"GPU: Medium | VRAM: Medium", "Visible glass samples screen-space scene color. Cost depends on glass coverage and the available color buffers."};
        case SettingId::FeatureClearCoat:
            return PerformanceImpact{"GPU: Low to medium", "Adds a second specular layer on eligible coated materials. Cost follows their visible screen coverage."};
        case SettingId::FeatureAnisotropic:
            return PerformanceImpact{"GPU: Low to medium", "Adds directional specular shading on eligible materials. It does not add scene geometry or a fullscreen pass."};
        case SettingId::FeatureSubsurface:
            return PerformanceImpact{"GPU: Low to medium", "Adds translucency shading on eligible materials. Cost depends on their visible screen coverage."};
        case SettingId::FeatureCloth:
            return PerformanceImpact{"GPU: Low to medium", "Selects cloth/sheen shading on eligible materials. Cloth physics has its own simulation-rate setting."};
        case SettingId::FeatureSpecularAa:
            return PerformanceImpact{"GPU: Low", "Adds local normal-variation math to reduce sparkling highlights. It does not add temporal history or multisample buffers."};
        case SettingId::FeatureGpuInstancing:
            return PerformanceImpact{"CPU: Usually saves work | GPU: Low overhead", "Batches repeated static meshes to reduce draw preparation. Geometry and visible instance counts remain the same."};
        case SettingId::FeatureGpuSkinning:
            return PerformanceImpact{"CPU: Usually saves work | GPU: Low overhead", "Moves animated vertex skinning to the GPU and avoids recurring CPU vertex uploads. Savings depend on animated geometry."};
        case SettingId::CpuStressEnabled:
        case SettingId::CpuStressWorkers:
            return PerformanceImpact{"CPU: Very high during stress phase", "The separate CPU benchmark loads independent scene worlds per worker. More workers increase CPU and memory demand."};
        case SettingId::CpuStressDuration:
            return PerformanceImpact{"CPU: Longer sustained workload", "Extends the CPU stress measurement duration without changing each worker's scene complexity."};
        case SettingId::WindDebug:
            return PerformanceImpact{"CPU / GPU: Low", "Adds cloth-wind diagnostic arrows. This is a debug visualization, separate from the wind simulation."};
        case SettingId::QualityPresetLabel:
        case SettingId::MusicEnabled:
        case SettingId::MusicVolume:
            return std::nullopt;
    }
    return std::nullopt;
}

std::span<const AdditionalFeatureImpact> additional_feature_impacts() noexcept {
    static constexpr std::array features{
        AdditionalFeatureImpact{"Color grading", {"GPU: Low", "Color adjustments use the display transform's lookup table. This optional feature is configured through command-line options."}},
        AdditionalFeatureImpact{"Dynamic resolution", {"GPU: Usually saves work | Upscaling: Low overhead", "Can lower internal resolution during interactive use. Scored benchmark runs keep a fixed pixel workload. This optional feature is configured through command-line options."}},
    };
    return features;
}

} // namespace heaven
