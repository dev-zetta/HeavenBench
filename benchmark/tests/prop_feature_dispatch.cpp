// Feature: in-ui-configuration,: Live feature toggles dispatch to set_enabled; unknown is a no-op
/*
 * HeavenBench — Property test for live feature-toggle dispatch.
 *
 * Feature: in-ui-configuration
 *   •: Live feature toggles dispatch to set_enabled; unknown is a no-op
 *
 *
 * (live feature toggles dispatch to set_enabled; unknown is a no-op):
 *   For any Live feature-toggle CatalogEntry (one whose SettingId maps to a
 *   registered IRenderFeature name via heaven::live_feature_name) and any
 *   boolean value, applying that edit through apply_config_intents invokes the
 *   backend set_feature_enabled(feature_name, value) EXACTLY ONCE with the
 *   catalog's feature name and the post-apply boolean. For any
 *   Live setting that maps to NO registered feature (live_feature_name ==
 *   nullptr — time-of-day, music volume, the per-frame shadow/quality knobs,
 *   etc.) the controller issues ZERO set_feature_enabled calls and instead
 *   routes the change through apply_live, so a toggle with no feature name can
 *   never reach the backend toggle path. The actual backend
 *   no-op for an unmatched-but-non-null name is covered by the weather
 *   smoke tests; here we assert the controller's dispatch contract — it only
 *   ever calls set_feature_enabled with the EXACT live_feature_name(id) string,
 *   never a bogus one.
 *   Ordinary known Live settings cancel an active run exactly once when the
 *   stored Active value changes. Frame generation instead keeps playback and
 *   permanently marks the tour diagnostic (unit_frame_generation covers that
 *   latch). No-op/rejected edits never cancel. A null feature-name mapping is
 *   not an unknown configuration key.
 *
 *   The controller (rhi/overlay/config_controller.{h,cpp}) drives all renderer
 *   side effects through the abstract heaven::RendererEffects interface, so the
 *   test substitutes the recording FakeRendererEffects (tests/config_controller_
 *   fakes.h) and asserts exact call counts/arguments. Everything here is GPU-free
 *   pure logic over a BenchConfig, so it runs on the host.
 *
 * The file self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/CMakeLists.txt
 * picks it up without editing the entry point. RapidCheck's default of 100
 * successful iterations meets the "≥ 100 iterations" requirement (raise via
 * RC_PARAMS=max_success=…).
 */
#include <functional>
#include <variant>
#include <vector>

#include <rapidcheck.h>

#include "bench_config_gen.h"               // heaven::testing::gen_bench_config()
#include "config_controller_fakes.h"        // heaven::testing::FakeRendererEffects
#include "rhi/overlay/config_controller.h"  // apply_config_intents / live_feature_name
#include "rhi/overlay/config_edit.h"        // OverlayIntents / emit_edit / SettingId
#include "rhi/overlay/settings_catalog.h"   // settings_catalog / Apply

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

using heaven::Apply;
using heaven::BenchConfig;
using heaven::CatalogEntry;
using heaven::ConfigUiState;
using heaven::OverlayIntents;
using heaven::SettingId;
using heaven::SettingValue;
using heaven::testing::FakeRendererEffects;
using heaven::testing::gen_bench_config;

// The SettingIds that the controller dispatches through set_feature_enabled:
// every Apply::Live entry whose live_feature_name(id) is non-null. the current
// classification names eleven such live feature toggles; we collect them from
// the catalog at runtime so the test tracks the catalog/controller rather than a
// hand-copied list. Massive Foliage is intentionally Reload because its
// enhanced-terrain topology is generated during scene loading.
std::vector<SettingId> live_feature_toggle_ids() {
    std::vector<SettingId> ids;
    for (const CatalogEntry& entry : heaven::settings_catalog()) {
        if (entry.apply == Apply::Live &&
            heaven::live_feature_name(entry.id) != nullptr) {
            ids.push_back(entry.id);
        }
    }
    return ids;
}

// The Live settings that are NOT feature toggles: Apply::Live entries whose
// live_feature_name(id) is null (time-of-day, weather knobs, exposure, music
// volume, the per-frame shadow/quality knobs, the preset label, …). These must
// route through apply_live, never through set_feature_enabled.
std::vector<SettingId> live_non_feature_ids() {
    std::vector<SettingId> ids;
    for (const CatalogEntry& entry : heaven::settings_catalog()) {
        if (entry.apply == Apply::Live &&
            heaven::live_feature_name(entry.id) == nullptr) {
            ids.push_back(entry.id);
        }
    }
    return ids;
}

bool run_p6_feature_dispatch_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 6: live feature toggles dispatch "
        "to set_enabled; unknown is a no-op (Validates: Requirements 2.2, 2.7)",
        [] {
            const std::vector<SettingId> feature_ids     = live_feature_toggle_ids();
            const std::vector<SettingId> non_feature_ids = live_non_feature_ids();

            // Structural sanity: the implementation names exactly eleven live feature
            // toggles, each mapping to a distinct registered feature name.
            RC_ASSERT(feature_ids.size() == 11u);

            // ── PART A: a live feature toggle dispatches exactly one
            //            set_feature_enabled(feature_name, value) call and NO
            //            apply_live. ────────────────────────
            {
                const SettingId id    = *rc::gen::elementOf(feature_ids);
                const bool      value = *rc::gen::arbitrary<bool>();

                BenchConfig working = gen_bench_config();
                BenchConfig active  = working;  // start in lockstep
                const CatalogEntry& entry = heaven::catalog_entry(id);
                const SettingValue active_before = entry.read(active);
                ConfigUiState ui;

                OverlayIntents intents;
                heaven::emit_edit(intents, id, SettingValue{value});

                FakeRendererEffects fx;
                heaven::apply_config_intents(working, active, ui, intents, fx);

                const char* expected_name = heaven::live_feature_name(id);
                RC_ASSERT(expected_name != nullptr);

                // EXACTLY ONE set_feature_enabled call, carrying the catalog's
                // feature name and the committed boolean (feature toggles are
                // stored verbatim, so the post-apply bool equals `value`).
                RC_ASSERT(fx.feature_calls.size() == 1u);
                RC_ASSERT(fx.feature_calls[0].name == std::string(expected_name));
                RC_ASSERT(fx.feature_calls[0].on == value);

                // Exactly one call NAMED the catalog feature (no bogus name) and
                // the value round-tripped onto Active_Configuration.
                RC_ASSERT(fx.feature_call_count(expected_name) == 1);

                // Feature toggles dispatch via set_feature_enabled, NOT apply_live.
                RC_ASSERT(fx.live_applies.empty());

                // No reload was triggered by a Live edit.
                RC_ASSERT(fx.rebuild_count == 0);
                RC_ASSERT(fx.cancel_count ==
                          (id != SettingId::FrameGeneration &&
                           entry.read(active) != active_before ? 1 : 0));
            }

            // ── PART B: a live NON-feature setting routes through apply_live and
            //            issues ZERO set_feature_enabled calls — a SettingId with
            //            live_feature_name == nullptr can never reach the backend
            //            toggle path. ─────────────────────────
            {
                const SettingId id = *rc::gen::elementOf(non_feature_ids);

                // live_feature_name returns nullptr for every non-feature id
                // (the direct contract assertion the test calls for).
                RC_ASSERT(heaven::live_feature_name(id) == nullptr);

                BenchConfig working = gen_bench_config();
                BenchConfig active  = working;
                ConfigUiState ui;

                // Build a correctly-typed edit value by reading the governed
                // field out of a generated config: read() yields a SettingValue
                // of the field's own variant alternative, so apply_to never sees
                // a mismatched type.
                const CatalogEntry& entry = heaven::catalog_entry(id);
                const SettingValue active_before = entry.read(active);
                const BenchConfig source  = gen_bench_config();
                const SettingValue value  = entry.read(source);

                OverlayIntents intents;
                heaven::emit_edit(intents, id, value);

                FakeRendererEffects fx;
                heaven::apply_config_intents(working, active, ui, intents, fx);

                // No feature-toggle dispatch for a non-feature live setting.
                RC_ASSERT(fx.feature_calls.empty());

                // Instead, the change routes through apply_live exactly once for
                // this id.
                RC_ASSERT(fx.live_apply_count(id) == 1);
                RC_ASSERT(fx.live_applies.size() == 1u);
                RC_ASSERT(fx.live_applies[0].id == id);

                // Still no reload for a Live edit.
                RC_ASSERT(fx.rebuild_count == 0);
                RC_ASSERT(fx.cancel_count ==
                          (id != SettingId::FrameGeneration &&
                           entry.read(active) != active_before ? 1 : 0));
            }
        });
}

const heaven::testing::Register g_register_feature_dispatch{
    &run_p6_feature_dispatch_property};

} // namespace
