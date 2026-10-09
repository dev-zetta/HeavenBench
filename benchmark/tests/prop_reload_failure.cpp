// Feature: in-ui-configuration,: Failed reload is transactional
/*
 * HeavenBench — Property test for a failed Benchmark_Reload.
 *
 * Feature: in-ui-configuration
 *   •: Failed reload is transactional
 *
 *
 * (failed reload is transactional):
 *   For any Working_Configuration, when RendererEffects::rebuild() reports
 *   FAILURE, the Benchmark_Reload must NOT half-apply: the Active_Configuration
 *   equals its pre-reload value over EVERY catalog setting (—
 *   the last good configuration is kept and rendering continues unchanged), the
 *   Dirty_State remains set (is_dirty() is unchanged, because Active never moved
 *   toward Working), and ConfigUiState::reload_error is set so the overlay can
 *   show an error indication.
 *
 *   The test generates a Working and a SEPARATELY randomized Active config and
 *   then forces a guaranteed Reload_Setting divergence (it flips
 *   tessellation.enabled, an Apply::Reload entry) so the pre-reload state is
 *   unambiguously dirty — that is what lets the assertion prove the failed
 *   reload LEAVES it dirty rather than merely finding it already clean. It
 *   snapshots the Active config before the call, seeds ConfigUiState with
 *   dirty=true and reload_error=false (so the assertion proves the failure SETS
 *   the error flag), raises only the reload intent (no edits, no save/load), and
 *   drives apply_config_intents() with a FakeRendererEffects whose rebuild() is
 *   switched to FAIL. It then asserts:
 *     • the active run was cancelled before the rebuild — cancel_count == 1
 *        — and exactly one rebuild was attempted
 *       (rebuild_count == 1);
 *     • Active equals its pre-reload snapshot for every catalog entry's governed
 *       field  — the transactional restore left nothing
 *       half-applied;
 *     • is_dirty(working, active) stays true and ui.dirty stays true, and the
 *       feedback flag matches the recomputed predicate (— dirty
 *       is unchanged because Active never advanced);
 *     • ui.reload_error == true; and
 *     • the operation log records a ConfigOp::Reload.
 *
 * The configuration controller (rhi/overlay/config_controller.{h,cpp}), the
 * Settings Catalog, and the fake RendererEffects are all GPU-free (no Filament
 * symbol, no I/O on this path), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via
 * heaven::testing::Register, so the file(GLOB) in benchmark/CMakeLists.txt picks
 * it up without editing the entry point. RapidCheck's default of 100 successful
 * iterations meets the "≥ 100 iterations" requirement (raise via
 * RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "bench_config_gen.h"               // heaven::testing::gen_bench_config
#include "config_controller_fakes.h"        // heaven::testing::FakeRendererEffects
#include "rhi/overlay/config_controller.h"  // apply_config_intents / is_dirty / ConfigOp
#include "rhi/overlay/settings_catalog.h"   // settings_catalog / CatalogEntry

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
using heaven::ConfigOp;
using heaven::ConfigOpLog;
using heaven::ConfigUiState;
using heaven::OverlayIntents;
using heaven::testing::FakeRendererEffects;
using heaven::testing::gen_bench_config;

bool run_p10_failed_reload_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 10: failed reload is "
        "transactional — Active unchanged, dirty kept, reload_error set "
        "(Validates: Requirements 3.6, 4.5)",
        [] {
            // A Working and an independently randomized Active config.
            BenchConfig working = gen_bench_config();
            BenchConfig active  = gen_bench_config();

            // Force a guaranteed Apply::Reload divergence so the pre-reload state
            // is unambiguously dirty: tessellation.enabled is a Reload_Setting,
            // so flipping it relative to Working makes is_dirty() true regardless
            // of what the random draws produced.
            active.tessellation.enabled = !working.tessellation.enabled;

            // Snapshot the Active config before the call so the assertions can
            // prove it is byte-for-byte unchanged after the failed reload.
            const BenchConfig active_before = active;

            // Sanity: the pre-reload state really is dirty (otherwise the
            // "dirty stays set" assertion would be vacuous).
            RC_PRE(heaven::is_dirty(working, active));

            // Seed the feedback block as if a reload were pending. reload_error
            // starts false so the assertion proves the FAILURE sets it, rather
            // than finding it already true.
            ConfigUiState ui;
            ui.dirty        = true;
            ui.reload_error = false;

            // Only the reload intent is raised — no edits, no save/load — so the
            // sole operation this frame is the Benchmark_Reload.
            OverlayIntents intents;
            intents.reload = true;

            // rebuild() FAILS.
            FakeRendererEffects fx;
            fx.rebuild_succeeds = false;

            const ConfigOpLog log =
                heaven::apply_config_intents(working, active, ui, intents, fx);

            // The active run was cancelled before the rebuild,
            // and exactly one rebuild was attempted.
            RC_ASSERT(fx.cancel_count == 1);
            RC_ASSERT(fx.rebuild_count == 1);

            // Active is byte-for-byte unchanged across EVERY catalog-governed
            // setting: the failed reload restored nothing half-applied

            for (const CatalogEntry& entry : heaven::settings_catalog()) {
                RC_ASSERT(entry.read(active) == entry.read(active_before));
            }

            // Dirty_State remains set: Active never advanced toward Working, so
            // is_dirty() is unchanged and the feedback flag matches the
            // recomputed predicate.
            RC_ASSERT(heaven::is_dirty(working, active) == true);
            RC_ASSERT(ui.dirty == true);
            RC_ASSERT(ui.dirty == heaven::is_dirty(working, active));

            // The failed reload raises the error indication.
            RC_ASSERT(ui.reload_error == true);

            // The operation log records the reload.
            RC_ASSERT(std::find(log.begin(), log.end(), ConfigOp::Reload) !=
                      log.end());
        });
}

const heaven::testing::Register g_register_reload_failure{
    &run_p10_failed_reload_property};

} // namespace
