// Feature: in-ui-configuration,: Edit routing by Live/Reload classification
/*
 * HeavenBench — Property test for configuration-edit routing.
 *
 * Feature: in-ui-configuration
 *   •: Edit routing by Live/Reload classification
 *
 *
 * (edit routing by Live/Reload classification):
 *   For any CatalogEntry and any value applied through apply_config_intents():
 *     • the value is ALWAYS written into the Working_Configuration
 *       (for Reload settings; the working copy always tracks the
 *       user's edits);
 *     • if the entry is Apply::Live the value is ALSO written into the
 *       Active_Configuration  and the edit triggers NO
 *       Benchmark_Reload (— a live change takes effect without a
 *       rebuild);
 *     • a Live edit cancels the active run exactly once iff its validated
 *       stored value changes Active; no-op/rejected Live edits and pending
 *       Reload edits do not cancel it;
 *     • if the entry is Apply::Reload the value is NOT written into the
 *       Active_Configuration (— a reload setting only lands in
 *       Active on a Benchmark_Reload);
 *     • after the edit, is_dirty() reflects ONLY Reload-setting divergence
 *       between Working and Active: a pure Live edit on two initially-equal
 *       configs leaves it false, while a Reload edit that changes the stored
 *       value makes it true. ConfigUiState::dirty mirrors is_dirty() exactly.
 *
 *   The test starts from Working == Active (two equal copies of a randomized
 *   BenchConfig), picks one random catalog entry, derives the post-clamp value
 *   the catalog would store (by running the entry's apply_to on a scratch copy),
 *   emits a single edit for it, and runs apply_config_intents() against the
 *   recording FakeRendererEffects. It then asserts the routing, no-reload, and
 *   dirty-state invariants above.
 *
 * The configuration controller and the Settings Catalog are GPU-free
 * (config_controller.{h,cpp} / settings_catalog.{h,cpp} depend only on
 * engine/types.h, engine/cli_parser.h, engine/config_file.h, and config_edit.h —
 * no Filament), so this runs on the host. The file self-registers with the
 * RapidCheck harness in test_main.cpp via heaven::testing::Register, so the
 * file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the entry
 * point. RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <variant>
#include <vector>

#include <rapidcheck.h>

#include "rhi/overlay/config_controller.h"
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

using heaven::Apply;
using heaven::BenchConfig;
using heaven::CatalogEntry;
using heaven::ConfigOp;
using heaven::ConfigOpLog;
using heaven::ConfigUiState;
using heaven::OverlayIntents;
using heaven::MaterialWorkflow;
using heaven::QualityLevel;
using heaven::RenderPreset;
using heaven::SettingValue;
using heaven::TextureQuality;
using heaven::testing::FakeRendererEffects;
using heaven::testing::gen_finite_float;
using heaven::testing::gen_quality_level;
using heaven::testing::gen_ranged_float;
using heaven::testing::gen_ranged_int;

// Draw an edit value of the SAME variant alternative the entry's field uses
// (discovered via the entry's current read()). Bounded numerics draw across
// their declared [min,max] (including boundary and out-of-range probes via the
// shared gen_ranged_* helpers); unbounded numerics draw a broad finite value.
// Whatever is drawn, the catalog's apply_to clamps/rejects deterministically, so
// the routing invariants hold against the post-clamp stored value regardless of
// validity —  separately pins validation parity.
[[nodiscard]] SettingValue gen_edit_value(const CatalogEntry& entry,
                                          const SettingValue&  current) {
    const bool bounded = entry.min_value < entry.max_value;
    return std::visit(
        [&](auto&& cur) -> SettingValue {
            using T = std::decay_t<decltype(cur)>;
            if constexpr (std::is_same_v<T, bool>) {
                return SettingValue{*rc::gen::arbitrary<bool>()};
            } else if constexpr (std::is_same_v<T, int>) {
                if (bounded) {
                    return SettingValue{gen_ranged_int(
                        static_cast<int>(entry.min_value),
                        static_cast<int>(entry.max_value))};
                }
                return SettingValue{*rc::gen::inRange(-10000, 10001)};
            } else if constexpr (std::is_same_v<T, float>) {
                if (bounded) {
                    return SettingValue{gen_ranged_float(entry.min_value,
                                                         entry.max_value)};
                }
                return SettingValue{gen_finite_float()};
            } else if constexpr (std::is_same_v<T, QualityLevel>) {
                return SettingValue{gen_quality_level()};
            } else if constexpr (std::is_same_v<T, RenderPreset>) {
                return SettingValue{static_cast<RenderPreset>(
                    *rc::gen::inRange(0, 5))};
            } else if constexpr (std::is_same_v<T, MaterialWorkflow>) {
                return SettingValue{static_cast<MaterialWorkflow>(
                    *rc::gen::inRange(0, 2))};
            } else if constexpr (std::is_same_v<T, TextureQuality>) {
                return SettingValue{static_cast<TextureQuality>(
                    *rc::gen::inRange(0, 3))};
            } else {
                return SettingValue{*rc::gen::arbitrary<std::string>()};
            }
        },
        current);
}

bool run_p5_edit_routing_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 5: edit routing by Live/Reload "
        "classification (Validates: Requirements 2.1, 2.4, 3.1)",
        [] {
            const std::vector<CatalogEntry>& catalog = heaven::settings_catalog();

            // Pick a single random catalog entry to edit this iteration.
            const std::size_t idx =
                *rc::gen::inRange<std::size_t>(0, catalog.size());
            const CatalogEntry& entry = catalog[idx];

            // Working == Active: two equal copies of a randomized config, so any
            // divergence after the edit is attributable to routing alone.
            BenchConfig working = heaven::testing::gen_bench_config();
            BenchConfig active  = working;

            // The Active value of the governed field BEFORE the edit, used to
            // assert a Reload edit leaves Active untouched.
            const SettingValue active_field_before = entry.read(active);

            // Derive the post-clamp value the catalog will store, by running the
            // SAME apply_to on a scratch copy of Working. This is what
            // entry.read(working) must equal afterwards regardless of clamping.
            const SettingValue edit_value =
                gen_edit_value(entry, entry.read(working));
            BenchConfig        scratch        = working;
            const SettingValue expected_stored =
                entry.apply_to(scratch, edit_value);

            // One committed edit for the chosen setting.
            OverlayIntents intents;
            heaven::emit_edit(intents, entry.id, edit_value);

            ConfigUiState       ui;
            FakeRendererEffects fx;
            const ConfigOpLog   log =
                heaven::apply_config_intents(working, active, ui, intents, fx);

            // (1) Working always reflects the committed (post-clamp) value.
            RC_ASSERT(entry.read(working) == expected_stored);

            // (2) A plain edit never rebuilds. Only an effective Live change
            //     cancels the run; no-op/rejected edits and pending Reload edits
            //     retain it.
            RC_ASSERT(std::find(log.begin(), log.end(), ConfigOp::Reload) ==
                      log.end());
            RC_ASSERT(fx.rebuild_count == 0);
            const bool live_changed = entry.apply == Apply::Live &&
                                      expected_stored != active_field_before;
            RC_ASSERT(fx.cancel_count == (live_changed ? 1 : 0));

            // (3) Live vs Reload routing into Active.
            if (entry.apply == Apply::Live) {
                // Written through to Active in lockstep.
                RC_ASSERT(entry.read(active) == entry.read(working));
            } else {
                // Left untouched until a reload.
                RC_ASSERT(entry.read(active) == active_field_before);
            }

            // (4) Dirty_State mirrors is_dirty() exactly, and is driven solely by
            //     Reload-setting divergence.
            RC_ASSERT(ui.dirty == heaven::is_dirty(working, active));

            if (entry.apply == Apply::Live) {
                // A pure Live edit on initially-equal configs never dirties:
                // both copies move together, so no Reload setting diverges.
                RC_ASSERT(ui.dirty == false);
            } else {
                // A Reload edit dirties iff it actually changed the stored value
                // relative to the (unchanged) Active field. Only this one entry
                // could have diverged, since the configs started equal.
                const bool changed = !(entry.read(working) == active_field_before);
                RC_ASSERT(ui.dirty == changed);
            }
        });
}

const heaven::testing::Register g_register_edit_routing{
    &run_p5_edit_routing_property};

} // namespace
