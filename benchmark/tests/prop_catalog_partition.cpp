// Feature: in-ui-configuration,: Catalog is a total, unique panel partition
/*
 * HeavenBench — Property test for the Settings Catalog partition.
 *
 * Feature: in-ui-configuration
 *   •: Catalog is a total, unique panel partition
 *
 *
 * (catalog is a total, unique panel partition):
 *   For any build of the Settings Catalog, every editable SettingId appears in
 *   EXACTLY ONE CatalogEntry (total + unique), each entry's `panel` is one of
 *   the six valid Panel values, and the per-panel entry counts are well-formed:
 *   the number of catalog entries equals the number of SettingId enumerators
 *   (so panels partition the full setting space), and every one of the six
 *   panels owns at least one entry. This is the structural guarantee that lets
 *   OverlayUI render exactly one control per governed setting
 *   with each setting assigned to exactly one panel.
 *
 *   The catalog is a static, build-once table, so the check is fully
 *   deterministic. Following the harness convention (and the fact that
 *   "a deterministic exhaustive check wrapped in rc::check is fine"), the
 *   exhaustive partition verification is wrapped in rc::check so it runs under
 *   the shared RapidCheck driver and satisfies the ≥100-iteration convention
 *   (RapidCheck's default of 100 successful iterations; raise via
 *   RC_PARAMS=max_success=…). The body draws a random SettingId each iteration
 *   and additionally re-verifies the whole-catalog invariants, so both the
 *   per-id "appears in exactly one entry" property and the global partition
 *   counts are exercised across the run.
 *
 * settings_catalog() is a GPU-free table (depends only on engine/types.h,
 * engine/cli_parser.h, and config_edit.h — no Filament, no I/O), so this runs on
 * the host. The file self-registers with the RapidCheck harness in test_main.cpp
 * via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 */
#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

#include <rapidcheck.h>

#include "rhi/overlay/config_edit.h"
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

using heaven::CatalogEntry;
using heaven::Panel;
using heaven::SettingId;

// Every editable SettingId, in the same order as the enum in config_edit.h.
// This is the authoritative enumeration the catalog must partition: the test
// asserts each of these appears in exactly one entry and that the catalog holds
// exactly this many entries.
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
        SettingId::Width, SettingId::Height, SettingId::Fullscreen, SettingId::FrameGeneration, SettingId::Hdr,
        SettingId::HdrPeakNits, SettingId::HdrPaperWhiteNits, SettingId::ClothSimHz,
        // Quality
        SettingId::TextureQuality, SettingId::FeatureGrass, SettingId::FeatureParticles,
        SettingId::FeatureGodRays, SettingId::FeatureVolumetricFog,
        SettingId::FeatureClouds, SettingId::FeatureLensFlares,
        SettingId::FeatureVolumetricClouds, SettingId::FeatureSsgi,
        SettingId::FeatureMassiveFoliage, SettingId::FeatureGroundRelief,
        SettingId::FeatureSoftShadows,
        SettingId::FeatureContactShadows, SettingId::FeaturePointLightShadows,
        SettingId::FeatureReflections, SettingId::WindDebug, SettingId::FeatureMsaa,
        SettingId::FeatureBloom, SettingId::BloomStrength, SettingId::FeatureAmbientOcclusion,
        SettingId::FeatureTaa, SettingId::FeatureDepthOfField,
        SettingId::FeatureRefraction, SettingId::FeatureClearCoat,
        SettingId::FeatureAnisotropic, SettingId::FeatureSubsurface,
        SettingId::FeatureCloth, SettingId::FeatureTransparentShadows,
        SettingId::FeatureColoredPenumbra, SettingId::FeatureSpecularAa,
        SettingId::FeatureGpuInstancing, SettingId::FeatureGpuSkinning,
        SettingId::HeatHazeEnabled, SettingId::SoftShadowLevel,
        SettingId::ContactShadowLevel, SettingId::ShadowLightBudget,
        SettingId::CloudQuality, SettingId::SsgiQuality, SettingId::AoQuality,
        SettingId::BloomQuality, SettingId::DofQuality, SettingId::TaaQuality,
        SettingId::FoliageDensity, SettingId::MsaaSamples,
        SettingId::SunShadowMapSize, SettingId::PointShadowMapSize,
        SettingId::RenderPreset, SettingId::MaterialWorkflow,
        SettingId::CpuStressEnabled, SettingId::CpuStressWorkers,
        SettingId::CpuStressDuration, SettingId::QualityPresetLabel,
        // Sound
        SettingId::MusicEnabled, SettingId::MusicVolume, SettingId::RenderTestScene,
    };
    return ids;
}

// The six valid panels. Used to assert every entry's panel is
// one of these and that each one owns at least one entry.
const std::vector<Panel>& all_panels() {
    static const std::vector<Panel> panels = {
        Panel::Camera, Panel::Environment, Panel::Tessellation,
        Panel::Settings, Panel::Quality,  Panel::Sound,
    };
    return panels;
}

bool is_valid_panel(Panel p) {
    return std::ranges::any_of(all_panels(), [p](Panel valid) { return p == valid; });
}

bool run_p1_catalog_partition_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 1: catalog is a total, unique "
        "panel partition (Validates: Requirements 1.1, 1.3)",
        [] {
            const std::vector<CatalogEntry>& catalog = heaven::settings_catalog();
            const std::vector<SettingId>&    ids     = all_setting_ids();

            // ── (c) Cardinality: the catalog holds exactly one entry per
            //        SettingId enumerator (no missing, no extra). Combined with
            //        the uniqueness check below this proves a 1:1 partition.
            RC_ASSERT(catalog.size() == ids.size());

            // ── Count how many entries carry each SettingId, and tally
            //        per-panel entry counts. Every entry's panel must be valid
            //        (b).
            std::unordered_map<int, int>   id_count;
            std::unordered_map<int, int>   panel_count;
            for (const CatalogEntry& entry : catalog) {
                RC_ASSERT(is_valid_panel(entry.panel));
                id_count[static_cast<int>(entry.id)]   += 1;
                panel_count[static_cast<int>(entry.panel)] += 1;
            }

            // ── (a) Totality + uniqueness: every SettingId enumerator appears
            //        in EXACTLY ONE entry.
            for (SettingId id : ids) {
                RC_ASSERT(id_count[static_cast<int>(id)] == 1);
            }

            // The number of distinct ids seen equals the enumerator count, so no
            // entry carries an id outside the authoritative enumeration.
            RC_ASSERT(id_count.size() == ids.size());

            // ── (d) Every one of the six panels owns at least one entry, and
            //        the per-panel counts sum to the full catalog (well-formed
            //        partition by panel).
            int summed = 0;
            for (Panel panel : all_panels()) {
                const int count = panel_count[static_cast<int>(panel)];
                RC_ASSERT(count > 0);
                summed += count;
            }
            RC_ASSERT(static_cast<std::size_t>(summed) == catalog.size());

            // ── Per-iteration spot check: a randomly drawn SettingId resolves
            //        through catalog_entry() to a single entry whose id matches
            //        and whose panel is valid (re-exercises the lookup the
            //        controller and OverlayUI rely on).
            const SettingId picked = *rc::gen::elementOf(ids);
            const CatalogEntry& resolved = heaven::catalog_entry(picked);
            RC_ASSERT(resolved.id == picked);
            RC_ASSERT(is_valid_panel(resolved.panel));
        });
}

const heaven::testing::Register g_register_catalog_partition{
    &run_p1_catalog_partition_property};

} // namespace
