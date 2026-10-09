/*
 * HeavenBench — Post-processing configuration
 *
 * Extracted verbatim out of FilamentBackend::setup_post_processing.
 */
#include "rhi/post_process.h"
#include "render_tuning.h"
#include "benchmark/microprofiler.h"

#include <filament/Engine.h>
#include <filament/View.h>
#include <filament/ColorGrading.h>
#include <filament/ColorSpace.h>
#include <filament/ToneMapper.h>
#include <filament/Options.h>

#include <math/vec3.h>
#include <math/vec4.h>

#include <glm/common.hpp>

#include <cstdio>

namespace heaven {
namespace {

// Forward declaration — defined after configure_post_processing; shared by the
// SDR and HDR paths to configure everything except the tone-mapper/ColorGrading.
void configure_post_processing_common(
    filament::View& view,
    const TonemapParams& tonemap,
    float fog_density_override,
    const QualityKnobs& quality,
    bool msaa_enabled,
    bool ssr_enabled,
    bool dynamic_resolution_enabled,
    float dynres_min_scale,
    const FeatureToggles& features);

// Dynamic resolution + FSR. When enabled, configure Filament's dynamic
// resolution with `enabled=true`, `minScale={k,k}`, `maxScale={1,1}`, and
// `quality=QualityLevel::HIGH` (Filament's FSR1 upscaler — see Options.h: HIGH
// selects AMD FidelityFX FSR1). Filament's own frame-time controller then scales
// the internal render target between `min_scale` and 1.0 to hold the target
// frame time, and the result is FSR-upscaled to the full display
// resolution. When disabled, dynamic resolution is left OFF — a
// default-constructed DynamicResolutionOptions has `enabled=false`, so the view
// renders at full resolution exactly like the Shipping_Look (≤1/255 parity,
// `min_scale` is the already-clamped Quality_Knob value.
void configure_dynamic_resolution(
    filament::View& view, bool enabled, float min_scale)
{
    filament::DynamicResolutionOptions dsr;
    if (enabled) {
        dsr.enabled  = true;
        dsr.minScale = filament::math::float2{ min_scale, min_scale };
        dsr.maxScale = filament::math::float2{ 1.0f, 1.0f };
        dsr.quality  = filament::QualityLevel::HIGH; // AMD FidelityFX FSR1
    } else {
        // Full-resolution render (parity): default-constructed options already
        // carry enabled=false; set it explicitly for clarity.
        dsr.enabled = false;
    }
    view.setDynamicResolutionOptions(dsr);
}

// Small glm → Filament math conversions for the color-grading chain.
[[nodiscard]] constexpr filament::math::float3 to_f3(const glm::vec3& v) {
    return { v.x, v.y, v.z };
}
[[nodiscard]] constexpr filament::math::float4 to_f4(const glm::vec4& v) {
    return { v.x, v.y, v.z, v.w };
}

// Chain the full color-grading controls  onto an SDR ColorGrading
// builder from the ColorGradingConfig. Called ONLY when color grading is
// enabled; when disabled the builder is left untouched so the result is
// bit-identical to the Shipping_Look.

// Filament applies the transforms in a fixed order (see ColorGrading.h):
// night-adaptation → white balance → channel mixer → shadows/mid-tones/
// highlights → slope/offset/power (CDL) → contrast → saturation. The neutral
// ColorGradingConfig defaults map onto Filament's own neutral values, so an
// enabled-but-neutral grade reproduces the Shipping_Look as well.
void apply_color_grading_chain(
    filament::ColorGrading::Builder& builder,
    const ColorGradingConfig& grading)
{
    // White balance + saturation start from the authored controls; the
    // night-adaptation preset  layers a blue-shifted white balance and
    // reduced saturation ON TOP, consistent with scotopic (low-light) vision —
    // distinct from the neutral daytime grade. Temperature runs blue (cool) at
    // -1.0 → warm at +1.0 (per Filament's whiteBalance docs), so a negative
    // shift cools/blue-shifts the image; saturation is scaled down.
    float temperature = grading.temperature;
    float tint        = grading.tint;
    float saturation  = grading.saturation;
    if (grading.night_adaptation) {
        temperature = glm::clamp(
                                 temperature +
                                 render_tuning::post_process.night_temperature_shift,
                                 ColorGradingConfig::k_white_balance_lo,
                                 ColorGradingConfig::k_white_balance_hi);
        saturation  = glm::clamp(
                                 saturation *
                                 render_tuning::post_process.night_saturation_scale,
                                 ColorGradingConfig::k_saturation_lo,
                                 ColorGradingConfig::k_saturation_hi);
    }

    builder.whiteBalance(temperature, tint);

    builder.channelMixer(to_f3(grading.mixer_red),
                         to_f3(grading.mixer_green),
                         to_f3(grading.mixer_blue));

    // Shadows/mid-tones/highlights: the config stores each range's color as an
    // OFFSET from neutral (0 == neutral), while Filament's neutral correction
    // color is {1,1,1} with weight 0. Add the offset onto the {1,1,1} identity
    // so a zero config reproduces Filament's neutral; the.w weight passes
    // through. `ranges` uses Filament's default tonal-zone boundaries.
    const glm::vec3 one(1.0f);
    const filament::math::float4 sh = to_f4(glm::vec4(one + glm::vec3(grading.shadows),
                                                      grading.shadows.w));
    const filament::math::float4 mt = to_f4(glm::vec4(one + glm::vec3(grading.midtones),
                                                      grading.midtones.w));
    const filament::math::float4 hl = to_f4(glm::vec4(one + glm::vec3(grading.highlights),
                                                      grading.highlights.w));
    const auto tuning_ranges = render_tuning::post_process.tonal_ranges;
    const filament::math::float4 ranges{
        tuning_ranges.x, tuning_ranges.y, tuning_ranges.z, tuning_ranges.w};
    builder.shadowsMidtonesHighlights(sh, mt, hl, ranges);

    // ASC CDL slope/offset/power (identity = slope 1, offset 0, power 1).
    builder.slopeOffsetPower(to_f3(grading.cdl_slope),
                             to_f3(grading.cdl_offset),
                             to_f3(grading.cdl_power));

    builder.contrast(grading.contrast);
    builder.saturation(saturation);
}

} // namespace

filament::ColorGrading* configure_post_processing(
    filament::Engine& engine,
    filament::View& view,
    const TonemapParams& tonemap,
    filament::ColorGrading* previous,
    float fog_density_override,
    const QualityKnobs& quality,
    bool msaa_enabled,
    bool ssr_enabled,
    bool hdr_output,
    ToneMapperSelection tone_mapper,
    bool color_grading_enabled,
    const ColorGradingConfig& grading,
    bool dynamic_resolution_enabled,
    float dynres_min_scale,
    const FeatureToggles& features,
    ColorGradingCache* cache)
{
    ColorGradingCache::Inputs inputs;
    inputs.hdr = hdr_output;
    inputs.exposure = tonemap.exposure;
    if (!hdr_output) {
        inputs.tone_mapper = tone_mapper;
        inputs.grading_enabled = color_grading_enabled;
        if (color_grading_enabled) {
            inputs.grading = {
                grading.temperature, grading.tint,
                grading.mixer_red.x, grading.mixer_red.y, grading.mixer_red.z,
                grading.mixer_green.x, grading.mixer_green.y, grading.mixer_green.z,
                grading.mixer_blue.x, grading.mixer_blue.y, grading.mixer_blue.z,
                grading.shadows.x, grading.shadows.y, grading.shadows.z, grading.shadows.w,
                grading.midtones.x, grading.midtones.y, grading.midtones.z, grading.midtones.w,
                grading.highlights.x, grading.highlights.y, grading.highlights.z, grading.highlights.w,
                grading.cdl_slope.x, grading.cdl_slope.y, grading.cdl_slope.z,
                grading.cdl_offset.x, grading.cdl_offset.y, grading.cdl_offset.z,
                grading.cdl_power.x, grading.cdl_power.y, grading.cdl_power.z,
                grading.contrast, grading.saturation};
            inputs.night_adaptation = grading.night_adaptation;
        }
    }
    if (cache && previous && cache->object == previous && cache->inputs == inputs) {
        view.setColorGrading(previous);
        configure_post_processing_common(view, tonemap, fog_density_override,
                                         quality, msaa_enabled, ssr_enabled,
                                         dynamic_resolution_enabled, dynres_min_scale, features);
        return previous;
    }

    profile::Scope scope("Color grading LUT build", "Rendering");
    if (cache) {
        cache->object = nullptr;
        cache->inputs.reset();
    }
    if (previous) {
        view.setColorGrading(nullptr);
        engine.destroy(previous);
    }
    const auto retain = [cache, &inputs](filament::ColorGrading* object) {
        if (cache && object) {
            cache->inputs = inputs;
            cache->object = object;
            ++cache->builds;
        }
        return object;
    };
    static filament::ACESLegacyToneMapper aces_mapper;
    // The prefiltered HDR environment carries more physically plausible
    // midtone energy than the old flat probe. Apply a small display-transform
    // calibration to both output paths so texture colors retain saturation and
    // local contrast without changing their authored data or scene lighting.
    constexpr float kSharedExposureCompensationEv =
        render_tuning::post_process.shared_exposure_ev;

    if (hdr_output) {
        // HDR display output (--hdr). Two changes vs. the SDR path:
        //   1. Tone map with GT7 instead of ACES-Legacy. GT7 is an
        //      HDR-display-referred operator: a framebuffer value of 1.0
        //      corresponds to a reference luminance of 100 cd/m^2 and highlights
        //      roll off ABOVE 1.0 rather than being compressed into [0,1] for an
        //      SDR monitor. That preserved supra-100-nit range is exactly what
        //      the PQ encode pass turns into bright HDR highlights.
        //   2. Output in LINEAR Rec.2020 (no OETF, Rec.2020 primaries). Filament
        //      tone-maps in linear Rec.2020 already, so this leaves the
        //      display-referred values untouched and lands them in the fp16 HDR
        //      scene target. The HDR output pass then applies the Rec.2020 PQ
        //      (ST.2084) OETF to produce the HDR10 swap-chain signal.
        static filament::GT7ToneMapper gt7_mapper;
        // Rec.2020 primaries + linear transfer + D65 white, built via Filament's
        // `Gamut - TransferFunction - WhitePoint` ColorSpace builder syntax.
        static const filament::color::ColorSpace rec2020_linear =
            filament::color::Gamut{{0.708f, 0.292f},
                                   {0.170f, 0.797f},
                                   {0.131f, 0.046f}}
            - filament::color::Linear
            - filament::color::D65;
        filament::ColorGrading* color_grading = filament::ColorGrading::Builder()
            .toneMapper(&gt7_mapper)
            .outputColorSpace(rec2020_linear)
            .exposure(tonemap.exposure + kSharedExposureCompensationEv)
            .build(engine);
        view.setColorGrading(color_grading);
        configure_post_processing_common(view, tonemap, fog_density_override,
                                         quality, msaa_enabled, ssr_enabled,
                                         dynamic_resolution_enabled, dynres_min_scale, features);
        return retain(color_grading);
    }

    // SDR output path. Select the tone-mapping operator from the
    // tone_mapper enum. ACES is the default and keeps using the existing
    // ACES-Legacy static, so a flagless SDR run is bit-identical to the
    // Shipping_Look. The other operators (AgX / PBR Neutral / GT7 /
    // Generic) replace ACES for that run. Each operator is a
    // function-local static so its lifetime exceeds the ColorGrading LUT build,
    // exactly like the existing aces_mapper / gt7_mapper statics. The SDR GT7
    // static is distinct from the HDR branch's gt7_mapper above; the HDR path is
    // never touched here. AgX uses its default look (AgxLook::NONE)
    // and Generic its default operator parameters.
    static filament::AgxToneMapper      agx_mapper;
    static filament::PBRNeutralToneMapper pbr_neutral_mapper;
    static filament::GT7ToneMapper      sdr_gt7_mapper;
    static filament::GenericToneMapper  generic_mapper;

    const filament::ToneMapper* sdr_tone_mapper = &aces_mapper;
    switch (tone_mapper) {
        case ToneMapperSelection::ACES:       sdr_tone_mapper = &aces_mapper;        break;
        case ToneMapperSelection::AgX:        sdr_tone_mapper = &agx_mapper;         break;
        case ToneMapperSelection::PBRNeutral: sdr_tone_mapper = &pbr_neutral_mapper; break;
        case ToneMapperSelection::GT7:        sdr_tone_mapper = &sdr_gt7_mapper;     break;
        case ToneMapperSelection::Generic:    sdr_tone_mapper = &generic_mapper;     break;
    }

    filament::ColorGrading::Builder builder;
    // The HDR branch is calibrated separately against paper white and peak
    // luminance. After moving SDR to the physically filtered environment, its
    // legacy +1 EV grading baseline maps diffuse whites too close to display
    // white. Pull only the SDR display transform down by half a stop, retaining
    // highlight detail without changing scene lighting or camera exposure.
    constexpr float kSdrAdditionalExposureCompensationEv =
        render_tuning::post_process.sdr_exposure_ev;
    builder.toneMapper(sdr_tone_mapper)
           .exposure(tonemap.exposure +
                     kSharedExposureCompensationEv +
                     kSdrAdditionalExposureCompensationEv);

    // Full color-grading chain. Only chained when enabled; when
    // disabled the builder holds exactly the selected tone mapper + exposure, so
    // the result is bit-identical to the Shipping_Look. HDR is handled
    // by the early-return branch above and is untouched.
    if (color_grading_enabled) {
        apply_color_grading_chain(builder, grading);
    }

    filament::ColorGrading* color_grading = builder.build(engine);
    view.setColorGrading(color_grading);
    configure_post_processing_common(view, tonemap, fog_density_override,
                                     quality, msaa_enabled, ssr_enabled,
                                     dynamic_resolution_enabled, dynres_min_scale, features);
    return retain(color_grading);
}

namespace {

// Shared post-processing stack (everything except the ColorGrading/tone-mapper,
// which differs between the SDR and HDR output paths above). Configures bloom,
// SSAO, DOF, fog, SSR, TAA, MSAA, and the dynamic-light z-slice range on `view`.
void configure_post_processing_common(
    filament::View& view,
    const TonemapParams& tonemap,
    float fog_density_override,
    const QualityKnobs& quality,
    bool msaa_enabled,
    bool ssr_enabled,
    bool dynamic_resolution_enabled,
    float dynres_min_scale,
    const FeatureToggles& features)
{
    // Bloom
    filament::View::BloomOptions bloom;
    bloom.enabled = features.bloom;
    bloom.strength = tonemap.bloom_strength;
    bloom.threshold = true;
    bloom.lensFlare = false;
    switch (quality.bloom_quality) {
        case QualityLevel::Low:
            bloom.resolution = render_tuning::post_process.bloom_resolution[0];
            bloom.levels = render_tuning::post_process.bloom_levels[0];
            bloom.quality = filament::QualityLevel::MEDIUM;
            break;
        case QualityLevel::Medium:
            bloom.resolution = render_tuning::post_process.bloom_resolution[1];
            bloom.levels = render_tuning::post_process.bloom_levels[1];
            bloom.quality = filament::QualityLevel::HIGH;
            break;
        case QualityLevel::High:
            bloom.resolution = render_tuning::post_process.bloom_resolution[2];
            bloom.levels = render_tuning::post_process.bloom_levels[2];
            bloom.quality = filament::QualityLevel::HIGH;
            break;
    }
    view.setBloomOptions(bloom);

    // SSAO
    filament::AmbientOcclusionOptions ao;
    ao.enabled = features.ambient_occlusion;
    ao.aoType =
        filament::AmbientOcclusionOptions::AmbientOcclusionType::GTAO;
    ao.radius = render_tuning::post_process.ao.x;
    ao.intensity = render_tuning::post_process.ao.y;
    ao.power = render_tuning::post_process.ao.z;
    // Filament also uses this resolution for the shared structure depth pass,
    // even when AO is disabled. Half-resolution depth makes contact-shadow
    // rays self-occlude in coarse, dithered stripes on otherwise smooth walls.
    ao.resolution = features.contact_shadows ||
        quality.ao_quality == QualityLevel::High ? 1.0f : 0.5f;
    ao.quality = quality.ao_quality == QualityLevel::High
        ? filament::QualityLevel::ULTRA : filament::QualityLevel::HIGH;
    ao.lowPassFilter = filament::QualityLevel::HIGH;
    ao.upsampling = filament::QualityLevel::HIGH;
    ao.bilateralThreshold = render_tuning::post_process.ao.w;
    ao.bentNormals = true;
    ao.gtao.useVisibilityBitmasks = false;
    if (quality.ao_quality == QualityLevel::High) {
        ao.gtao.sampleSliceCount =
            render_tuning::post_process.ao_ultra_slices;
        ao.gtao.sampleStepsPerSlice =
            render_tuning::post_process.ao_ultra_steps_per_slice;
    }
    view.setAmbientOcclusionOptions(ao);
    view.setGuardBandOptions(
        filament::GuardBandOptions{.enabled =
            ssr_enabled || (features.ambient_occlusion &&
                            quality.ao_quality == QualityLevel::High)});

    // Camera updates own the enabled state and optical CoC scale. Weather
    // reconfiguration runs afterward and must preserve both; it only changes
    // quality or applies the user's explicit disable switch.
    auto dof = view.getDepthOfFieldOptions();
    dof.enabled = dof.enabled && features.depth_of_field;
    apply_dof_quality(dof, quality.dof_quality);
    view.setDepthOfFieldOptions(dof);

    // Fog

    // Two coupled choices here:
    //   * `cutOffDistance` smaller than the sky-sphere radius
    //     (1000 m) so fog is excluded from the sky. Without this
    //     the fog density at sky fragments depends on world-Z
    //     height, which produces a vertical brightness band that
    //     follows the camera (the long-running "seam" bug).
    //   * `density` low enough that real scene geometry stays
    //     visible inside the fog cutoff. Earlier `density = 0.005`
    //     plus a finite cutoff dropped the islands into a wall of
    //     pink haze; `density = 0.0008` keeps them legible while
    //     still adding aerial perspective.
    // Filament's built-in distance fog is OFF by default: the scene's fog is
    // driven entirely by our heaven_volumetric_fog material (a
    // per-light ray-march that gives aerial perspective AND local
    // scattering halos around each lamp).

    // The weather system can opt in to Filament's distance fog via
    // `fog_density_override`: a value >= 0.0f enables fog at that density
    // for the overcast rain response. The default negative sentinel
    // means "use the shipping config" — fog stays disabled and output is
    // bit-identical to the pre-weather behavior.
    filament::FogOptions fog;
    if (fog_density_override >= 0.0f) {
        // Weather override: populate the FULL FogOptions so the overcast haze
        // is bounded and grey, NOT Filament's default white-and-infinite fog.

        // Leaving the other fields at their defaults was the "seam"/white-out
        // bug: default `color = {1,1,1}` (WHITE), `cutOffDistance = INFINITY`
        // (so the 1000 m sky-sphere and far field get fogged too) and
        // `maximumOpacity = 1.0` together saturate the distance and sky to
        // white, producing a flat near-white region with a hard diagonal edge
        // where geometry meets the blue sky. The fixes below mirror the
        // distance-fog seam warning above:
        //   * cutOffDistance below the sky-sphere radius (~1000 m) so the SKY
        //     is excluded from the fog — removes the white wall over the sky
        //     and the hard seam against it.
        //   * a neutral overcast GREY color (not white) so the haze reads as
        //     an overcast sky tone rather than a white-out.
        //   * maximumOpacity < 1.0 so even the far field never fully whites
        //     out — distant islands stay legible.
        //   * a few metres of clear `distance` so near geometry stays crisp,
        //     and a gentle height falloff so the haze sits low in the scene.
        fog.enabled        = true;
        fog.density        = fog_density_override;
        const auto fog_distances =
            render_tuning::post_process.weather_fog_distances;
        const auto fog_color = render_tuning::post_process.weather_fog_color;
        fog.distance       = fog_distances.x;
        fog.cutOffDistance = fog_distances.y;
        fog.maximumOpacity = fog_distances.z;
        fog.color          = {fog_color.x, fog_color.y, fog_color.z};
        fog.height         = 0.0f;
        fog.heightFalloff  = fog_distances.w;
    } else {
        fog.enabled = false;
    }
    view.setFogOptions(fog);

    // Screen-space reflections

    // Filament does single-pass SSR off the depth/colour history.
    // The default `maxDistance=3.0` was tuned for tight indoor
    // scenes where reflections die past a few metres; in our open
    // floating-island benchmark we want the dragon statue to pick
    // up the airship, and lamp pools on the cobblestone road to
    // reflect adjacent lanterns. Push the ray length out to 25m
    // and tighten the stride a little for fewer step artefacts on
    // the fine cobblestone normal map.

    // SSR is HDR-aware in Filament — it samples the colour buffer
    // post-tonemap-pre-bloom, so the lamp glow contributes to the
    // reflection naturally without us doing anything extra.
    filament::ScreenSpaceReflectionsOptions ssr;
    ssr.enabled = ssr_enabled;
    ssr.thickness = render_tuning::post_process.ssr.x;
    ssr.bias = render_tuning::post_process.ssr.y;
    ssr.maxDistance = render_tuning::post_process.ssr.z;
    ssr.stride = render_tuning::post_process.ssr.w;
    view.setScreenSpaceReflectionsOptions(ssr);

    // Temporal anti-aliasing

    // Replaces the FXAA that was on `view_->setAntiAliasing()` —
    // we set that to NONE in init_pipelines() so only TAA runs.

    // Why TAA: the grass blade alpha-tested edges and the foliage
    // billboards both produce per-pixel shimmer at native MSAA-0
    // resolution that FXAA cannot suppress (FXAA is a single-frame
    // luminance edge filter; it can't see the temporal flicker).
    // TAA reuses 16 jittered Halton samples across frames, which
    // collapses the shimmer into a stable image while also giving
    // ~2× effective texturing resolution thanks to the -1.0 LOD
    // bias.

    // Knobs:
    //   feedback = 0.12  — Filament default; lower = more history
    //                      reuse (smoother, more ghosting).
    //   sharpness = 0.20 — modest unsharp-mask post-pass to
    //                      counter the slight TAA-induced softness
    //                      on stone textures.
    //   preventFlickering = true — adapts feedback per-pixel when a
    //                      pixel keeps changing. Crucial for grass
    //                      and the airship's spinning propeller,
    //                      both of which would smear without it.
    //   jitterPattern = HALTON_23_X16 — the standard high-quality
    //                      Halton(2,3) sequence over 16 frames.
    //   hdr = true       — our render target is HDR; tells TAA to
    //                      do the box-clipping in linear space
    //                      rather than after tonemap.
    filament::TemporalAntiAliasingOptions taa;
    taa.enabled = features.taa;
    taa.feedback = render_tuning::post_process.taa.x;
    taa.lodBias = render_tuning::post_process.taa.y;
    taa.sharpness = render_tuning::post_process.taa.z;
    taa.hdr = true;
    taa.preventFlickering = true;
    taa.jitterPattern = quality.taa_quality == QualityLevel::Low
        ? filament::TemporalAntiAliasingOptions::JitterPattern::HALTON_23_X16
        : filament::TemporalAntiAliasingOptions::JitterPattern::HALTON_23_X32;
    taa.boxType =
        filament::TemporalAntiAliasingOptions::BoxType::AABB_VARIANCE;
    taa.boxClipping =
        filament::TemporalAntiAliasingOptions::BoxClipping::ACCURATE;
    view.setTemporalAntiAliasingOptions(taa);

    // Multi-sample anti-aliasing (MSAA), layered ON TOP of TAA.

    // The TAA block above is deliberately left untouched: MSAA and
    // TAA coexist — MSAA resolves the hard geometric edges that TAA's temporal
    // history cannot fully settle (thin geometry, high-contrast silhouettes),
    // while TAA continues to suppress the grass/foliage shimmer.

    // The sample count comes from the Quality_Knob (QualityKnobs::msaa_samples),
    // which the CLI restricts to the accepted set {1,2,4} and which the
    // init-time capability query (configure_msaa) has already clamped to a
    // GPU-supported count. When the MSAA Feature_Toggle is off we configure 1
    // sample (MSAA disabled) so anti-aliasing behaviour is identical to the
    // Shipping_Look — TAA only. A default-constructed QualityKnobs
    // carries msaa_samples == 1 and msaa_enabled defaults to false, so baseline
    // callers are bit-identical.
    filament::MultiSampleAntiAliasingOptions msaa;
    const int msaa_count = msaa_enabled ? quality.msaa_samples : 1;
    msaa.enabled     = msaa_enabled && msaa_count > 1;
    msaa.sampleCount = static_cast<uint8_t>(msaa_count);
    msaa.customResolve = msaa.enabled && msaa_count >= 4;
    view.setMultiSampleAntiAliasingOptions(msaa);

    // Dynamic-light z-slice range. Filament's clustered shading
    // partitions the camera frustum into z-slices and assigns each
    // light to the slices its falloff sphere intersects. The default
    // (5m–100m) compresses *all* lights into a narrow z-band relative
    // to our scene scale, which floods one slice with too many
    // overlapping lights and overflows the per-tile light record
    // buffer. Visually that surfaces as **screen-aligned rectangular
    // patches** where some tiles get the full lamp contribution and
    // adjacent tiles get none.

    // Our point lights have falloff <= 18m and the visible scene
    // ranges from ~0.5m (camera at door) to ~150m (far floating
    // islands). Pushing zLightNear down to 0.5m and zLightFar in
    // to 60m gives Filament more z-slices precisely where the lights
    // actually live, drastically reducing per-slice crowding.
    view.setDynamicLightingOptions(
        render_tuning::post_process.dynamic_light_range.x,
        render_tuning::post_process.dynamic_light_range.y);

    // Dynamic resolution + FSR. Applied here so BOTH the SDR and HDR
    // output paths configure it identically. When disabled it is left off and
    // the view renders at full resolution (≤1/255 parity).
    configure_dynamic_resolution(view, dynamic_resolution_enabled, dynres_min_scale);
}

} // namespace

int configure_msaa(int requested, std::span<const int> supported)
{
    // Pure selection: highest supported count <= request, else 1.
    const int applied = msaa_fallback(requested, supported);

    // Single diagnostic, only when the GPU could not honour the request
    // When applied == requested there is nothing to report.
    if (applied != requested) {
        std::printf("  MSAA: requested %d samples not supported by GPU — "
                    "falling back to %d samples\n", requested, applied);
    }
    return applied;
}

void apply_dof_quality(
    filament::DepthOfFieldOptions& options, QualityLevel quality) noexcept {
    switch (quality) {
        case QualityLevel::Low:
            options.nativeResolution = false;
            options.foregroundRingCount = 0;
            options.backgroundRingCount = 0;
            options.fastGatherRingCount = 0;
            break;
        case QualityLevel::Medium:
            options.nativeResolution = true;
            options.foregroundRingCount = 5;
            options.backgroundRingCount = 5;
            options.fastGatherRingCount = 5;
            break;
        case QualityLevel::High:
            options.nativeResolution = true;
            options.foregroundRingCount = 9;
            options.backgroundRingCount = 7;
            options.fastGatherRingCount = 7;
            break;
    }
}

} // namespace heaven
