// Feature: in-ui-configuration,: Out-of-range live reject leaves Active_Configuration and renderer unchanged
/*
 * HeavenBench — Property test for out-of-range live reject.
 *
 * Feature: in-ui-configuration
 *   •: Out-of-range live reject leaves Active_Configuration and
 *     renderer unchanged
 *
 *
 * (faithful interpretation, given this catalog):
 *
 *   The property statement speaks of "a reject-class Live setting (e.g. a
 *   fixed-set setting handled by apply_msaa_samples)". In THIS catalog the only
 *   validator with genuine reject-and-retain semantics is apply_msaa_samples,
 *   and MsaaSamples is classified Apply::Reload (its MSAA target is resolved at
 *   init_pipelines), NOT Live — every Apply::Live setting clamps or wraps rather
 *   than rejecting. So a literal "reject-class Live setting" does not exist here.
 *   The faithful expression of  ("a Live edit whose value is out
 *   of range leaves the Active_Configuration field and the renderer unchanged
 *   from what the shared validator would produce — never the raw out-of-range
 *   value") is therefore split into two complementary checks:
 *
 *   (a) LIVE CLAMP/WRAP NEVER STORES THE RAW VALUE.
 *       For any Apply::Live clamped/wrapped setting and any value the shared
 *       validator changes (an out-of-range input), applying the edit through
 *       apply_config_intents() leaves the Active_Configuration field holding
 *       EXACTLY the shared validator's output (the clamped bound / wrapped
 *       value), which lies within the catalog's declared [min,max] and is never
 *       the raw out-of-range input. The renderer mutation issued for that field
 *       (the recorded RendererEffects::apply_live snapshot) carries the same
 *       in-bounds value, and no OTHER governed field is mutated — so the edit
 *       never pushes the renderer to an out-of-range state. This is the Live
 *       analogue of "reject the raw value".
 *
 *   (b) THE GENUINE REJECT VALIDATOR RETAINS THE PRIOR VALUE.
 *       For the fixed-set validator apply_msaa_samples (MsaaSamples), an
 *       out-of-set request retains the prior count. Because MsaaSamples is
 *       Apply::Reload, the controller additionally leaves Active untouched (a
 *       Reload edit never writes Active) AND issues NO renderer mutation for it.
 *       So an out-of-range MSAA edit independently satisfies "out-of-range
 *       leaves Active_Configuration unchanged and issues no renderer mutation
 *       for that field" — the heart of.
 *
 * The configuration controller (rhi/overlay/config_controller.{h,cpp}), the
 * Settings Catalog, and the shared validators are GPU-free (no Filament, no I/O
 * beyond the Config_File module, which this test never invokes), so this runs on
 * the host. It drives renderer side effects through the recording
 * heaven::testing::FakeRendererEffects fake. The file self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register pattern,
 * so the file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the
 * entry point. RapidCheck's default of 100 successful iterations meets the
 * "≥ 100 iterations" bar (raise via RC_PARAMS=max_success=…).
 */
#include <cmath>
#include <format>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include <rapidcheck.h>

#include "engine/cli_parser.h"
#include "engine/types.h"
#include "rhi/overlay/config_controller.h"
#include "rhi/overlay/config_edit.h"
#include "rhi/overlay/settings_catalog.h"

#include "bench_config_gen.h"
#include "config_controller_fakes.h"

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
using heaven::ConfigurationEdit;
using heaven::OverlayIntents;
using heaven::QualityLevel;
using heaven::SettingId;
using heaven::SettingValue;
using heaven::testing::FakeRendererEffects;

// ── Out-of-range raw generators ──────────────────────────────────

// Draw a finite float strictly OUTSIDE [lo, hi] (either below lo or above hi by
// at least one full span), so the shared validator is guaranteed to change it.
[[nodiscard]] float gen_oor_float(double lo, double hi) {
    if (hi < lo) std::swap(lo, hi);
    const double span = (hi > lo) ? (hi - lo) : 1.0;
    const int mult = *rc::gen::inRange(1, 5);
    if (*rc::gen::inRange(0, 2) == 0) {
        return static_cast<float>(lo - span * static_cast<double>(mult));
    }
    return static_cast<float>(hi + span * static_cast<double>(mult));
}

// Draw an int strictly OUTSIDE [lo, hi] so clamp_* changes it to a bound.
[[nodiscard]] int gen_oor_int(int lo, int hi) {
    if (hi < lo) std::swap(lo, hi);
    if (*rc::gen::inRange(0, 2) == 0) {
        return lo - *rc::gen::inRange(1, 11);
    }
    return hi + *rc::gen::inRange(1, 11);
}

// ── SettingValue extraction ──────────────────────────────────────
[[nodiscard]] float        get_float(const SettingValue& v)   { return *std::get_if<float>(&v); }
[[nodiscard]] int          get_int(const SettingValue& v)     { return *std::get_if<int>(&v); }
[[nodiscard]] QualityLevel get_quality(const SettingValue& v) { return *std::get_if<QualityLevel>(&v); }

// Assert that applying the edit left every governed field EXCEPT `edited`
// exactly as it was in `before` — i.e. the Live edit mutated only its own field
// and pushed no other part of the Active_Configuration anywhere (Requirement
// the renderer is otherwise unchanged).
void assert_only_edited_field_changed(const BenchConfig& after,
                                      const BenchConfig& before,
                                      SettingId edited) {
    for (const CatalogEntry& e : heaven::settings_catalog()) {
        if (e.id == edited) continue;
        RC_ASSERT(e.read(after) == e.read(before));
    }
}

// Assert a numeric stored value lies within the entry's declared bounds.
void assert_within_bounds(const CatalogEntry& e, double value) {
    if (e.min_value != e.max_value) {
        RC_ASSERT(value >= static_cast<double>(e.min_value));
        RC_ASSERT(value <= static_cast<double>(e.max_value));
    }
}

// ── (a) Live clamp/wrap settings never store the raw out-of-range value ──

// For each kind, generate an out-of-range raw value, compute the value the
// shared validator produces VIA THE EXACT path the catalog uses, drive a single
// Live edit through apply_config_intents() starting from working == active, and
// assert: the Active field holds the validator's output (in bounds, ≠ raw); the
// recorded renderer apply carries that same in-bounds value; only that field
// changed; and no reload/cancel/feature mutation occurred.
bool run_p7_live_clamp_never_raw_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 7: out-of-range live edit stores "
        "the validator output, never the raw value (Validates: Requirements 2.6)",
        [] {
            // Start from a randomized but in-lockstep pair: Working == Active, so
            // the only divergence a Live edit can introduce is the field itself.
            const BenchConfig base   = heaven::testing::gen_bench_config();
            BenchConfig        working = base;
            BenchConfig        active  = base;

            // 0,1,2 camera (aperture/shutter/iso) — clamp_camera_*
            // 3      time of day                   — wrap_24
            // 4      rain intensity                — format-then-parse
            // 5      dry-down seconds              — clamp_dry_down_seconds
            // 6      music volume                  — format-then-parse
            // 7      shadow light budget           — clamp_quality_shadow_light_budget
            // 8,9    soft/contact shadow level     — clamp_quality_level
            const int kind = *rc::gen::inRange(0, 10);

            SettingId    id{};
            SettingValue raw_value;     // the out-of-range edit value
            SettingValue expected;      // what the shared validator produces

            switch (kind) {
                case 0: {
                    id = SettingId::CameraAperture;
                    const float raw = gen_oor_float(
                        heaven::PhysicalCameraConfig::k_aperture_lo,
                        heaven::PhysicalCameraConfig::k_aperture_hi);
                    raw_value = SettingValue{raw};
                    expected  = SettingValue{heaven::clamp_camera_aperture(raw).value};
                    break;
                }
                case 1: {
                    id = SettingId::CameraShutter;
                    const float raw = gen_oor_float(
                        heaven::PhysicalCameraConfig::k_shutter_lo,
                        heaven::PhysicalCameraConfig::k_shutter_hi);
                    raw_value = SettingValue{raw};
                    expected  = SettingValue{heaven::clamp_camera_shutter(raw).value};
                    break;
                }
                case 2: {
                    id = SettingId::CameraIso;
                    const float raw = gen_oor_float(
                        heaven::PhysicalCameraConfig::k_iso_lo,
                        heaven::PhysicalCameraConfig::k_iso_hi);
                    raw_value = SettingValue{raw};
                    expected  = SettingValue{heaven::clamp_camera_iso(raw).value};
                    break;
                }
                case 3: {
                    id = SettingId::TimeOfDay;
                    const float raw = gen_oor_float(0.0, 24.0);
                    raw_value = SettingValue{raw};
                    expected  = SettingValue{heaven::wrap_24(raw)};
                    break;
                }
                case 4: {
                    id = SettingId::RainIntensity;
                    const float raw = gen_oor_float(0.0, 1.0);
                    raw_value = SettingValue{raw};
                    // Catalog renders to a token then runs the shared parser;
                    // compute the expectation via the identical path.
                    expected = SettingValue{
                        heaven::parse_rain_intensity(std::format("{}", raw).c_str()).value};
                    break;
                }
                case 5: {
                    id = SettingId::DryDownSeconds;
                    const float raw = gen_oor_float(0.5, 30.0);
                    raw_value = SettingValue{raw};
                    expected  = SettingValue{heaven::clamp_dry_down_seconds(raw)};
                    break;
                }
                case 6: {
                    id = SettingId::MusicVolume;
                    const float raw = gen_oor_float(0.0, 1.0);
                    raw_value = SettingValue{raw};
                    expected = SettingValue{
                        heaven::parse_music_volume(std::format("{}", raw).c_str()).value};
                    break;
                }
                case 7: {
                    id = SettingId::ShadowLightBudget;
                    const int raw = gen_oor_int(0, base.quality.shadow_light_budget_max);
                    raw_value = SettingValue{raw};
                    // The catalog clamps against the config's own quality knobs;
                    // working == active == base, so use base.quality.
                    expected = SettingValue{
                        heaven::clamp_quality_shadow_light_budget(raw, base.quality).value};
                    break;
                }
                default: {
                    id = (kind == 8) ? SettingId::SoftShadowLevel
                                     : SettingId::ContactShadowLevel;
                    const int raw = gen_oor_int(0, 2);
                    raw_value = SettingValue{raw};
                    expected  = SettingValue{heaven::clamp_quality_level(raw).value};
                    break;
                }
            }

            // Drive exactly one Live edit through the controller.
            OverlayIntents intents;
            intents.edits.push_back(ConfigurationEdit{id, raw_value});

            FakeRendererEffects fx;
            heaven::ConfigUiState ui;
            const heaven::ConfigOpLog log =
                heaven::apply_config_intents(working, active, ui, intents, fx);

            // Exactly one edit op, no save/load/reload.
            RC_ASSERT(log.size() == 1u);
            RC_ASSERT(log.front() == heaven::ConfigOp::Edit);

            const CatalogEntry& entry = heaven::catalog_entry(id);

            // (1) Active holds EXACTLY the validator's output — never the raw
            //     out-of-range input.
            const SettingValue active_value = entry.read(active);
            RC_ASSERT(active_value == expected);
            RC_ASSERT(!(active_value == raw_value));

            // (2) Working agrees (Live edits write both configs in lockstep), so
            //     the pair never diverges on a Live setting → not dirty.
            RC_ASSERT(entry.read(working) == expected);
            RC_ASSERT(!ui.dirty);

            // (3) The stored value is inside the catalog's declared bounds.
            if (const float* f = std::get_if<float>(&active_value)) {
                assert_within_bounds(entry, static_cast<double>(*f));
            } else if (const int* i = std::get_if<int>(&active_value)) {
                assert_within_bounds(entry, static_cast<double>(*i));
            } else if (const QualityLevel* q = std::get_if<QualityLevel>(&active_value)) {
                assert_within_bounds(entry, static_cast<double>(static_cast<int>(*q)));
            }

            // (4) The edit mutated ONLY its own governed field in Active.
            assert_only_edited_field_changed(active, base, id);

            // (5) The renderer mutation issued for this Live setting carries the
            //     in-bounds validator output, not the raw value. These settings
            //     are not feature toggles, so exactly one apply_live and no
            //     set_feature_enabled / rebuild. A changed effective value
            //     cancels an active score before its workload is modified.
            RC_ASSERT(fx.live_apply_count(id) == 1);
            RC_ASSERT(fx.live_applies.size() == 1u);
            RC_ASSERT(entry.read(fx.live_applies.front().active) == expected);
            RC_ASSERT(fx.feature_calls.empty());
            RC_ASSERT(fx.cancel_count == (expected != entry.read(base) ? 1 : 0));
            RC_ASSERT(fx.rebuild_count == 0);
        });
}

// ── (b) MSAA reject-and-retain leaves Active and the renderer unchanged ──

// MsaaSamples uses apply_msaa_samples (reject-and-retain) and is Apply::Reload.
// An out-of-set request retains the prior count, and because the setting is
// Reload the controller leaves Active untouched and issues no renderer mutation
// for it — the literal  guarantee for a genuine reject validator.
bool run_p7_msaa_reject_retain_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 7: out-of-set MSAA edit retains "
        "the prior count and leaves Active/renderer unchanged "
        "(Validates: Requirements 2.6)",
        [] {
            const BenchConfig base = heaven::testing::gen_bench_config();

            // Seed a known, accepted prior MSAA count so reject-and-retain is
            // observable, and an out-of-set request the validator must reject.
            const int prior = *rc::gen::element(1, 2, 4);
            const int raw   = *rc::gen::element(-2, 0, 3, 5, 6, 7, 8, 16);

            BenchConfig working = base;
            working.quality.msaa_samples = prior;
            BenchConfig active = working;       // Working == Active to start.

            // The shared validator itself rejects-and-retains.
            const heaven::MsaaApplyResult helper = heaven::apply_msaa_samples(raw, prior);
            RC_ASSERT(helper.rejected);
            RC_ASSERT(helper.value == prior);

            OverlayIntents intents;
            intents.edits.push_back(
                ConfigurationEdit{SettingId::MsaaSamples, SettingValue{raw}});

            FakeRendererEffects fx;
            heaven::ConfigUiState ui;
            const heaven::ConfigOpLog log =
                heaven::apply_config_intents(working, active, ui, intents, fx);

            RC_ASSERT(log.size() == 1u);
            RC_ASSERT(log.front() == heaven::ConfigOp::Edit);

            // Active is untouched: a Reload edit never writes Active, and the
            // validator would have retained the prior count anyway.
            RC_ASSERT(active.quality.msaa_samples == prior);

            // Working retains the prior count too (reject-and-retain), so the
            // out-of-set request changed nothing.
            RC_ASSERT(working.quality.msaa_samples == prior);

            // No renderer mutation whatsoever for this rejected Reload edit.
            RC_ASSERT(fx.live_applies.empty());
            RC_ASSERT(fx.feature_calls.empty());
            RC_ASSERT(fx.cancel_count == 0);
            RC_ASSERT(fx.rebuild_count == 0);

            // Working and Active still agree on every Reload field, so the
            // rejected edit did not dirty the configuration or raise an error.
            RC_ASSERT(!ui.dirty);
            RC_ASSERT(!ui.reload_error);
        });
}

const heaven::testing::Register g_register_live_clamp_never_raw{
    &run_p7_live_clamp_never_raw_property};
const heaven::testing::Register g_register_msaa_reject_retain{
    &run_p7_msaa_reject_retain_property};

} // namespace
