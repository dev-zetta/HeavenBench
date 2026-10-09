// Feature: in-ui-configuration,: Validation parity with the shared engine helpers
/*
 * HeavenBench — Property test for Settings Catalog validation parity.
 *
 * Feature: in-ui-configuration
 *   •: Validation parity with the shared engine helpers
 *
 *
 * (validation parity with the shared engine helpers):
 *   For any bounded or validated CatalogEntry and any raw input value
 *   (including out-of-range and, where applicable, NaN), `entry.apply_to(cfg, v)`
 *   stores EXACTLY the value the corresponding shared engine/types.h helper
 *   produces for that same input, AND that stored value lies within the entry's
 *   declared [min_value, max_value]. The committed value is therefore never less
 *   than the minimum nor greater than the maximum.
 *
 *   This is the guarantee that a value validates identically regardless of
 *   source: a UI edit routed through the catalog clamps/rejects/wraps a raw
 *   input exactly as the CLI flag and the Config_File loader do, because all
 *   three call the same shared helper. The catalog's apply_to() for each entry
 *   routes through one of:
 *     CameraAperture/Shutter/Iso  → clamp_camera_aperture / _shutter / _iso
 *     TimeOfDay                   → wrap_24
 *     RainIntensity               → format-then-parse via parse_rain_intensity
 *     DryDownSeconds              → clamp_dry_down_seconds
 *     Soft/Contact/Cloud/Ssgi Q   → clamp_quality_level
 *     ShadowLightBudget           → clamp_quality_shadow_light_budget
 *     FoliageDensity              → clamp_quality_foliage_density
 *     MsaaSamples                 → apply_msaa_samples (reject-and-retain)
 *     MusicVolume                 → format-then-parse via parse_music_volume
 *
 *   For the two format-then-parse entries (rain intensity, music volume) the
 *   catalog renders the float to a token with std::format("{}", value) and then
 *   runs the shared string parser, so the expected value here is computed via
 *   the IDENTICAL format-then-parse path — making the comparison exact.
 *
 *   For apply_msaa_samples (reject-and-retain) the test seeds cfg with a known
 *   prior sample count and asserts an out-of-set input retains that prior value,
 *   matching the helper.
 *
 * settings_catalog() is GPU-free (depends only on engine/types.h,
 * engine/cli_parser.h, and config_edit.h — no Filament, no I/O), so this runs on
 * the host. The file self-registers with the RapidCheck harness in test_main.cpp
 * via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100 iterations"
 * bar (raise via RC_PARAMS=max_success=…).
 */
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include <rapidcheck.h>

#include "engine/cli_parser.h"
#include "engine/types.h"
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

using heaven::BenchConfig;
using heaven::CatalogEntry;
using heaven::QualityLevel;
using heaven::SettingId;
using heaven::SettingValue;

// ── Raw-input generators ─────────────────────────────────────────

// Generate a finite float spanning a full range one span below `lo` to one
// span above `hi`, so the value lands below, inside (including the bounds), and
// above the entry's range across a run. Drawn from a 1000-step fixed-point grid
// so values stay finite and the clamp branches are exercised uniformly.
float gen_float_spanning(double lo, double hi) {
    const double span = hi - lo;
    const double ext_lo = lo - span;
    const double ext_hi = hi + span;
    const int steps = 1000;
    const int n = *rc::gen::inRange(0, steps + 1);
    return static_cast<float>(ext_lo + (ext_hi - ext_lo) *
                                            static_cast<double>(n) /
                                            static_cast<double>(steps));
}

// Like gen_float_spanning but injects a NaN roughly one draw in six, for the
// float helpers that explicitly sanitize NaN to a finite default.
float gen_float_spanning_or_nan(double lo, double hi) {
    if (*rc::gen::inRange(0, 6) == 0) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return gen_float_spanning(lo, hi);
}

// ── SettingValue extraction (post-apply stored value) ───────────
float    get_float(const SettingValue& v)   { return *std::get_if<float>(&v); }
int      get_int(const SettingValue& v)      { return *std::get_if<int>(&v); }
QualityLevel get_quality(const SettingValue& v) { return *std::get_if<QualityLevel>(&v); }

// Assert the numeric `value` lies within the entry's declared [min,max] when the
// entry is bounded (min_value != max_value).
void assert_within_bounds(const CatalogEntry& e, double value) {
    if (e.min_value != e.max_value) {
        RC_ASSERT(value >= static_cast<double>(e.min_value));
        RC_ASSERT(value <= static_cast<double>(e.max_value));
    }
}

// Apply a raw float through a float-valued entry and assert it equals the value
// `expected` the shared helper produces, and lies within the declared bounds.
void check_float_entry(SettingId id, float raw, float expected) {
    const CatalogEntry& e = heaven::catalog_entry(id);
    BenchConfig cfg;
    const SettingValue stored = e.apply_to(cfg, SettingValue{raw});
    const float got = get_float(stored);
    RC_ASSERT(got == expected);
    assert_within_bounds(e, static_cast<double>(got));
}

bool run_p4_validation_parity_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 4: validation parity with the "
        "shared engine helpers (Validates: Requirements 1.6, 1.7)",
        [] {
            // The shared QualityKnobs the catalog reads bounds from when applying
            // shadow_light_budget / foliage_density: a default BenchConfig's
            // quality, exactly as the catalog uses cfg.quality on a fresh config.
            const heaven::QualityKnobs knobs = BenchConfig{}.quality;

            // 0..2  camera (aperture/shutter/iso)
            // 3     time of day (wrap_24)
            // 4     rain intensity (format-then-parse)
            // 5     dry-down seconds
            // 6..9  quality levels (soft/contact/cloud/ssgi)
            // 10    shadow light budget
            // 11    foliage density
            // 12    msaa samples (reject-and-retain)
            // 13    music volume (format-then-parse)
            const int kind = *rc::gen::inRange(0, 14);

            switch (kind) {
                case 0: {
                    const float raw = gen_float_spanning_or_nan(
                        heaven::PhysicalCameraConfig::k_aperture_lo,
                        heaven::PhysicalCameraConfig::k_aperture_hi);
                    check_float_entry(SettingId::CameraAperture, raw,
                                      heaven::clamp_camera_aperture(raw).value);
                    break;
                }
                case 1: {
                    const float raw = gen_float_spanning_or_nan(
                        heaven::PhysicalCameraConfig::k_shutter_lo,
                        heaven::PhysicalCameraConfig::k_shutter_hi);
                    check_float_entry(SettingId::CameraShutter, raw,
                                      heaven::clamp_camera_shutter(raw).value);
                    break;
                }
                case 2: {
                    const float raw = gen_float_spanning_or_nan(
                        heaven::PhysicalCameraConfig::k_iso_lo,
                        heaven::PhysicalCameraConfig::k_iso_hi);
                    check_float_entry(SettingId::CameraIso, raw,
                                      heaven::clamp_camera_iso(raw).value);
                    break;
                }
                case 3: {
                    // wrap_24 does not sanitize NaN, so NaN is not applicable
                    // here; feed only finite values spanning below/in/above the
                    // [0,24) day so the value is always normalised in range.
                    const float raw = gen_float_spanning(0.0, 24.0);
                    check_float_entry(SettingId::TimeOfDay, raw,
                                      heaven::wrap_24(raw));
                    break;
                }
                case 4: {
                    // Format-then-parse parity: render exactly as the catalog
                    // does, then run the shared parser.
                    const float raw = gen_float_spanning_or_nan(0.0, 1.0);
                    const float expected =
                        heaven::parse_rain_intensity(std::format("{}", raw).c_str()).value;
                    check_float_entry(SettingId::RainIntensity, raw, expected);
                    break;
                }
                case 5: {
                    const float raw = gen_float_spanning_or_nan(0.5, 30.0);
                    check_float_entry(SettingId::DryDownSeconds, raw,
                                      heaven::clamp_dry_down_seconds(raw));
                    break;
                }
                case 6:
                case 7:
                case 8:
                case 9: {
                    static const SettingId level_ids[] = {
                        SettingId::SoftShadowLevel, SettingId::ContactShadowLevel,
                        SettingId::CloudQuality, SettingId::SsgiQuality,
                    };
                    const SettingId id = level_ids[kind - 6];
                    const CatalogEntry& e = heaven::catalog_entry(id);
                    // Span below 0, the {0,1,2} valid set, and above 2.
                    const int raw = *rc::gen::inRange(-3, 7);
                    BenchConfig cfg;
                    const SettingValue stored = e.apply_to(cfg, SettingValue{raw});
                    const QualityLevel got = get_quality(stored);
                    RC_ASSERT(got == heaven::clamp_quality_level(raw).value);
                    assert_within_bounds(e, static_cast<double>(static_cast<int>(got)));
                    break;
                }
                case 10: {
                    const CatalogEntry& e =
                        heaven::catalog_entry(SettingId::ShadowLightBudget);
                    // Span below 0, in-range incl bounds, and above the max (16).
                    const int raw = *rc::gen::inRange(-5, 30);
                    BenchConfig cfg;
                    const SettingValue stored = e.apply_to(cfg, SettingValue{raw});
                    const int got = get_int(stored);
                    RC_ASSERT(got ==
                              heaven::clamp_quality_shadow_light_budget(raw, knobs).value);
                    assert_within_bounds(e, static_cast<double>(got));
                    break;
                }
                case 11: {
                    const float raw = gen_float_spanning_or_nan(
                        1.0, static_cast<double>(knobs.foliage_density_max));
                    check_float_entry(
                        SettingId::FoliageDensity, raw,
                        heaven::clamp_quality_foliage_density(raw, knobs).value);
                    break;
                }
                case 12: {
                    const CatalogEntry& e =
                        heaven::catalog_entry(SettingId::MsaaSamples);
                    // Seed a known prior count so the reject-and-retain branch is
                    // observable: an out-of-set request must retain `prior`.
                    const int prior = *rc::gen::element(1, 2, 4);
                    const int raw = *rc::gen::element(-1, 0, 1, 2, 3, 4, 5, 8);
                    BenchConfig cfg;
                    cfg.quality.msaa_samples = prior;
                    const SettingValue stored = e.apply_to(cfg, SettingValue{raw});
                    const int got = get_int(stored);
                    RC_ASSERT(got == heaven::apply_msaa_samples(raw, prior).value);
                    assert_within_bounds(e, static_cast<double>(got));
                    break;
                }
                default: {
                    const float raw = gen_float_spanning_or_nan(0.0, 1.0);
                    const float expected =
                        heaven::parse_music_volume(std::format("{}", raw).c_str()).value;
                    check_float_entry(SettingId::MusicVolume, raw, expected);
                    break;
                }
            }
        });
}

const heaven::testing::Register g_register_catalog_validation{
    &run_p4_validation_parity_property};

} // namespace
