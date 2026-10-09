// Feature: in-ui-configuration,: Edits are applied before save, load, and reload intents
/*
 * HeavenBench — Property test for the intent-ordering invariant.
 *
 * Feature: in-ui-configuration
 *   •: Edits are applied before save, load, and reload intents
 *
 *
 * (intent ordering):
 *   For ANY frame's worth of OverlayIntents — any number of Configuration_Edits,
 *   an optional save (with a real temp-file path or an empty path), an optional
 *   load (whose path may or may not name a real file), and an optional reload —
 *   apply_config_intents() processes them in the fixed global order
 *
 *        edits  <  save  <  load  <  reload
 *
 *   so that every edit is applied before any save/load/reload intent. The
 *   controller records each operation into a ConfigOpLog in execution order; the
 *   property maps each op to a rank (Edit=0, Save=1, Load=2, Reload=3) and
 *   asserts the rank sequence is NON-DECREASING — a single robust check that
 *   simultaneously proves max(Edit) < min(Save) < min(Load) < min(Reload) and
 *   every pairwise ordering between them.
 *
 *   It additionally checks the log is exactly the operations that were requested:
 *     • the count of Edit ops equals intents.edits.size();
 *     • a Save op appears iff intents.save was set;
 *     • a Load op appears iff intents.load was set;
 *     • a Reload op appears iff intents.reload was set OR a load SUCCEEDED — a
 *       successful load forces a Benchmark_Reload for its Reload_Settings even
 *       when the UI did not raise the reload intent, so the
 *       reload-presence expectation accounts for that case explicitly.
 *
 *   Save/load paths use a per-iteration temp directory so the controller's real
 *   write_config_file / load_config_file calls are exercised but contained; a
 *   load is steered to either a real (just-written) file → success, or a
 *   nonexistent file → failure, so BOTH the "load triggers reload" and "load
 *   does not trigger reload" cases are covered without the test wrongly failing.
 *
 * The configuration controller (rhi/overlay/config_controller.{h,cpp}), the
 * Settings Catalog, the Config_File module, and the fake RendererEffects are all
 * GPU-free (no Filament symbol), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via
 * heaven::testing::Register, so the file(GLOB) in benchmark/CMakeLists.txt picks
 * it up without editing the entry point. RapidCheck's default of 100 successful
 * iterations meets the "≥ 100 iterations" requirement (raise via
 * RC_PARAMS=max_success=…).
 */
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include <rapidcheck.h>

#include "bench_config_gen.h"               // heaven::testing::gen_bench_config
#include "config_controller_fakes.h"        // heaven::testing::FakeRendererEffects
#include "engine/config_file.h"             // write_config_file (seed a loadable file)
#include "rhi/overlay/config_controller.h"  // apply_config_intents / ConfigOp / ConfigOpLog
#include "rhi/overlay/config_edit.h"        // OverlayIntents / ConfigurationEdit
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
using heaven::ConfigurationEdit;
using heaven::OverlayIntents;
using heaven::SettingValue;
using heaven::testing::FakeRendererEffects;
using heaven::testing::gen_bench_config;

// Map a ConfigOp to its global execution rank. The controller emits ops in the
// fixed order edits → save → load → reload, so a correct log
// has a NON-DECREASING rank sequence.
[[nodiscard]] int op_rank(ConfigOp op) {
    switch (op) {
        case ConfigOp::Edit:   return 0;
        case ConfigOp::Save:   return 1;
        case ConfigOp::Load:   return 2;
        case ConfigOp::Reload: return 3;
    }
    return -1;
}

[[nodiscard]] int count_op(const ConfigOpLog& log, ConfigOp op) {
    int n = 0;
    for (ConfigOp o : log) {
        if (o == op) ++n;
    }
    return n;
}

// RAII temp directory: created on construction, recursively removed on
// destruction so cleanup happens even if an RC_ASSERT throws out of the lambda.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        static std::atomic<unsigned long long> counter{0};
        const unsigned long long n = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               ("ugx_intent_order_" + std::to_string(n));
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// Draw a type-correct, in-range SettingValue for `entry` by reading the field
// out of a randomly generated BenchConfig. read() returns the variant
// alternative apply_to() expects, so the edit is always well typed; the
// controller clamps on apply, so any boundary value is handled deterministically.

// The one free-form string setting (quality_preset_label) is normalized to a
// JSON-serialization-safe ASCII value: this ordering test serializes the
// Working_Configuration on a save intent, and the JSON writer throws on invalid
// UTF-8 — a robustness concern that is the subject of the save/round-trip
// properties (14/15), not this ordering invariant. The op-ordering under test is
// independent of string CONTENT, so keeping the label ASCII here avoids coupling
// this test to the writer's UTF-8 handling without weakening the property.
[[nodiscard]] SettingValue gen_edit_value(const CatalogEntry& entry) {
    const BenchConfig source = gen_bench_config();
    SettingValue v = entry.read(source);
    if (std::holds_alternative<std::string>(v)) {
        v = std::string("preset");
    }
    return v;
}

// Normalize the only free-form string setting to a serialization-safe value so a
// save intent that writes this config through write_config_file cannot throw on
// non-UTF-8 bytes (see gen_edit_value for the rationale and scope).
void sanitize_for_serialization(BenchConfig& cfg) {
    cfg.quality_preset_label = "preset";
}

bool run_p11_intent_ordering_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 11: edits are applied before "
        "save, load, and reload intents (Validates: Requirements 7.5)",
        [] {
            const std::vector<CatalogEntry>& catalog = heaven::settings_catalog();

            // ── Working / Active configs the controller mutates ──
            BenchConfig working = gen_bench_config();
            BenchConfig active  = gen_bench_config();
            // Keep the Working_Configuration serialization-safe: a save intent
            // writes it through write_config_file (see sanitize_for_serialization).
            sanitize_for_serialization(working);
            ConfigUiState ui;

            OverlayIntents intents;

            // ── Random edits: 0..6 edits for random catalog entries ──
            const int edit_count = *rc::gen::inRange(0, 7);
            for (int i = 0; i < edit_count; ++i) {
                const std::size_t idx =
                    *rc::gen::inRange<std::size_t>(0, catalog.size());
                const CatalogEntry& entry = catalog[idx];
                intents.edits.push_back(
                    ConfigurationEdit{entry.id, gen_edit_value(entry)});
            }

            // Per-iteration temp dir for any real save/load I/O.
            TempDir tmp;

            // ── Random save: with a temp-file path or an empty path ──
            const bool do_save = *rc::gen::arbitrary<bool>();
            if (do_save) {
                intents.save = true;
                const bool empty_path = *rc::gen::arbitrary<bool>();
                if (!empty_path) {
                    intents.save_path = (tmp.path / "saved_config.json").string();
                }
                // empty save_path → controller surfaces a diagnostic, no I/O.
            }

            // ── Random load: cover both the success and failure branches ──
            // With one branch we first write a real config file and point the
            // load at it (so a successful parse exercises the load-triggered
            // reload); with the other we point at a path that
            // does not exist (so the controller's load fails). Both go through
            // the controller's REAL load_config_file, contained in the temp dir.
            const bool do_load = *rc::gen::arbitrary<bool>();
            if (do_load) {
                intents.load = true;
                const bool aim_success = *rc::gen::arbitrary<bool>();
                if (aim_success) {
                    const std::string good =
                        (tmp.path / "loadable_config.json").string();
                    // Write a serialization-safe config so the writer cannot
                    // throw on a non-UTF-8 label; its own diagnostics are ignored
                    // here because we do NOT predict the load outcome — we observe
                    // it below.
                    BenchConfig file_cfg = gen_bench_config();
                    sanitize_for_serialization(file_cfg);
                    const auto write_diagnostics =
                        heaven::write_config_file(good, file_cfg);
                    RC_PRE(write_diagnostics.empty());
                    intents.load_path = good;
                } else {
                    intents.load_path =
                        (tmp.path / "does_not_exist.json").string();
                }
            }

            // ── Random reload intent ──
            intents.reload = *rc::gen::arbitrary<bool>();

            // Determine whether the controller's load will succeed by performing
            // the SAME read it will perform. load_config_file takes the base by
            // const reference and has no side effects on `active` or the file, so
            // this peek is deterministic and matches the controller's decision
            // exactly — far more robust than predicting success from the write's
            // diagnostics, since a written value need not always round-trip to a
            // clean parse. A successful load forces a Benchmark_Reload even when
            // the reload intent is unset.
            bool load_triggers_reload = false;
            if (intents.load) {
                const heaven::ConfigLoadResult peek =
                    heaven::load_config_file(intents.load_path, active);
                load_triggers_reload = peek.parsed_ok;
            }
            const bool expect_reload = intents.reload || load_triggers_reload;

            FakeRendererEffects fx;

            const ConfigOpLog log =
                heaven::apply_config_intents(working, active, ui, intents, fx);

            // ── PRIMARY: the op-rank sequence is non-decreasing ──
            // This single check proves the global ordering edits < save < load
            // < reload: no Save precedes an Edit, no Load
            // precedes a Save, no Reload precedes a Load, and transitively every
            // pairwise ordering between the four groups.
            for (std::size_t i = 1; i < log.size(); ++i) {
                RC_ASSERT(op_rank(log[i - 1]) <= op_rank(log[i]));
            }

            // ── The log is exactly the requested operations ──
            // One Edit op per emitted edit.
            RC_ASSERT(count_op(log, ConfigOp::Edit) ==
                      static_cast<int>(intents.edits.size()));
            // Save/Load appear iff their intent was raised.
            RC_ASSERT(count_op(log, ConfigOp::Save) == (intents.save ? 1 : 0));
            RC_ASSERT(count_op(log, ConfigOp::Load) == (intents.load ? 1 : 0));
            // Reload appears iff the reload intent was raised OR the load
            // actually succeeded (the load-triggered-reload case — Requirement
            // as observed by the deterministic peek above.
            RC_ASSERT(count_op(log, ConfigOp::Reload) == (expect_reload ? 1 : 0));
        });
}

const heaven::testing::Register g_register_intent_ordering{
    &run_p11_intent_ordering_property};

} // namespace
