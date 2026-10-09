/*
 * HeavenBench — In-UI configuration value header (GPU-free)
 *
 * This header is the single home for the value types that flow between the pure
 * Dear ImGui widget builder (OverlayUI) and the Main_Loop's configuration
 * controller: the identity of every editable setting (SettingId), the typed
 * value an edit carries (SettingValue), the per-frame edit record
 * (ConfigurationEdit), the extended OverlayIntents the builder returns, and the
 * ConfigUiState feedback block the Main_Loop passes back into build().
 *
 * It is deliberately GPU-free: it depends only on engine/types.h (already
 * Filament-free) and the standard library, references no Filament symbol, and
 * has no source file. This lets OverlayUI, the Settings Catalog, the
 * configuration controller, and the headless host test target share these
 * types without pulling in the renderer.
 */
#ifndef HEAVENBENCH_SRC_RHI_OVERLAY_CONFIG_EDIT_H_
#define HEAVENBENCH_SRC_RHI_OVERLAY_CONFIG_EDIT_H_

#include "engine/types.h"
#include "rhi/overlay/microprofiler_ui.h"

#include <string>
#include <variant>
#include <vector>

namespace heaven {

// Identity of every editable setting, shared by the Settings Catalog and the
// edit path so a control, its validator, and its Live/Reload classification all
// agree on which BenchConfig field they govern. Grouped by owning Options_Panel
// in the same order as the current Live/Reload classification table.
enum class SettingId {
    // ── Camera (Live; not persisted in config_schema) ──
    PhysicalCameraEnabled,
    CameraAperture,
    CameraShutter,
    CameraIso,

    // ── Environment (Live) ──
    TimeOfDay,
    RainIntensity,
    DryDownSeconds,
    FeatureRain,
    FeatureWetReflections,
    FeatureSky,

    // ── Tessellation (Reload) ──
    TessEnabled,
    TessScale,
    TessFactor,
    TessDistance,

    // ── Settings (Reload) ──
    Width,
    Height,
    Fullscreen,
    Hdr,
    HdrPeakNits,
    HdrPaperWhiteNits,
    ClothSimHz,
    RenderTestScene,

    // ── Quality (mix of Live and Reload) ──
    TextureQuality,
    FeatureGrass,
    FeatureParticles,
    FeatureGodRays,
    FeatureVolumetricFog,
    FeatureClouds,
    FeatureLensFlares,
    FeatureVolumetricClouds,
    FeatureSsgi,
    FeatureMassiveFoliage,
    FeatureGroundRelief,
    FeatureSoftShadows,
    FeatureContactShadows,
    FeaturePointLightShadows,
    FeatureReflections,
    WindDebug,
    FeatureMsaa,
    FeatureBloom,
    BloomStrength,
    FeatureAmbientOcclusion,
    FeatureTaa,
    FeatureDepthOfField,
    FeatureRefraction,
    FeatureClearCoat,
    FeatureAnisotropic,
    FeatureSubsurface,
    FeatureCloth,
    FeatureTransparentShadows,
    FeatureColoredPenumbra,
    FeatureSpecularAa,
    FeatureGpuInstancing,
    FeatureGpuSkinning,
    HeatHazeEnabled,
    SoftShadowLevel,
    ContactShadowLevel,
    ShadowLightBudget,
    CloudQuality,
    SsgiQuality,
    AoQuality,
    BloomQuality,
    DofQuality,
    TaaQuality,
    FoliageDensity,
    MsaaSamples,
    SunShadowMapSize,
    PointShadowMapSize,
    RenderPreset,
    MaterialWorkflow,
    CpuStressEnabled,
    CpuStressWorkers,
    CpuStressDuration,
    QualityPresetLabel,

    // ── Sound ──
    MusicEnabled,
    MusicVolume,
};

// Typed value carried by an edit. Headless-friendly: the alternatives are plain
// value types (and heaven::QualityLevel), so the variant references no Filament
// or GPU type.
using SettingValue =
    std::variant<bool, int, float, heaven::QualityLevel,
                 heaven::RenderPreset, heaven::MaterialWorkflow,
                 heaven::TextureQuality, std::string>;

// A single committed change surfaced by OverlayUI this frame:
// exactly which setting changed and the new value the user committed.
struct ConfigurationEdit {
    SettingId    id;
    SettingValue value;
};

// User intents raised by the overlay this frame. The caller honours `quit`
// before `start_benchmark` so simultaneous activation quits without starting a
// run. All fields default to "no intent".
struct OverlayIntents {
    // ── existing benchmark/menu intents (unchanged) ──
    bool start_benchmark = false;   // Benchmark control
    bool cancel          = false;   // rarely from UI; ESC is handled in the loop
    bool dismiss_result  = false;   // Result_Report dismiss
    bool quit            = false;   // Quit control

    // The Options_Panel a menu control opened this frame, or None

    enum class Panel {
        None,
        Camera,
        Environment,
        Tessellation,
        Settings,
        Quality,
        Sound
    } open_panel = Panel::None;

    // ── new configuration intents/edits ──
    std::vector<ConfigurationEdit> edits;
    bool        save = false;  std::string save_path;
    bool        load = false;  std::string load_path;
    bool        reload = false;
    bool        dismiss_diagnostics = false;
    MicroprofileIntents microprofile;
};

// Feedback the Main_Loop passes back into OverlayUI::build() so the pure widget
// builder can render dirty/error/success state and diagnostics without owning
// any of it.
struct ConfigUiState {
    bool                     dirty        = false;
    bool                     reload_error = false;
    bool                     success      = false;
    std::string              current_config_name = "config.json";
    std::string              success_detail;        // saved/loaded path
    std::vector<std::string> diagnostics;           // (in order)
    bool texture_set_available = false;
    uint64_t texture_original_gpu_bytes = 0;
    uint64_t texture_2k_gpu_bytes = 0;
    uint64_t texture_4k_gpu_bytes = 0;
};

// Append exactly one ConfigurationEdit carrying `id` and `value` to `intents`.
// This is the single funnel OverlayUI uses to surface a committed control change
// one call → one edit, leaving the rest of the intents
// untouched.
inline void emit_edit(OverlayIntents& intents, SettingId id, SettingValue value) {
    intents.edits.push_back(ConfigurationEdit{id, std::move(value)});
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_OVERLAY_CONFIG_EDIT_H_
