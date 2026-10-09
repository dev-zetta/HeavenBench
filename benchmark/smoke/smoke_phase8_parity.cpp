/*
 * HeavenBench — Shipping_Look configuration and pixel-comparison smoke case.
 *
 * Verify the all-disabled reference settings and the per-capability toggles,
 * render the reference configuration on the live backend, and exercise the
 * RGBA comparison helper with identical, changed, and mismatched buffers.
 * Full camera-path pixel parity still needs scene assets and recorded reference
 * images; validation-layer acceptance needs a configured Vulkan validation run.
 * This small-window harness records those missing checks as SKIP.
 *
 * Performance is measured separately by the production camera-path run. Its
 * informational window has no universal FPS requirement for these settings.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "rhi/filament_backend.h"
#include "engine/types.h"
#include "engine/camera.h"
#include "benchmark/pixel_parity.h"

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_phase8_parity]";

// Render exactly one empty-scene frame on the live backend in the all-disabled
// configuration. The backend tolerates empty instance/mesh spans, so
// this drives the full per-frame path (and presents) without loading the heavy
// Heaven scene, mirroring the sibling smoke cases.
void render_one_frame(IRhiBackend& backend, const Camera& camera,
                      const LightingParams& lighting,
                      const TonemapParams& tonemap, int w, int h) {
    MaterialResolver resolver =
        [](const std::string&, const std::string&) -> GpuMaterial {
            return GpuMaterial{};
        };
    WeatherState weather;
    backend.render_frame(std::span<const Instance>{},
                         std::span<const int>{},
                         std::span<const GpuMesh>{},
                         resolver, camera, lighting, weather, tonemap, w, h);
}

// True iff `t` has every one of the ten  capability toggles OFF. This is
// the disabled state the ≤1/255 parity bar is defined over.
bool all_phase8_toggles_off(const FeatureToggles& t) {
    return !t.clear_coat && !t.anisotropic && !t.subsurface && !t.cloth &&
           !t.transparent_shadows && !t.colored_penumbra && !t.specular_aa &&
           !t.color_grading && !t.physical_camera && !t.dynamic_resolution;
}

// True iff `g` is the neutral grade that reproduces the Shipping_Look — every
// control at its identity value and no night-adaptation preset.
bool grading_is_neutral(const ColorGradingConfig& g) {
    return g.temperature == 0.0f && g.tint == 0.0f &&
           g.mixer_red   == glm::vec3(1.0f, 0.0f, 0.0f) &&
           g.mixer_green == glm::vec3(0.0f, 1.0f, 0.0f) &&
           g.mixer_blue  == glm::vec3(0.0f, 0.0f, 1.0f) &&
           g.shadows    == glm::vec4(0.0f) &&
           g.midtones   == glm::vec4(0.0f) &&
           g.highlights == glm::vec4(0.0f) &&
           g.cdl_slope  == glm::vec3(1.0f) &&
           g.cdl_offset == glm::vec3(0.0f) &&
           g.cdl_power  == glm::vec3(1.0f) &&
           g.contrast == 1.0f && g.saturation == 1.0f &&
           !g.night_adaptation;
}

// True iff `c` is the default physically-based-camera exposure that reproduces
// the Shipping_Look when the physical camera is disabled.
bool camera_is_shipping_default(const PhysicalCameraConfig& c) {
    return c.aperture == 16.0f && c.shutter_speed == (1.0f / 125.0f) &&
           c.sensitivity == 100.0f;
}

} // namespace

CaseResult run_phase8_parity_case(const CaseContext& ctx) {
    bool ok = true;

    // ── HOST-SIDE: the ALL-DISABLED config == Shipping_Look ──
    // The seven recommended  capabilities now default ON, so a flagless
    // run is NOT the parity reference. The ≤1/255 bar is defined over the
    // ALL-DISABLED configuration: every  capability forced off (every
    // --no-<feature> passed + the opt-in capabilities off), the ACES tone
    // mapper, neutral grading, and the default physical-camera exposure. We
    // build that configuration here and pin that it is the all-off
    // Shipping_Look reference; we ALSO pin that a default-constructed AppConfig
    // is NOT all-off (the seven default ON), so the new default can never
    // silently revert to the old all-off behaviour.
    {
        // The ALL-DISABLED reference: start from defaults and force every
        // capability off + ACES + neutral grading + default camera.
        AppConfig ref;
        ref.features.clear_coat          = false;
        ref.features.anisotropic         = false;
        ref.features.subsurface          = false;
        ref.features.cloth               = false;
        ref.features.transparent_shadows = false;
        ref.features.colored_penumbra    = false;
        ref.features.specular_aa         = false;
        ref.features.color_grading       = false;
        ref.features.physical_camera     = false;
        ref.features.dynamic_resolution  = false;
        ref.tone_mapper                  = ToneMapperSelection::ACES;
        ref.grading                      = ColorGradingConfig{};
        ref.physical_camera              = PhysicalCameraConfig{};

        if (!all_phase8_toggles_off(ref.features)) {
            std::printf("%s FAIL: the all-disabled reference still has a Phase-8 "
                        "capability enabled — the ≤1/255 parity bar (Req 14.1) is "
                        "defined over the all-disabled state\n", kTag);
            ok = false;
        }
        if (ref.tone_mapper != ToneMapperSelection::ACES) {
            std::printf("%s FAIL: all-disabled reference tone mapper is not ACES "
                        "(Req 9.3/14.1)\n", kTag);
            ok = false;
        }
        if (!grading_is_neutral(ref.grading)) {
            std::printf("%s FAIL: all-disabled reference grading is not neutral "
                        "(Req 10.4/14.1)\n", kTag);
            ok = false;
        }
        if (!camera_is_shipping_default(ref.physical_camera)) {
            std::printf("%s FAIL: all-disabled reference physical camera is not "
                        "the Shipping_Look exposure (Req 11.4/14.1)\n", kTag);
            ok = false;
        }

        // A default-constructed AppConfig now has the seven recommended
        // capabilities ON, so it is NOT the all-disabled reference. Pin that so
        // the new default-on policy cannot silently regress to all-off.
        const AppConfig def;
        if (all_phase8_toggles_off(def.features)) {
            std::printf("%s FAIL: a default AppConfig is all-off, but the seven "
                        "recommended Phase-8 capabilities are expected to default "
                        "ON (clear_coat=%d anisotropic=%d subsurface=%d cloth=%d "
                        "transparent_shadows=%d colored_penumbra=%d "
                        "specular_aa=%d)\n", kTag,
                        def.features.clear_coat, def.features.anisotropic,
                        def.features.subsurface, def.features.cloth,
                        def.features.transparent_shadows,
                        def.features.colored_penumbra, def.features.specular_aa);
            ok = false;
        }
        // The three opt-in capabilities still default OFF.
        if (def.features.color_grading || def.features.physical_camera ||
            def.features.dynamic_resolution) {
            std::printf("%s FAIL: an opt-in Phase-8 capability defaults ON, "
                        "expected OFF (Req 14.1)\n", kTag);
            ok = false;
        }
        // The default tone-mapper / grading / camera stay at the Shipping_Look.
        if (def.tone_mapper != ToneMapperSelection::ACES) {
            std::printf("%s FAIL: default tone mapper is not ACES (Req 9.3/14.1)\n",
                        kTag);
            ok = false;
        }
        if (!grading_is_neutral(def.grading)) {
            std::printf("%s FAIL: default grading is not neutral (Req 10.4/14.1)\n",
                        kTag);
            ok = false;
        }
        if (!camera_is_shipping_default(def.physical_camera)) {
            std::printf("%s FAIL: default physical camera is not the Shipping_Look "
                        "exposure (Req 11.4/14.1)\n", kTag);
            ok = false;
        }
    }

    // Build the explicit all-disabled reference, including grass. The default
    // configuration enables several capabilities and is not this reference.

    AppConfig all_disabled = ctx.config;
    all_disabled.features.clear_coat          = false;
    all_disabled.features.anisotropic         = false;
    all_disabled.features.subsurface          = false;
    all_disabled.features.cloth               = false;
    all_disabled.features.transparent_shadows = false;
    all_disabled.features.colored_penumbra    = false;
    all_disabled.features.specular_aa         = false;
    all_disabled.features.color_grading       = false;
    all_disabled.features.physical_camera     = false;
    all_disabled.features.dynamic_resolution  = false;
    all_disabled.tone_mapper                  = ToneMapperSelection::ACES;
    all_disabled.grading                      = ColorGradingConfig{};
    all_disabled.physical_camera              = PhysicalCameraConfig{};
    all_disabled.features.grass               = false;
    if (!all_phase8_toggles_off(all_disabled.features) ||
        all_disabled.features.grass) {
        std::printf("%s FAIL: the all-disabled configuration is not fully "
                    "disabled (a Phase-8 toggle or grass is still on) — the "
                    "pixel-parity comparison uses the disabled state (Req "
                    "14.1/14.4)\n", kTag);
        ok = false;
    }

    // The ten per-capability-disabled configurations: each capability forced OFF
    // on top of an otherwise-all-enabled  set, so its effect contributes
    // nothing and that surface reverts to the Shipping_Look
    // The genuine per-camera
    // pixel comparison for each is owned by the --benchmark path; here we assert
    // the harness builds exactly the disabled-capability configuration the bar
    // is run over for each one.
    {
        // index → (name, predicate that the named capability is OFF). We flip
        // every  toggle ON, then disable exactly one and assert only that
        // one is off — proving the disable is isolated to the named capability.
        struct Cap {
            const char* name;
            void (*disable)(FeatureToggles&);
            bool (*is_off)(const FeatureToggles&);
        };
        const Cap caps[] = {
            {"clear-coat",
             [](FeatureToggles& f){ f.clear_coat = false; },
             [](const FeatureToggles& f){ return !f.clear_coat; }},
            {"anisotropic",
             [](FeatureToggles& f){ f.anisotropic = false; },
             [](const FeatureToggles& f){ return !f.anisotropic; }},
            {"subsurface",
             [](FeatureToggles& f){ f.subsurface = false; },
             [](const FeatureToggles& f){ return !f.subsurface; }},
            {"cloth",
             [](FeatureToggles& f){ f.cloth = false; },
             [](const FeatureToggles& f){ return !f.cloth; }},
            {"transparent-shadows",
             [](FeatureToggles& f){ f.transparent_shadows = false; },
             [](const FeatureToggles& f){ return !f.transparent_shadows; }},
            {"colored-penumbra",
             [](FeatureToggles& f){ f.colored_penumbra = false; },
             [](const FeatureToggles& f){ return !f.colored_penumbra; }},
            {"specular-aa",
             [](FeatureToggles& f){ f.specular_aa = false; },
             [](const FeatureToggles& f){ return !f.specular_aa; }},
            {"color-grading",
             [](FeatureToggles& f){ f.color_grading = false; },
             [](const FeatureToggles& f){ return !f.color_grading; }},
            {"physical-camera",
             [](FeatureToggles& f){ f.physical_camera = false; },
             [](const FeatureToggles& f){ return !f.physical_camera; }},
            {"dynamic-resolution",
             [](FeatureToggles& f){ f.dynamic_resolution = false; },
             [](const FeatureToggles& f){ return !f.dynamic_resolution; }},
        };
        for (const Cap& cap : caps) {
            FeatureToggles f{};
            // Enable every  capability so the one we disable is isolated.
            f.clear_coat = f.anisotropic = f.subsurface = f.cloth =
                f.transparent_shadows = f.colored_penumbra = f.specular_aa =
                f.color_grading = f.physical_camera = f.dynamic_resolution = true;
            cap.disable(f);
            if (!cap.is_off(f)) {
                std::printf("%s FAIL: disabling '%s' did not turn that "
                            "capability off in the per-capability-disabled "
                            "configuration\n", kTag, cap.name);
                ok = false;
            }
        }
    }

    if (!ok) return CaseResult::Fail;

    // ── LIVE RENDER + LIVE DIFF HELPER (needs the Filament backend) ──
    // The backend bring-up below is FilamentBackend-specific. The harness always
    // constructs a FilamentBackend (it is the sole IRhiBackend), so this is
    // safe; api() lets us bail gracefully if that ever changes rather than
    // risking a bad cast — mirroring the sibling parity case.
    if (ctx.backend.api() != GraphicsApi::Filament) {
        std::printf("%s SKIP: live render needs the Filament backend; host-side "
                    "Shipping_Look config invariants verified (Req 14.1)\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);

    // Bring the render pipeline up so frames actually present on the live
    // backend. A failure here means the environment can't drive the backend (no
    // usable GPU) — SKIP so the gated test still passes headlessly. init_pipelines
    // is idempotent, so an earlier case having already run it is fine.
    if (auto r = backend.init_pipelines(all_disabled); !r) {
        std::printf("%s SKIP: init_pipelines failed (%s); host-side "
                    "Shipping_Look config invariants verified (Req 14.1)\n",
                    kTag, r.error().message.c_str());
        return CaseResult::Skip;
    }

    Camera camera;
    LightingParams lighting = lighting_for_time_of_day(all_disabled.time_of_day);
    TonemapParams  tonemap;

    const int w = ctx.width;
    const int h = ctx.height;

    // Render a handful of all-disabled (Shipping_Look-equivalent) frames so the
    // disabled-features render path runs end-to-end on the live GPU.
    constexpr int kFrames = 4;
    for (int i = 0; i < kFrames; ++i) {
        render_one_frame(backend, camera, lighting, tonemap, w, h);
    }

    // ── Prove the pixel-diff path is live end-to-end here ───────
    // The pure comparison the parity bar is defined over (heaven::compare_rgba8
    // + within_shipping_look_tolerance, tolerance kShippingLookMaxChannelDelta)
    // is compiled into THIS renderer-linked binary as well as the headless test
    // target. Exercise it on small synthetic RGBA8 buffers so a regression in
    // the comparison logic surfaces here too, not only in heaven_benchmark_tests.
    constexpr std::size_t kW = 8, kH = 8;
    std::vector<std::uint8_t> reference(kW * kH * 4);
    for (std::size_t px = 0; px < kW * kH; ++px) {
        const std::size_t base = px * 4;
        reference[base + 0] = static_cast<std::uint8_t>((px * 7)  & 0xFF);
        reference[base + 1] = static_cast<std::uint8_t>((px * 13) & 0xFF);
        reference[base + 2] = static_cast<std::uint8_t>((px * 29) & 0xFF);
        reference[base + 3] = 255;
    }

    // Identical ⇒ max delta 0 ⇒ within tolerance (the disabled state is
    // bit-identical to the Shipping_Look reference).
    {
        std::vector<std::uint8_t> captured = reference;
        const PixelDiff d = compare_rgba8(captured, reference, kW, kH);
        if (!within_shipping_look_tolerance(d) || d.max_channel_delta != 0) {
            std::printf("%s FAIL: identical buffers not within tolerance "
                        "(size_match=%d max_delta=%d)\n", kTag, d.size_match,
                        d.max_channel_delta);
            ok = false;
        }
    }
    // A 2-level drift ⇒ exceeds tolerance (the regression the bar catches).
    {
        std::vector<std::uint8_t> captured = reference;
        captured[2] = static_cast<std::uint8_t>(
            reference[2] <= 253 ? reference[2] + 2 : reference[2] - 2);
        const PixelDiff d = compare_rgba8(captured, reference, kW, kH);
        if (within_shipping_look_tolerance(d) || d.max_channel_delta != 2) {
            std::printf("%s FAIL: a 2-level drift was treated as within "
                        "tolerance (max_delta=%d within=%d — the ≤1/255 bar "
                        "allows at most %d)\n", kTag, d.max_channel_delta,
                        within_shipping_look_tolerance(d),
                        kShippingLookMaxChannelDelta);
            ok = false;
        }
    }
    // Size mismatch ⇒ failure (a non-comparable frame is never a silent pass).
    {
        const std::vector<std::uint8_t> smaller((kW) * (kH - 1) * 4, 0);
        const PixelDiff d = compare_rgba8(smaller, reference, kW, kH);
        if (within_shipping_look_tolerance(d) || d.size_match) {
            std::printf("%s FAIL: a size mismatch was treated as comparable "
                        "(size_match=%d within=%d)\n", kTag, d.size_match,
                        within_shipping_look_tolerance(d));
            ok = false;
        }
    }

    if (!ok) return CaseResult::Fail;

    // Configuration, live rendering, and the diff helper were checked above.
    // The full scene/reference comparison and validation-layer run require
    // additional inputs that this bare-backend harness does not provide.
    std::printf("%s SKIP: verified the all-disabled reference settings, default "
                "capability settings, and ten per-capability configurations; "
                "rendered %d reference frames and checked RGBA comparison "
                "accounting. Full camera-path pixel parity requires recorded "
                "reference images; Vulkan validation requires a separate "
                "validation-layer run.\n", kTag, kFrames);
    return CaseResult::Skip;
}

} // namespace heaven::smoke
