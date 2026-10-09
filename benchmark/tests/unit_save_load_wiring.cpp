// Feature: in-ui-configuration — example/unit tests for save/load/reload wiring
/*
 * HeavenBench — deterministic example/unit tests for the configuration
 * controller's save / load / reload wiring.
 *
 * Feature: in-ui-configuration
 *    — Validates:,
 *
 *
 * These are EXAMPLE/UNIT tests (fixed, deterministic scenarios), not property
 * tests: each drives heaven::apply_config_intents() with a hand-built
 * OverlayIntents and a recording FakeRendererEffects, then asserts the exact
 * resulting ConfigUiState / operation log / fake call counts. They complement
 * the property tests (prop_*.cpp, tasks 4.4–4.12, 5.1–5.2) by pinning concrete
 * branch behaviour the controller promises:
 *
 *   1. Save with a non-empty path → write_config_file runs; zero diagnostics →
 *      success set with success_detail == path.
 *   2. Save with an empty path → no I/O; diagnostics == {"a target path is
 *      required"}; no success.
 *   3. Save where write_config_file returns diagnostics (parent dir missing) →
 *      diagnostics non-empty; no success.
 *   4. Load reads onto the Active_Configuration as base: the loaded
 *      Working equals an independent load_config_file(path, active), and a
 *      non-persisted field (physical-camera aperture) keeps the Active base
 *      value rather than the written file's value.
 *   5. A successful load requests a reload  and the active run is
 *      cancelled before the rebuild: the op log records
 *      Load then Reload, cancel_count == 1, rebuild_count == 1.
 *   6. The reload control is surfaced when dirty: a Reload-classified
 *      edit makes Working diverge from Active, so is_dirty() / ui.dirty hold.
 *   7. An empty/failing load is handled atomically at the controller level
 *      (context): Working and Active are unchanged and diagnostics
 *      are surfaced. (The empty-path WITHHELD behaviour proper lives in
 *      OverlayUI, here we only assert the controller's atomic
 *      handling of an empty/failing load.)
 *   8. Diagnostics persistence and the no-diagnostics path: the
 *      controller only clears diagnostics on a NEW save/load, so an edit-only
 *      frame (and a dismiss-only frame) leaves prior diagnostics intact, while
 *      a clean save leaves diagnostics empty and sets success. (Dismissal of
 *      diagnostics is owned by main.cpp / OverlayUI,  — the controller
 *      has no dismiss handling, which scenario 8 documents.)
 *
 * The configuration controller (rhi/overlay/config_controller.{h,cpp}), the
 * Settings Catalog, the Config_File module, and the fake RendererEffects are all
 * GPU-free (no Filament symbol; the only I/O is the Config_File module's own
 * save/load against a temp file), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via
 * heaven::testing::Register, so the file(GLOB) in benchmark/CMakeLists.txt picks
 * it up without editing the entry point. These are deterministic unit tests, so
 * they use plain boolean check functions rather than rc::check.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "config_controller_fakes.h"        // heaven::testing::FakeRendererEffects
#include "engine/config_file.h"             // load_config_file / write_config_file
#include "rhi/overlay/config_controller.h"  // apply_config_intents / is_dirty / ConfigOp
#include "rhi/overlay/config_edit.h"        // OverlayIntents / ConfigUiState / SettingId
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
using heaven::SettingId;
using heaven::SettingValue;
using heaven::testing::FakeRendererEffects;

// ── Temp-file plumbing ──────────────────────────────────────────

// Unique path under the system temp dir. A static counter plus a steady-clock
// stamp keeps paths distinct within this process run and across re-runs.
std::string unique_temp_path() {
    namespace fs = std::filesystem;
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long id = counter.fetch_add(1);
    const long long tick =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path =
        fs::temp_directory_path() /
        ("heaven_save_load_wiring_" + std::to_string(tick) + "_" +
         std::to_string(id) + ".json");
    return path.string();
}

void remove_temp(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec); // best-effort cleanup
}

// Replace any byte the JSON serializer would reject (non-ASCII / control) with
// '_'. write_config_file throws on a non-UTF-8 string, so every String field is
// sanitized before a save, exactly as the sibling round-trip tests do.
std::string sanitize_ascii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x80) c = '_';
    }
    return out;
}

// A deterministic, in-range, serializable BenchConfig used as the known
// configuration the save/load scenarios write and read. Defaults are valid and
// ASCII; the label is sanitized defensively.
BenchConfig make_known_config() {
    BenchConfig cfg;                         // defaults are in-range / serializable
    cfg.quality_preset_label = sanitize_ascii(cfg.quality_preset_label);
    cfg.width                = 1920;
    cfg.height               = 1080;
    cfg.time_of_day          = 12.0f;
    cfg.weather.rain_intensity = 0.25f;
    return cfg;
}

// True iff `a` and `b` agree on EVERY catalog-governed setting (the full
// editable surface), comparing through SettingValue's operator==.
bool configs_equal_over_catalog(const BenchConfig& a, const BenchConfig& b) {
    return std::ranges::all_of(heaven::settings_catalog(), [&](const CatalogEntry& entry) {
        return entry.read(a) == entry.read(b);
    });
}

// Small per-scenario failure reporter mirroring the codebase's unit-test style.
struct Checker {
    const char* scenario;
    bool        ok = true;
    void expect(bool cond, const std::string& msg) {
        if (!cond) {
            std::printf("[unit_save_load_wiring/%s] FAILED: %s\n", scenario,
                        msg.c_str());
            ok = false;
        }
    }
};

// ── Scenario 1: save with a NON-EMPTY path → success  ──
bool scenario_save_non_empty_path() {
    Checker c{"save_non_empty_path"};

    const BenchConfig cfg = make_known_config();
    BenchConfig working = cfg;
    BenchConfig active  = cfg;

    const std::string path = unique_temp_path();

    ConfigUiState  ui;
    OverlayIntents intents;
    intents.save      = true;
    intents.save_path = path;

    FakeRendererEffects fx;
    const ConfigOpLog log =
        heaven::apply_config_intents(working, active, ui, intents, fx);

    const bool file_exists = std::filesystem::exists(path);
    remove_temp(path);

    // The save op ran and write_config_file actually produced the file (5.2).
    c.expect(std::find(log.begin(), log.end(), ConfigOp::Save) != log.end(),
             "operation log should record a Save");
    c.expect(file_exists, "write_config_file should have created the target file");
    // Zero diagnostics → success identifying the saved path (5.3).
    c.expect(ui.diagnostics.empty(), "a clean save should surface no diagnostics");
    c.expect(ui.success, "a clean save should set ui.success");
    c.expect(ui.success_detail == path,
             "ui.success_detail should equal the saved path");
    // A pure save runs no renderer effect.
    c.expect(fx.rebuild_count == 0 && fx.cancel_count == 0,
             "a save must not cancel a run or rebuild");

    return c.ok;
}

// ── Scenario 2: save with an EMPTY path → required-path note  ──
bool scenario_save_empty_path() {
    Checker c{"save_empty_path"};

    const BenchConfig cfg = make_known_config();
    BenchConfig working = cfg;
    BenchConfig active  = cfg;

    ConfigUiState  ui;
    OverlayIntents intents;
    intents.save      = true;
    intents.save_path = "";  // empty → no I/O

    FakeRendererEffects fx;
    const ConfigOpLog log =
        heaven::apply_config_intents(working, active, ui, intents, fx);

    // The save op is logged (the controller services the intent) but performs
    // no write; it surfaces exactly the required-path diagnostic and no success.
    c.expect(std::find(log.begin(), log.end(), ConfigOp::Save) != log.end(),
             "operation log should record a Save even for an empty path");
    c.expect(ui.diagnostics.size() == 1u &&
                 ui.diagnostics[0] == "a target path is required",
             "an empty save path should surface exactly the required-path note");
    c.expect(!ui.success, "an empty save path must not set success");

    return c.ok;
}

// ── Scenario 3: save where write_config_file fails → diagnostics  ──
bool scenario_save_write_diagnostic() {
    Checker c{"save_write_diagnostic"};

    const BenchConfig cfg = make_known_config();
    BenchConfig working = cfg;
    BenchConfig active  = cfg;

    // A target beneath a directory that does not exist makes the underlying
    // ofstream open fail deterministically (independent of permission bits), so
    // write_config_file returns a diagnostic naming the path.
    static std::atomic<unsigned> dir_counter{0};
    const std::filesystem::path missing_dir =
        std::filesystem::temp_directory_path() /
        ("heaven_no_such_dir_swl_" +
         std::to_string(dir_counter.fetch_add(1)));
    {
        std::error_code ec;
        std::filesystem::remove_all(missing_dir, ec);
    }
    const std::string path = (missing_dir / "out.json").string();

    ConfigUiState  ui;
    OverlayIntents intents;
    intents.save      = true;
    intents.save_path = path;

    FakeRendererEffects fx;
    heaven::apply_config_intents(working, active, ui, intents, fx);

    const bool created = std::filesystem::exists(path);
    {
        std::error_code ec;
        std::filesystem::remove_all(missing_dir, ec);
    }

    // The write failed: at least one diagnostic, no success, no file created.
    c.expect(!ui.diagnostics.empty(),
             "a failed write should surface at least one diagnostic");
    c.expect(!ui.success, "a failed write must not set success");
    c.expect(!created, "a failed write must not create the target file");

    return c.ok;
}

// ── Scenario 4: load reads onto the Active_Configuration as base  ──
bool scenario_load_uses_active_as_base() {
    Checker c{"load_uses_active_as_base"};

    // The config written to disk: a distinct, default aperture (16.0, not in
    // config_schema so it is never serialized).
    BenchConfig target = make_known_config();
    target.physical_camera.aperture = 16.0f;  // default; not persisted

    const std::string path = unique_temp_path();
    const std::vector<std::string> write_diags =
        heaven::write_config_file(path, target);
    c.expect(write_diags.empty(), "writing the known config should be clean");

    // The Active_Configuration the load applies onto. Give it a DISTINCT,
    // non-persisted aperture so we can prove the loaded result kept the Active
    // base value rather than the written file's value.
    BenchConfig active_before = make_known_config();
    active_before.physical_camera.aperture = 4.0f;  // distinct base value

    // The controller loads onto the current Active as the base, so the
    // ground-truth result is an independent load against the same Active.
    const ConfigLoadResult expected =
        heaven::load_config_file(path, active_before);
    c.expect(expected.parsed_ok, "the independent reference load should parse");

    BenchConfig working = active_before;
    BenchConfig active  = active_before;

    ConfigUiState  ui;
    OverlayIntents intents;
    intents.load      = true;
    intents.load_path = path;

    FakeRendererEffects fx;
    fx.rebuild_succeeds = true;
    heaven::apply_config_intents(working, active, ui, intents, fx);

    remove_temp(path);

    // (6.3) Working matches an independent load onto the same Active base over
    // every catalog setting.
    c.expect(configs_equal_over_catalog(working, expected.config),
             "loaded Working should equal load_config_file(path, active)");
    // (6.3, base usage) The non-persisted aperture kept the Active base value
    // (4.0), NOT the written file's value (16.0) — proving `active` was the base.
    c.expect(working.physical_camera.aperture == 4.0f,
             "a non-persisted field should retain the Active base value");
    c.expect(ui.current_config_name == path,
             "a successful load should retain the full path for subsequent saves");

    return c.ok;
}

// ── Scenario 5: successful load requests a reload, run cancelled first ──

bool scenario_load_requests_reload() {
    Checker c{"load_requests_reload"};

    const BenchConfig target = make_known_config();
    const std::string path = unique_temp_path();
    const std::vector<std::string> write_diags =
        heaven::write_config_file(path, target);
    c.expect(write_diags.empty(), "writing the known config should be clean");

    BenchConfig active_before = make_known_config();
    active_before.width = 1280;  // diverge so the loaded config differs

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

    // (6.6) A successful load logs a Load then requests a Reload.
    const auto load_it  = std::find(log.begin(), log.end(), ConfigOp::Load);
    const auto reload_it = std::find(log.begin(), log.end(), ConfigOp::Reload);
    c.expect(load_it != log.end(), "log should record the Load");
    c.expect(reload_it != log.end(),
             "a successful load should request a Benchmark_Reload");
    c.expect(load_it != log.end() && reload_it != log.end() &&
                 load_it < reload_it,
             "the Load must be logged before the Reload");
    // (3.4, 3.5) The active run was cancelled before the single rebuild.
    c.expect(fx.cancel_count == 1, "the active run should be cancelled once");
    c.expect(fx.rebuild_count == 1, "exactly one rebuild should run");

    return c.ok;
}

// ── Scenario 6: the reload control is surfaced when dirty  ──
bool scenario_dirty_when_reload_edit() {
    Checker c{"dirty_when_reload_edit"};

    BenchConfig active = make_known_config();  // width 1920
    BenchConfig working = active;

    // Width is a Reload_Setting: editing it diverges Working from Active without
    // touching Active, so the overlay should surface its Apply/Reload control.
    ConfigUiState  ui;
    OverlayIntents intents;
    intents.edits.push_back(heaven::ConfigurationEdit{SettingId::Width,
                                                      SettingValue{1280}});

    FakeRendererEffects fx;
    heaven::apply_config_intents(working, active, ui, intents, fx);

    // (4.6 keying state) The edit landed in Working only; Active is untouched,
    // so the configuration is dirty — the state the Apply/Reload control keys on.
    c.expect(working.width == 1280, "a Reload edit should update Working");
    c.expect(active.width == 1920,
             "a Reload edit must not touch Active before a reload");
    c.expect(ui.dirty, "ui.dirty should be set after a divergent Reload edit");
    c.expect(heaven::is_dirty(working, active),
             "is_dirty should agree the configs diverge in a Reload setting");
    // No reload was requested by a bare edit.
    c.expect(fx.rebuild_count == 0, "an edit alone must not trigger a rebuild");

    return c.ok;
}

// ── Scenario 7: an empty/failing load is handled atomically (ctx) ──
bool scenario_empty_load_atomic() {
    Checker c{"empty_load_atomic"};

    // NOTE: the empty-path WITHHELD behaviour proper (the overlay never raises
    // intents.load for an empty path) lives in OverlayUI. Here we
    // assert only that IF the controller is handed an empty/failing load it
    // handles it atomically — no change to either config, diagnostics surfaced.
    const BenchConfig before = make_known_config();
    BenchConfig working = before;
    BenchConfig active  = before;

    ConfigUiState ui;
    ui.success        = true;       // seed a stale success to prove it clears
    ui.success_detail = "stale";

    OverlayIntents intents;
    intents.load      = true;
    intents.load_path = "";  // empty → load_config_file("", active) fails

    FakeRendererEffects fx;
    const ConfigOpLog log =
        heaven::apply_config_intents(working, active, ui, intents, fx);

    // Working and Active are unchanged: the failed load is atomic.
    c.expect(configs_equal_over_catalog(working, before),
             "a failed load must leave Working unchanged");
    c.expect(configs_equal_over_catalog(active, before),
             "a failed load must leave Active unchanged");
    // No reload / cancel ran on the failure path.
    c.expect(std::find(log.begin(), log.end(), ConfigOp::Reload) == log.end(),
             "a failed load must not request a reload");
    c.expect(fx.rebuild_count == 0 && fx.cancel_count == 0,
             "a failed load must not cancel or rebuild");
    // The unchanged-note diagnostic is surfaced and the stale success cleared.
    c.expect(std::find(ui.diagnostics.begin(), ui.diagnostics.end(),
                       std::string("configuration in effect is unchanged")) !=
                 ui.diagnostics.end(),
             "a failed load should surface the unchanged note");
    c.expect(!ui.success, "a failed load must clear success");

    return c.ok;
}

// ── Scenario 8: diagnostics persistence + no-diagnostics path  ──
bool scenario_diagnostics_persistence() {
    Checker c{"diagnostics_persistence"};

    BenchConfig working = make_known_config();
    BenchConfig active  = working;
    ConfigUiState ui;

    // Frame 1 — an empty save populates diagnostics.
    {
        OverlayIntents intents;
        intents.save      = true;
        intents.save_path = "";
        FakeRendererEffects fx;
        heaven::apply_config_intents(working, active, ui, intents, fx);
    }
    c.expect(ui.diagnostics.size() == 1u &&
                 ui.diagnostics[0] == "a target path is required",
             "frame 1 should leave the required-path diagnostic");

    // Frame 2 — only edits, no save/load: the controller clears diagnostics ONLY
    // on a new save/load, so the prior diagnostic must remain intact (8.2).
    {
        OverlayIntents intents;
        intents.edits.push_back(heaven::ConfigurationEdit{
            SettingId::TimeOfDay, SettingValue{8.0f}});
        FakeRendererEffects fx;
        heaven::apply_config_intents(working, active, ui, intents, fx);
    }
    c.expect(ui.diagnostics.size() == 1u &&
                 ui.diagnostics[0] == "a target path is required",
             "an edit-only frame must not clear existing diagnostics");

    // Frame 3 — a dismiss-only frame: the controller has NO dismiss handling
    // (dismissal is owned by main.cpp / OverlayUI), so it leaves the
    // diagnostics intact too.
    {
        OverlayIntents intents;
        intents.dismiss_diagnostics = true;
        FakeRendererEffects fx;
        heaven::apply_config_intents(working, active, ui, intents, fx);
    }
    c.expect(ui.diagnostics.size() == 1u,
             "the controller must not itself clear diagnostics on dismiss");

    // Frame 4 — a clean save (valid path, zero write diagnostics): the new
    // operation clears the prior diagnostics and surfaces no diagnostic, setting
    // success instead (8.4).
    {
        const std::string path = unique_temp_path();
        OverlayIntents intents;
        intents.save      = true;
        intents.save_path = path;
        FakeRendererEffects fx;
        heaven::apply_config_intents(working, active, ui, intents, fx);
        const bool created = std::filesystem::exists(path);
        remove_temp(path);
        c.expect(created, "the clean save should create the file");
    }
    c.expect(ui.diagnostics.empty(),
             "a clean save should clear the prior diagnostics (8.4)");
    c.expect(ui.success, "a clean save with no diagnostics should set success");

    return c.ok;
}

// Run all scenarios, reporting each independently so one failure does not mask
// the others.
bool run_save_load_wiring_units() {
    bool ok = true;
    ok = scenario_save_non_empty_path()    && ok;
    ok = scenario_save_empty_path()        && ok;
    ok = scenario_save_write_diagnostic()  && ok;
    ok = scenario_load_uses_active_as_base() && ok;
    ok = scenario_load_requests_reload()   && ok;
    ok = scenario_dirty_when_reload_edit() && ok;
    ok = scenario_empty_load_atomic()      && ok;
    ok = scenario_diagnostics_persistence() && ok;
    return ok;
}

const heaven::testing::Register g_register_save_load_wiring{
    &run_save_load_wiring_units};

} // namespace
