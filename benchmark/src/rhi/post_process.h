/*
 * HeavenBench — Post-processing configuration
 *
 * Configures the main view's post-processing stack (ACES color grading,
 * bloom, SSAO, depth-of-field, fog, screen-space reflections, temporal
 * anti-aliasing, and the dynamic-light z-slice range). This holds no
 * persistent state beyond the ColorGrading object, so it lives as a free
 * function rather than a class — mirroring the sky_ibl / filament_util
 * precedent for pure / near-stateless helpers.
 *
 * Extracted verbatim out of FilamentBackend::setup_post_processing.
 */
#ifndef HEAVENBENCH_SRC_RHI_POST_PROCESS_H_
#define HEAVENBENCH_SRC_RHI_POST_PROCESS_H_

#include "rhi/rhi.h"   // TonemapParams

#include <array>
#include <optional>

namespace filament {
class Engine;
class View;
class ColorGrading;
struct DepthOfFieldOptions;
}

namespace heaven {

// One non-owning cache per Engine/owned ColorGrading. The caller still owns
// and destroys the returned object; reset this cache if that ownership changes.
// Only LUT inputs participate. Weather, bloom and other View options continue
// to apply on every configuration call.
struct ColorGradingCache {
    struct Inputs {
        bool hdr = false;
        float exposure = 0.0f;
        ToneMapperSelection tone_mapper = ToneMapperSelection::ACES;
        bool grading_enabled = false;
        std::array<float, 34> grading{};
        bool night_adaptation = false;
        bool operator==(const Inputs&) const = default;
    };
    std::optional<Inputs> inputs;
    filament::ColorGrading* object = nullptr;
    uint64_t builds = 0;
};

// Configure the post-processing stack on `view`. The previous
// ColorGrading instance (or nullptr on first call) is reused when `cache`
// matches its exact LUT inputs, otherwise replaced. The resulting instance is
// returned and ownership stays with the caller (FilamentBackend keeps it
// in color_grading_ and destroys it at teardown).

// `fog_density_override` is an optional weather hook: a value >= 0.0f
// enables Filament's distance fog at that density (used by the overcast
// weather response), while the default negative sentinel (-1.0f) means
// "use the shipping config" — fog stays off and the scene's fog is driven
// entirely by the heaven_volumetric_fog material, so default callers are
// bit-identical to the pre-weather behavior.

// `quality` carries the advanced-rendering Quality_Knobs and `msaa_enabled`
// the MSAA Feature_Toggle. When `msaa_enabled` is true the view's
// multi-sample anti-aliasing is configured to `quality.msaa_samples` samples
// (already capped to a GPU-supported count by configure_msaa below); when
// false MSAA is configured to 1 sample (disabled) so anti-aliasing stays
// identical to the Shipping_Look (TAA only). The existing TAA block
// is left unchanged either way. Defaults keep baseline callers
// bit-identical: a default-constructed QualityKnobs has msaa_samples == 1 and
// msaa_enabled defaults to false, so MSAA is off.

// `tone_mapper` selects the SDR tone-mapping operator. It applies ONLY
// to the SDR output path: ACES (the default, bit-identical to the Shipping_Look
// via the existing ACES-Legacy static), AgX, PBR Neutral, GT7, or
// Generic. The `--hdr` output path is untouched and always uses GT7,
// so the SDR selection never alters HDR behaviour. The default
// (ToneMapperSelection::ACES) keeps baseline callers bit-identical.

// `color_grading_enabled` + `grading` carry the full color-grading chain
// When `color_grading_enabled` is true, the SDR ColorGrading is built
// with the `whiteBalance`, `channelMixer`, `shadowsMidtonesHighlights`,
// `slopeOffsetPower` (ASC CDL), `contrast`, and `saturation` controls from
// `grading` (plus the optional night-adaptation preset). When false, NONE of the
// grading builder calls are chained — the ColorGrading is built exactly as
// before (selected tone mapper + exposure only), so output is bit-identical to
// the Shipping_Look. Defaults keep baseline callers bit-identical and
// the grading is applied ONLY to the SDR path; the `--hdr` branch is untouched
// (still holds).

// `dynamic_resolution_enabled` + `dynres_min_scale` carry the dynamic-resolution
// + FSR scaling capability. When enabled, the view configures Filament's
// dynamic resolution with `enabled=true`, `minScale={k,k}`, `maxScale={1,1}`, and
// `quality=QualityLevel::HIGH` (Filament's FSR1 upscaler), letting Filament's own
// frame-time controller scale the internal render target between `dynres_min_scale`
// and 1.0 to meet the target frame time, then FSR-upscale to the full display
// resolution. When disabled, dynamic resolution is left off (full-resolution
// render) so output is bit-identical to the Shipping_Look (≤1/255 parity, Req
// `dynres_min_scale` is the already-clamped Quality_Knob value
// (clamp_dynres_min_scale). Defaults keep baseline callers bit-identical
// (disabled → full resolution), and the setting is applied to both the SDR and
// HDR output paths via configure_post_processing_common.
[[nodiscard]] filament::ColorGrading* configure_post_processing(
    filament::Engine& engine,
    filament::View& view,
    const TonemapParams& tonemap,
    filament::ColorGrading* previous,
    float fog_density_override = -1.0f,
    const QualityKnobs& quality = {},
    bool msaa_enabled = false,
    bool ssr_enabled = true,
    bool hdr_output = false,
    ToneMapperSelection tone_mapper = ToneMapperSelection::ACES,
    bool color_grading_enabled = false,
    const ColorGradingConfig& grading = {},
    bool dynamic_resolution_enabled = false,
    float dynres_min_scale = 1.0f,
    const FeatureToggles& features = {},
    ColorGradingCache* cache = nullptr);

// Resolve the MSAA sample count to apply against the GPU's capabilities
// `requested` is the configured sample count; `supported` is the
// set of sample counts the GPU reports as usable. Returns the highest
// supported count <= requested (down to 1 when nothing at or below the request
// is supported) and, when the applied count differs from the request, emits a
// SINGLE diagnostic naming both. This is the init-time capability query that
// wraps the pure heaven::msaa_fallback helper; callers configure the returned
// count once at startup and pass it through QualityKnobs::msaa_samples.
[[nodiscard]] int configure_msaa(int requested, std::span<const int> supported);

// Apply the preset quality tier to a DOF options value without changing its
// enabled/cocScale fields. Shared by startup and authored per-frame updates.
void apply_dof_quality(
    filament::DepthOfFieldOptions& options, QualityLevel quality) noexcept;

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_POST_PROCESS_H_
