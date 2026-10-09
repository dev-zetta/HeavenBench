// Feature: in-ui-configuration,: Dirty_State equals reload-setting divergence
/*
 * HeavenBench — Property test for the Dirty_State predicate.
 *
 * Feature: in-ui-configuration
 *   •: Dirty_State equals reload-setting divergence
 *
 *
 * (Dirty_State equals reload-setting divergence):
 *   For any pair of BenchConfig values (Working_Configuration, Active_Configuration),
 *   `heaven::is_dirty(working, active)` is true IF AND ONLY IF the two configs
 *   differ in at least one Apply::Reload catalog entry. Live_Settings never
 *   contribute to the dirty state because the controller writes them into both
 *   configs the moment they are applied, so they cannot diverge (Requirements
 *
 *
 *   The randomized property generates two independent BenchConfigs via the
 *   shared generator, computes the reference predicate INDEPENDENTLY in the test
 *   (iterate settings_catalog(); for every Apply::Reload entry compare
 *   entry.read(working) == entry.read(active); the reference is "any differ"),
 *   and asserts is_dirty() agrees with that reference exactly.
 *
 *   Targeted cases pin down the corners the random draw rarely hits cleanly:
 *     (a) working == active            ⇒ is_dirty is false;
 *     (b) equal pair, flip exactly one Reload setting in working
 *                                      ⇒ is_dirty becomes true (and false again
 *                                         once the change is reverted);
 *     (c) equal pair, flip exactly one Live setting in working (every Reload
 *         setting still equal)         ⇒ is_dirty stays false.
 *   The (b)/(c) mutations go through the catalog's apply_to(), the same shared
 *   validators the live/CLI/load paths use.
 *
 * is_dirty() and the Settings Catalog are GPU-free pure logic, so this runs on
 * the host. The file self-registers with the RapidCheck harness in test_main.cpp
 * via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100 iterations"
 * requirement (raise via RC_PARAMS=max_success=…).
 *
 * No Filament symbol is referenced (controller + catalog + config_edit.h are
 * Filament-free), so this links into the headless heaven_benchmark_tests target.
 */
#include <algorithm>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "rhi/overlay/config_controller.h"
#include "rhi/overlay/settings_catalog.h"

// Shared host-test BenchConfig generator (tests/bench_config_gen.h). Quoted
// includes resolve relative to this file's directory, so the sibling header is
// found without adding tests/ to the target include path.
#include "bench_config_gen.h"

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
using heaven::SettingValue;
using heaven::Widget;
using heaven::is_dirty;
using heaven::settings_catalog;
using heaven::testing::gen_bench_config;

// Reference predicate computed independently of is_dirty(): two configs are
// "reload-divergent" when any Apply::Reload catalog entry reads a different
// value out of each. This intentionally re-implements the contract from scratch
// (catalog filtering + SettingValue equality) so the test does not merely echo
// the implementation under test.
[[nodiscard]] bool reference_reload_divergent(const BenchConfig& a,
                                              const BenchConfig& b) {
    return std::ranges::any_of(settings_catalog(), [&](const CatalogEntry& entry) {
        return entry.apply == Apply::Reload && entry.read(a) != entry.read(b);
    });
}

// is_dirty(working, active) == "differ in any Apply::Reload entry".
bool run_p8_dirty_equals_reload_divergence() {
    return rc::check(
        "Feature: in-ui-configuration, Property 8: Dirty_State equals "
        "reload-setting divergence (Validates: Requirements 3.2, 4.1)",
        [] {
            const BenchConfig working = gen_bench_config();
            const BenchConfig active  = gen_bench_config();

            const bool reference = reference_reload_divergent(working, active);
            RC_ASSERT(is_dirty(working, active) == reference);

            // is_dirty is symmetric in its arguments (it only compares fields).
            // NOLINTNEXTLINE(readability-suspicious-call-argument) -- intentional symmetry test.
            RC_ASSERT(is_dirty(active, working) == reference);
        });
}

// TARGETED (a): an identical pair is never dirty, regardless of the (possibly
// out-of-range) values the generator produced.
bool run_p8_equal_pair_not_dirty() {
    return rc::check(
        "Feature: in-ui-configuration, Property 8 (targeted a): identical "
        "Working/Active is not dirty",
        [] {
            const BenchConfig cfg = gen_bench_config();
            RC_ASSERT(is_dirty(cfg, cfg) == false);

            // A separately-taken copy compares equal too.
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) -- exercises value copying.
            const BenchConfig copy = cfg;
            RC_ASSERT(is_dirty(cfg, copy) == false);
        });
}

// TARGETED (b): from an equal pair, flipping exactly one Reload bool setting in
// the working config makes is_dirty true; reverting it makes it false again.
// Every Apply::Reload Checkbox entry is exercised, since flipping a bool through
// apply_to() is guaranteed to produce a divergence.
bool run_p8_single_reload_change_is_dirty() {
    return rc::check(
        "Feature: in-ui-configuration, Property 8 (targeted b): one Reload "
        "change sets dirty",
        [] {
            const BenchConfig base = gen_bench_config();

            for (const CatalogEntry& entry : settings_catalog()) {
                if (entry.apply != Apply::Reload) continue;
                if (entry.widget != Widget::Checkbox) continue;

                BenchConfig working = base;
                const BenchConfig& active = base;

                // Pre-condition: the untouched pair is clean.
                RC_ASSERT(is_dirty(working, active) == false);

                // Flip exactly this one Reload setting in working.
                const bool current = std::get<bool>(entry.read(working));
                entry.apply_to(working, SettingValue{!current});

                // The flipped Reload setting now diverges → dirty.
                RC_ASSERT(std::get<bool>(entry.read(working)) != current);
                RC_ASSERT(is_dirty(working, active) == true);

                // Reverting the single change clears the divergence.
                entry.apply_to(working, SettingValue{current});
                RC_ASSERT(is_dirty(working, active) == false);
            }
        });
}

// TARGETED (c): from an equal pair, flipping exactly one Live bool setting in
// the working config (leaving every Reload setting equal) leaves is_dirty false,
// because Live_Settings never contribute to the dirty state.
bool run_p8_single_live_change_stays_clean() {
    return rc::check(
        "Feature: in-ui-configuration, Property 8 (targeted c): one Live "
        "change stays clean",
        [] {
            const BenchConfig base = gen_bench_config();

            for (const CatalogEntry& entry : settings_catalog()) {
                if (entry.apply != Apply::Live) continue;
                if (entry.widget != Widget::Checkbox) continue;

                BenchConfig working = base;
                const BenchConfig& active = base;

                RC_ASSERT(is_dirty(working, active) == false);

                // Flip exactly this one Live setting in working only.
                const bool current = std::get<bool>(entry.read(working));
                entry.apply_to(working, SettingValue{!current});
                RC_ASSERT(std::get<bool>(entry.read(working)) != current);

                // Reload settings are still all equal → not dirty despite the
                // Live divergence.
                RC_ASSERT(is_dirty(working, active) == false);
            }
        });
}

const heaven::testing::Register g_register_dirty_state_property{
    &run_p8_dirty_equals_reload_divergence};
const heaven::testing::Register g_register_dirty_equal_pair{
    &run_p8_equal_pair_not_dirty};
const heaven::testing::Register g_register_dirty_single_reload{
    &run_p8_single_reload_change_is_dirty};
const heaven::testing::Register g_register_dirty_single_live{
    &run_p8_single_live_change_stays_clean};

} // namespace
