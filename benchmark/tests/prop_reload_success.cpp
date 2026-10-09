// Feature: in-ui-configuration,: Successful reload advances Active to Working and clears dirty
/*
 * HeavenBench — Property test for a successful Benchmark_Reload.
 *
 * Feature: in-ui-configuration
 *   •: Successful reload advances Active to Working and clears dirty
 *
 *
 * (successful reload advances Active to Working and clears dirty):
 *   For any Working_Configuration, when RendererEffects::rebuild() SUCCEEDS, the
 *   Benchmark_Reload copies the Working_Configuration into the
 *   Active_Configuration so the two are equal over EVERY catalog setting
 * is_dirty() becomes false (— the
 *   Dirty_State is cleared), and ConfigUiState::reload_error is cleared
 *   (— a clean reload leaves no error indication).
 *
 *   The test generates a Working and a SEPARATELY randomized Active config so
 *   they almost always diverge in one or more Reload_Settings (a pre-reload
 *   dirty state). It seeds ConfigUiState with dirty=true and reload_error=true
 *   to prove the successful reload actually CLEARS them rather than merely
 *   leaving them already-false. It raises only the reload intent (no edits, no
 *   save/load) and drives apply_config_intents() with a FakeRendererEffects
 *   whose rebuild() is switched to succeed. It then asserts:
 *     • the active run was cancelled before the rebuild — cancel_count == 1
 *        — and rebuild_count == 1;
 *     • Active equals Working for every catalog entry's governed field
 *        — the transactional copy landed in full;
 *     • is_dirty(working, active) == false and ui.dirty == false
 *
 *     • ui.reload_error == false; and
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

bool run_p9_successful_reload_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 9: successful reload advances "
        "Active to Working and clears dirty (Validates: Requirements 3.3, 3.7, "
        "4.4)",
        [] {
            // A Working and an independently randomized Active config: they
            // almost always differ in some Reload_Setting, modelling a dirty
            // pre-reload state the successful reload must reconcile.
            BenchConfig working = gen_bench_config();
            BenchConfig active  = gen_bench_config();

            // Seed the feedback block as if a reload were pending and a PRIOR
            // reload had errored, so the assertions prove the successful reload
            // CLEARS both flags rather than finding them already false.
            ConfigUiState ui;
            ui.dirty        = true;
            ui.reload_error = true;

            // Only the reload intent is raised — no edits, no save/load — so the
            // sole operation this frame is the Benchmark_Reload.
            OverlayIntents intents;
            intents.reload = true;

            // rebuild() succeeds.
            FakeRendererEffects fx;
            fx.rebuild_succeeds = true;

            const ConfigOpLog log =
                heaven::apply_config_intents(working, active, ui, intents, fx);

            // The active run was cancelled before the rebuild,
            // and exactly one rebuild was attempted.
            RC_ASSERT(fx.cancel_count == 1);
            RC_ASSERT(fx.rebuild_count == 1);

            // Active now equals Working for EVERY catalog-governed setting: the
            // transactional copy landed in full.
            for (const CatalogEntry& entry : heaven::settings_catalog()) {
                RC_ASSERT(entry.read(active) == entry.read(working));
            }

            // Dirty_State is cleared, both as recomputed by is_dirty() and as
            // reflected in the feedback block.
            RC_ASSERT(heaven::is_dirty(working, active) == false);
            RC_ASSERT(ui.dirty == false);

            // The error indication from the prior failed reload is cleared

            RC_ASSERT(ui.reload_error == false);

            // The operation log records the reload.
            RC_ASSERT(std::find(log.begin(), log.end(), ConfigOp::Reload) !=
                      log.end());
        });
}

const heaven::testing::Register g_register_reload_success{
    &run_p9_successful_reload_property};

} // namespace
