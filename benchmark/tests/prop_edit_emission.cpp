// Feature: in-ui-configuration,: Exactly one edit per committed control
/*
 * HeavenBench — Property test for the edit-emission helper.
 *
 * Feature: in-ui-configuration
 *   •: Exactly one edit per committed control
 *
 *
 * (exactly one edit per committed control):
 *   For any SettingId and any committed SettingValue, the pure
 *   `heaven::emit_edit(OverlayIntents&, SettingId, SettingValue)` helper appends
 *   EXACTLY ONE ConfigurationEdit carrying that id and value to
 *   OverlayIntents::edits, and leaves every other intent field untouched. This
 *   is the single funnel OverlayUI uses to surface a committed control change
 * so "one commit → one edit" must hold across the full
 *   SettingId enum and every SettingValue variant alternative
 *   (bool / int / float / heaven::QualityLevel / std::string).
 *
 *   The test seeds a fresh OverlayIntents with a random number of pre-existing
 *   edits (and otherwise default intent fields), snapshots every non-edits
 *   field, calls emit_edit once, and asserts:
 *     • edits grew by exactly one (the prior edits are preserved verbatim);
 *     • the newly appended edit carries the generated id and value; and
 *     • no other OverlayIntents field changed (— the helper
 *       represents the request solely as an appended edit, mutating nothing
 *       else).
 *
 * emit_edit is a pure inline helper in the GPU-free value header
 * rhi/overlay/config_edit.h (no Filament, no I/O), so this runs on the host. The
 * file self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/CMakeLists.txt
 * picks it up without editing the entry point. RapidCheck's default of 100
 * successful iterations meets the "≥ 100 iterations" requirement (raise via
 * RC_PARAMS=max_success=…).
 */
#include <cmath>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include <rapidcheck.h>

#include "rhi/overlay/config_edit.h"

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

using heaven::ConfigurationEdit;
using heaven::OverlayIntents;
using heaven::QualityLevel;
using heaven::SettingId;
using heaven::SettingValue;

// Every editable SettingId, in the same order as the enum in config_edit.h. The
// generator draws uniformly from this list so the property exercises the full
// panel partition (Camera → Environment → Tessellation → Settings → Quality →
// Sound).
const std::vector<SettingId>& all_setting_ids() {
    static const std::vector<SettingId> ids = {
        // Camera
        SettingId::PhysicalCameraEnabled, SettingId::CameraAperture,
        SettingId::CameraShutter, SettingId::CameraIso,
        // Environment
        SettingId::TimeOfDay, SettingId::RainIntensity, SettingId::DryDownSeconds,
        SettingId::FeatureRain, SettingId::FeatureWetReflections,
        SettingId::FeatureSky,
        // Tessellation
        SettingId::TessEnabled, SettingId::TessScale, SettingId::TessFactor,
        SettingId::TessDistance,
        // Settings
        SettingId::Width, SettingId::Height, SettingId::Fullscreen, SettingId::Hdr,
        SettingId::HdrPeakNits, SettingId::HdrPaperWhiteNits, SettingId::ClothSimHz,
        // Quality
        SettingId::TextureQuality, SettingId::FeatureGrass, SettingId::FeatureParticles,
        SettingId::FeatureGodRays, SettingId::FeatureVolumetricFog,
        SettingId::FeatureClouds, SettingId::FeatureLensFlares,
        SettingId::FeatureVolumetricClouds, SettingId::FeatureSsgi,
        SettingId::FeatureMassiveFoliage, SettingId::FeatureSoftShadows,
        SettingId::FeatureContactShadows, SettingId::FeaturePointLightShadows,
        SettingId::FeatureReflections, SettingId::WindDebug, SettingId::FeatureMsaa,
        SettingId::FeatureBloom, SettingId::FeatureAmbientOcclusion,
        SettingId::FeatureTaa, SettingId::FeatureDepthOfField,
        SettingId::FeatureRefraction, SettingId::FeatureClearCoat,
        SettingId::FeatureAnisotropic, SettingId::FeatureSubsurface,
        SettingId::FeatureCloth, SettingId::FeatureTransparentShadows,
        SettingId::FeatureColoredPenumbra, SettingId::FeatureSpecularAa,
        SettingId::FeatureGpuInstancing, SettingId::FeatureGpuSkinning,
        SettingId::HeatHazeEnabled, SettingId::SoftShadowLevel,
        SettingId::ContactShadowLevel, SettingId::ShadowLightBudget,
        SettingId::CloudQuality, SettingId::SsgiQuality, SettingId::FoliageDensity,
        SettingId::MsaaSamples, SettingId::QualityPresetLabel,
        // Sound
        SettingId::MusicEnabled, SettingId::MusicVolume,
    };
    return ids;
}

// A SettingValue across every variant alternative. Floats are constrained to be
// finite so the value-equality assertion below is well-defined (NaN never
// compares equal to itself, which would be a spurious failure unrelated to the
// helper's behaviour).
SettingValue gen_setting_value() {
    const int kind = *rc::gen::inRange(0, 9);
    switch (kind) {
        case 0:
            return SettingValue{*rc::gen::arbitrary<bool>()};
        case 1:
            return SettingValue{*rc::gen::arbitrary<int>()};
        case 2:
            return SettingValue{*rc::gen::suchThat<float>(
                [](float f) { return std::isfinite(f); })};
        case 3:
            return SettingValue{*rc::gen::element(
                QualityLevel::Low, QualityLevel::Medium, QualityLevel::High)};
        case 4:
            return SettingValue{static_cast<heaven::RenderPreset>(
                *rc::gen::inRange(0, 5))};
        case 5:
            return SettingValue{static_cast<heaven::MaterialWorkflow>(
                *rc::gen::inRange(0, 2))};
        case 6:
            return SettingValue{static_cast<heaven::TextureQuality>(
                *rc::gen::inRange(0, 3))};
        default:
            return SettingValue{*rc::gen::arbitrary<std::string>()};
    }
}

bool run_p3_exactly_one_edit_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 3: exactly one edit per "
        "committed control (Validates: Requirements 1.4, 7.2)",
        [] {
            // A fresh OverlayIntents pre-seeded with a random number of existing
            // edits, so the property verifies "appends exactly one" rather than
            // merely "ends with one".
            OverlayIntents intents;
            const int prior_count = *rc::gen::inRange(0, 8);
            for (int i = 0; i < prior_count; ++i) {
                intents.edits.push_back(
                    ConfigurationEdit{*rc::gen::element(all_setting_ids().front(),
                                                        all_setting_ids().back()),
                                      gen_setting_value()});
            }
            const std::vector<ConfigurationEdit> edits_before = intents.edits;

            // The committed control change.
            const SettingId    id    = *rc::gen::elementOf(all_setting_ids());
            SettingValue value = gen_setting_value();

            // A copy of the value to compare against, since emit_edit consumes
            // its argument by move.
            const SettingValue expected_value = value;

            heaven::emit_edit(intents, id, std::move(value));

            // Exactly one edit was appended.
            RC_ASSERT(intents.edits.size() == edits_before.size() + 1u);

            // The pre-existing edits are preserved verbatim (nothing reordered
            // or rewritten).
            for (std::size_t i = 0; i < edits_before.size(); ++i) {
                RC_ASSERT(intents.edits[i].id == edits_before[i].id);
                RC_ASSERT(intents.edits[i].value == edits_before[i].value);
            }

            // The newly appended edit carries the committed id and value.
            const ConfigurationEdit& appended = intents.edits.back();
            RC_ASSERT(appended.id == id);
            RC_ASSERT(appended.value == expected_value);

            // No other intent field was touched: the helper's
            // sole effect is appending the edit. Compared against a pristine
            // default OverlayIntents.
            const OverlayIntents fresh;
            RC_ASSERT(intents.start_benchmark == fresh.start_benchmark);
            RC_ASSERT(intents.cancel == fresh.cancel);
            RC_ASSERT(intents.dismiss_result == fresh.dismiss_result);
            RC_ASSERT(intents.quit == fresh.quit);
            RC_ASSERT(intents.open_panel == fresh.open_panel);
            RC_ASSERT(intents.save == fresh.save);
            RC_ASSERT(intents.save_path == fresh.save_path);
            RC_ASSERT(intents.load == fresh.load);
            RC_ASSERT(intents.load_path == fresh.load_path);
            RC_ASSERT(intents.reload == fresh.reload);
            RC_ASSERT(intents.dismiss_diagnostics == fresh.dismiss_diagnostics);
        });
}

const heaven::testing::Register g_register_edit_emission{
    &run_p3_exactly_one_edit_property};

} // namespace
