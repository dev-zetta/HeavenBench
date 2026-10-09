// Feature: in-ui-configuration,: Load result handling (success copies; error is atomic)
/*
 * HeavenBench — Property test for load-result handling in the
 * configuration controller.
 *
 * Feature: in-ui-configuration
 *   •: Load result handling (success copies; error is atomic)
 *
 *
 * (load result handling):
 *   When load reports parse success, the loaded BenchConfig is copied into the
 *   Working_Configuration, every Live_Setting is applied to
 *   the renderer, and a Benchmark_Reload is requested for the
 *   Reload_Settings. When load reports an error condition, the
 *   Working_Configuration AND the Active_Configuration are left unchanged
 *    and the "configuration in effect is unchanged"
 *   diagnostic — plus every loader diagnostic — is surfaced.
 *
 *   The configuration controller (rhi/overlay/config_controller.{h,cpp}) calls
 *   the REAL load_config_file(path, active); it does not accept an injected
 *   ConfigLoadResult. To drive the success vs. failure branches deterministically
 *   this test therefore uses REAL files under a unique std::filesystem temp path:
 *
 *     • SUCCESS — a valid Config_File is written first with write_config_file()
 *       (asserted to emit zero diagnostics), then loaded through the controller.
 *       Working must equal the loaded result over every catalog setting, live
 *       applies must be recorded, and a reload must have been requested and run
 *       (the success rebuild stub advances Active to Working).
 *     • FAILURE — the load path points at a nonexistent file or a file holding
 *       non-JSON garbage, so load_config_file returns parsed_ok=false with
 *       config == base. Working and Active must be byte-for-byte unchanged over
 *       every catalog setting, the unchanged-note diagnostic must be present,
 *       and no success may be surfaced.
 *
 * The controller, the Settings Catalog, the Config_File module, and the fake
 * RendererEffects are all GPU-free (no Filament symbol; the only I/O is the
 * Config_File module's own save/load against the temp file), so this runs on the
 * host. The file self-registers with the RapidCheck harness in test_main.cpp via
 * heaven::testing::Register, so the file(GLOB) in benchmark/CMakeLists.txt picks
 * it up without editing the entry point. RapidCheck's default of 100 successful
 * iterations meets the "≥ 100 iterations" requirement (raise via
 * RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "bench_config_gen.h"               // heaven::testing::gen_bench_config
#include "config_controller_fakes.h"        // heaven::testing::FakeRendererEffects
#include "engine/config_file.h"             // load_config_file / write_config_file
#include "rhi/overlay/config_controller.h"  // apply_config_intents / ConfigOp
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
using heaven::ConfigLoadResult;
using heaven::ConfigOp;
using heaven::ConfigOpLog;
using heaven::ConfigUiState;
using heaven::OverlayIntents;
using heaven::testing::FakeRendererEffects;
using heaven::testing::gen_bench_config;

// A unique path under the system temp directory. Uniqueness comes from a
// process-wide atomic counter combined with the steady-clock tick, so iterated
// cases never collide.
[[nodiscard]] std::string unique_temp_path() {
    namespace fs = std::filesystem;
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long id = counter.fetch_add(1);
    const long long tick =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path =
        fs::temp_directory_path() /
        std::format("heaven_load_handling_{}_{}.json", tick, id);
    return path.string();
}

void remove_temp(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec); // best-effort cleanup; ignore failure
}

void write_raw(const std::string& path, const std::string& content) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    os << content;
    os.close();
}

// Replace any byte the JSON serializer would reject (non-ASCII / control) with
// '_', yielding a valid-UTF-8 ASCII string. The generator draws arbitrary bytes
// for quality_preset_label, but a real preset label is always plain text; this
// keeps the round-trip's only string field serializable without weakening the
// property (the label is still an arbitrary in-range string value).
[[nodiscard]] std::string sanitize_label(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x80) c = '_';
    }
    return out;
}

// True iff `a` and `b` agree on EVERY catalog-governed setting (the full
// editable surface, comparing through SettingValue's operator==).
[[nodiscard]] bool configs_equal_over_catalog(const BenchConfig& a,
                                              const BenchConfig& b) {
    return std::ranges::all_of(heaven::settings_catalog(), [&](const CatalogEntry& entry) {
        return entry.read(a) == entry.read(b);
    });
}

// ── SUCCESS: a valid file load copies into Working, applies Live, reloads ──
bool run_p12_load_success_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 12: load success copies into "
        "Working, applies Live, and reloads (Validates: Requirements 6.4, 6.5)",
        [] {
            // The base Active_Configuration the load applies onto, and a
            // separately randomized target written to disk.
            const BenchConfig active_before = gen_bench_config();
            BenchConfig       target         = gen_bench_config();
            // The only persisted string field; keep it serializable (see above).
            target.quality_preset_label = sanitize_label(target.quality_preset_label);

            const std::string path = unique_temp_path();

            // Write the target config; a clean serialization emits zero
            // diagnostics. (Out-of-range generated values serialize as-is and are
            // clamped on load — that produces non-fatal LOAD diagnostics, never
            // write diagnostics.)
            const std::vector<std::string> write_diags =
                heaven::write_config_file(path, target);
            RC_ASSERT(write_diags.empty());

            // The controller loads onto the current Active as the base, so the
            // ground-truth "loaded result" is an independent load against the
            // same pre-load Active. It is deterministic, so Working must match it
            // exactly afterwards.
            const ConfigLoadResult expected =
                heaven::load_config_file(path, active_before);
            RC_ASSERT(expected.parsed_ok);

            BenchConfig working = active_before;
            BenchConfig active  = active_before;

            ConfigUiState  ui;
            OverlayIntents intents;
            intents.load      = true;
            intents.load_path = path;

            FakeRendererEffects fx;
            fx.rebuild_succeeds = true;

            const ConfigOpLog log =
                heaven::apply_config_intents(working, active, ui, intents, fx);

            remove_temp(path);

            // (6.4) The loaded BenchConfig was copied into Working: it matches an
            // independent load of the same file onto the same base, over every
            // catalog setting.
            RC_ASSERT(configs_equal_over_catalog(working, expected.config));

            // (6.5) Live_Settings were applied to the renderer: both feature
            // toggles and non-feature live applies were dispatched.
            RC_ASSERT(!fx.feature_calls.empty());
            RC_ASSERT(!fx.live_applies.empty());

            // (6.6) A Benchmark_Reload was requested and run: the active run was
            // cancelled first, exactly one rebuild ran, and the log records it.
            RC_ASSERT(std::find(log.begin(), log.end(), ConfigOp::Load) !=
                      log.end());
            RC_ASSERT(std::find(log.begin(), log.end(), ConfigOp::Reload) !=
                      log.end());
            RC_ASSERT(fx.cancel_count == 1);
            RC_ASSERT(fx.rebuild_count == 1);

            // The successful reload (success stub) advanced Active to Working, so
            // the two agree over every catalog setting.
            RC_ASSERT(configs_equal_over_catalog(active, working));
        });
}

// ── FAILURE: an error load leaves Working AND Active unchanged (atomic) ──
bool run_p12_load_failure_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 12: load error is atomic and "
        "surfaces the unchanged note (Validates: Requirements 6.7, 8.5, 9.4)",
        [] {
            // The configuration in effect immediately before the load. Both
            // Working and Active start equal to it.
            const BenchConfig before = gen_bench_config();

            BenchConfig working = before;
            BenchConfig active  = before;

            // Two error shapes: a path that does not exist (unopenable) and a
            // real file holding non-JSON garbage (parse failure). Both yield
            // parsed_ok=false with config == base.
            const int kind = *rc::gen::inRange(0, 2);
            std::string path;
            if (kind == 0) {
                // Never created → load_config_file cannot open it.
                path = unique_temp_path();
            } else {
                path = unique_temp_path();
                write_raw(path, "not json{{ this is : garbage ]] ");
            }

            ConfigUiState ui;
            // Seed a stale success to prove the failed load clears it.
            ui.success        = true;
            ui.success_detail = "stale";

            OverlayIntents intents;
            intents.load      = true;
            intents.load_path = path;

            FakeRendererEffects fx;

            const ConfigOpLog log =
                heaven::apply_config_intents(working, active, ui, intents, fx);

            remove_temp(path);

            // (6.7 / 9.4) Working AND Active are unchanged over every catalog
            // setting — the failed load is atomic, mutating neither config.
            RC_ASSERT(configs_equal_over_catalog(working, before));
            RC_ASSERT(configs_equal_over_catalog(active, before));

            // No reload was requested and no renderer effect ran on the failure
            // path: nothing was cancelled or rebuilt.
            RC_ASSERT(std::find(log.begin(), log.end(), ConfigOp::Reload) ==
                      log.end());
            RC_ASSERT(fx.rebuild_count == 0);
            RC_ASSERT(fx.cancel_count == 0);

            // (8.5) The "configuration in effect is unchanged" diagnostic is
            // surfaced, alongside the loader's own diagnostic(s).
            RC_ASSERT(std::find(ui.diagnostics.begin(), ui.diagnostics.end(),
                                std::string("configuration in effect is unchanged")) !=
                      ui.diagnostics.end());
            RC_ASSERT(ui.diagnostics.size() >= 2u);

            // A failed load surfaces no success indication.
            RC_ASSERT(ui.success == false);
        });
}

const heaven::testing::Register g_register_load_success{
    &run_p12_load_success_property};
const heaven::testing::Register g_register_load_failure{
    &run_p12_load_failure_property};

} // namespace
