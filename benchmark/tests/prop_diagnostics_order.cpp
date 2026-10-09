// Feature: in-ui-configuration,: Diagnostics are surfaced in order and replaced per operation
/*
 * HeavenBench — Property test for configuration-diagnostics ordering and
 * per-operation replacement.
 *
 * Feature: in-ui-configuration
 *   •: Diagnostics are surfaced in order and replaced per operation
 *
 *
 * (diagnostics in order, replaced per operation):
 *   8.1 (order): the diagnostics a save or load operation surfaces into
 *       ConfigUiState::diagnostics appear in EXACTLY the order the underlying
 *       Config_File module reported them (for a failed load, the controller's
 *       trailing "configuration in effect is unchanged" note is appended LAST,
 *       after every module diagnostic).
 *   8.3 (replacement): each new save or load operation REPLACES the prior
 *       operation's diagnostics (and success state) rather than appending to
 *       them, so messages from a previous operation never leak into a new one.
 *
 *   The controller (rhi/overlay/config_controller.cpp) is the seam under test:
 *   it clears diagnostics/success at the start of each save/load and then
 *   populates them. This property drives the REAL Config_File module through
 *   temp files (so the loader/writer's own diagnostic order is the ground
 *   truth) and asserts the controller surfaces them faithfully.
 *
 * Strategy — each iteration picks one of four scenarios:
 *   • LoadOrderSuccess  — a well-formed JSON file with several unknown keys
 *     yields ≥2 loader diagnostics with parsed_ok=true; assert the controller's
 *     ui.diagnostics equals load_config_file()'s own diagnostics vector exactly
 *     (same entries, same order). (8.1)
 *   • LoadOrderFailure  — a malformed / non-object / empty file yields one
 *     loader diagnostic with parsed_ok=false; assert ui.diagnostics equals the
 *     loader's diagnostics followed by the trailing unchanged-note as the LAST
 *     entry. (8.1)
 *   • ReplaceEmptySave  — seed stale diagnostics + success, run an empty-path
 *     save; assert the stale entries are GONE and ui == {"a target path is
 *     required"}, success=false. Then run a SECOND operation and assert it
 *     replaced the first's result again. (8.3)
 *   • ReplaceCleanSave  — seed stale diagnostics + success=false, run a
 *     successful save to a writable temp path; assert the stale entries are
 *     GONE, ui.diagnostics is empty and success=true. Then run a SECOND
 *     (empty-path) save and assert it again replaced the prior result. (8.3)
 *
 * The configuration controller and the Settings Catalog are GPU-free, and the
 * Config_File module is likewise Filament-free, so this runs on the host. The
 * file self-registers with the RapidCheck harness in test_main.cpp via
 * heaven::testing::Register, so the file(GLOB) in benchmark/CMakeLists.txt picks
 * it up without editing the entry point. RapidCheck's default of 100 successful
 * iterations meets the "≥ 100 iterations" requirement (raise via
 * RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "engine/config_file.h"
#include "rhi/overlay/config_controller.h"

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
using heaven::ConfigLoadResult;
using heaven::ConfigUiState;
using heaven::OverlayIntents;
using heaven::testing::FakeRendererEffects;
using heaven::testing::gen_bench_config;

// The controller's trailing note appended after a failed load,
// which  pins as the LAST surfaced entry on a load failure.
constexpr const char* kUnchangedNote = "configuration in effect is unchanged";

// A small RAII temp file: writes `content` to a unique path under the system
// temp directory on construction and removes it on destruction, so the property
// exercises the REAL Config_File module without leaving artifacts behind.
struct TempFile {
    std::filesystem::path path;

    explicit TempFile(const std::string& content) {
        // A per-instance unique name from a RapidCheck-drawn token plus a
        // monotonically increasing counter, so concurrent iterations never
        // collide on the same path.
        static std::uint64_t counter = 0;
        const std::uint64_t  n       = ++counter;
        const auto tag = *rc::gen::inRange<std::uint64_t>(0, 1'000'000'000ULL);
        path = std::filesystem::temp_directory_path() /
               (std::string("ugx_diag_") + std::to_string(tag) + "_" +
                std::to_string(n) + ".json");

        std::ofstream os(path, std::ios::binary | std::ios::trunc);
        os << content;
    }

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    [[nodiscard]] std::string str() const { return path.string(); }
};

// Build a writable temp path WITHOUT creating the file, for the successful-save
// scenario. The destructor cleans up whatever the writer created.
struct TempPath {
    std::filesystem::path path;

    TempPath() {
        static std::uint64_t counter = 0;
        const std::uint64_t  n       = ++counter;
        const auto tag = *rc::gen::inRange<std::uint64_t>(0, 1'000'000'000ULL);
        path = std::filesystem::temp_directory_path() /
               (std::string("ugx_save_") + std::to_string(tag) + "_" +
                std::to_string(n) + ".json");
    }

    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    [[nodiscard]] std::string str() const { return path.string(); }
};

// Replace the schema's three String-typed fields (data_dir, node_file,
// quality_preset_label) with safe ASCII so the writer's nlohmann dump() cannot
// throw on a non-UTF-8 byte sequence. gen_bench_config() draws arbitrary
// std::strings (which may contain invalid UTF-8); that is irrelevant to the
// diagnostics ordering/replacement behavior  exercises, so we
// normalize the string fields before any successful save.
void sanitize_strings(BenchConfig& cfg) {
    cfg.data_dir             = "data";
    cfg.node_file            = "scene.node";
    cfg.quality_preset_label = "preset";
}

// A well-formed JSON object body carrying `count` (≥2) unknown keys, each of
// which the loader reports as an ignored-unknown-key diagnostic. Unknown keys
// are used (rather than clamp values) because they deterministically yield one
// diagnostic apiece regardless of the BenchConfig contents, so the scenario
// reliably produces ≥2 ordered loader diagnostics with parsed_ok=true.
std::string unknown_keys_json(int count) {
    std::string body = "{\n";
    for (int i = 0; i < count; ++i) {
        // Distinct, schema-absent keys. The loader's diagnostic order follows
        // the parser's key iteration order, which the test does NOT predict —
        // it compares against load_config_file()'s own vector below.
        body += "  \"zz_unknown_key_";
        body += std::to_string(i);
        body += "\": ";
        body += std::to_string(i);
        if (i + 1 < count) body += ",";
        body += "\n";
    }
    body += "}\n";
    return body;
}

// Run a single load intent through the controller and return the resulting UI
// state. `base` is the Active_Configuration the load applies onto.
ConfigUiState run_load(const std::string& path, const BenchConfig& base) {
    BenchConfig    working = base;
    BenchConfig    active  = base;
    ConfigUiState  ui;
    FakeRendererEffects fx;

    OverlayIntents intents;
    intents.load      = true;
    intents.load_path = path;

    heaven::apply_config_intents(working, active, ui, intents, fx);
    return ui;
}

// Run a single save intent through the controller, starting from a UI state
// pre-seeded with `stale` diagnostics and `seed_success`, and return the
// resulting UI state.
ConfigUiState run_save(const std::string&             path,
                       const BenchConfig&                    cfg,
                       std::vector<std::string>       stale,
                       bool                           seed_success) {
    BenchConfig    working = cfg;
    BenchConfig    active  = cfg;
    ConfigUiState  ui;
    ui.diagnostics    = std::move(stale);
    ui.success        = seed_success;
    ui.success_detail = "previous-operation-detail";
    FakeRendererEffects fx;

    OverlayIntents intents;
    intents.save      = true;
    intents.save_path = path;

    heaven::apply_config_intents(working, active, ui, intents, fx);
    return ui;
}

bool run_p13_diagnostics_order_property() {
    return rc::check(
        "Feature: in-ui-configuration, Property 13: diagnostics are surfaced in "
        "order and replaced per operation (Validates: Requirements 8.1, 8.3)",
        [] {
            const int scenario = *rc::gen::inRange(0, 4);
            const BenchConfig base = gen_bench_config();

            switch (scenario) {
                // ── 8.1: ordering on a SUCCESSFUL load with ≥2 diagnostics ──
                case 0: {
                    const int count = *rc::gen::inRange(2, 7);
                    TempFile  file(unknown_keys_json(count));

                    // Ground truth: the loader's own diagnostic vector and order.
                    const ConfigLoadResult ref =
                        heaven::load_config_file(file.str(), base);
                    RC_ASSERT(ref.parsed_ok);
                    RC_ASSERT(ref.diagnostics.size() ==
                              static_cast<std::size_t>(count));

                    const ConfigUiState ui = run_load(file.str(), base);

                    // The controller surfaces the loader's diagnostics verbatim,
                    // in the same order, with no trailing note on success.
                    RC_ASSERT(ui.diagnostics == ref.diagnostics);
                    RC_ASSERT(ui.success == false);
                    break;
                }

                // ── 8.1: ordering on a FAILED load (note appended LAST) ──
                case 1: {
                    // Three malformed shapes, all parse failures: a syntax error,
                    // a non-object document, and an empty file.
                    const int kind = *rc::gen::inRange(0, 3);
                    std::string content;
                    if (kind == 0) {
                        content = "{ this is not valid json ";
                    } else if (kind == 1) {
                        content = "[1, 2, 3]";
                    } else {
                        content = "   \n  \t ";
                    }
                    TempFile file(content);

                    const ConfigLoadResult ref =
                        heaven::load_config_file(file.str(), base);
                    RC_ASSERT(!ref.parsed_ok);
                    RC_ASSERT(!ref.diagnostics.empty());

                    const ConfigUiState ui = run_load(file.str(), base);

                    // ui.diagnostics == loader diagnostics (in order) + the
                    // trailing unchanged-note as the final entry (8.1, 8.5).
                    std::vector<std::string> expected = ref.diagnostics;
                    expected.emplace_back(kUnchangedNote);
                    RC_ASSERT(ui.diagnostics == expected);

                    // The note is genuinely LAST and the module diagnostics keep
                    // their order ahead of it.
                    RC_ASSERT(ui.diagnostics.back() == kUnchangedNote);
                    for (std::size_t i = 0; i < ref.diagnostics.size(); ++i) {
                        RC_ASSERT(ui.diagnostics[i] == ref.diagnostics[i]);
                    }
                    RC_ASSERT(ui.success == false);
                    break;
                }

                // ── 8.3: replacement by an EMPTY-PATH save ──
                case 2: {
                    // Random, non-trivial stale diagnostics from a "prior op".
                    const int stale_n = *rc::gen::inRange(1, 5);
                    std::vector<std::string> stale;
                    stale.reserve(stale_n);
for (int i = 0; i < stale_n; ++i) {
                        stale.push_back("stale-diagnostic-" + std::to_string(i));
                    }
                    const std::vector<std::string> stale_copy = stale;

                    // Empty save path → single required-path diagnostic, no I/O.
                    const ConfigUiState ui =
                        run_save(/*path=*/"", base, stale, /*seed_success=*/true);

                    // The prior operation's diagnostics were REPLACED, not
                    // appended to: none of the stale entries survive.
                    for (const std::string& s : stale_copy) {
                        RC_ASSERT(std::find(ui.diagnostics.begin(),
                                            ui.diagnostics.end(),
                                            s) == ui.diagnostics.end());
                    }
                    RC_ASSERT(ui.diagnostics ==
                              std::vector<std::string>{"a target path is required"});
                    RC_ASSERT(ui.success == false);

                    // A SECOND operation again REPLACES the first's result. Feed
                    // the just-produced diagnostics back in as the new stale set
                    // and run another empty-path save.
                    const ConfigUiState ui2 =
                        run_save(/*path=*/"", base, ui.diagnostics,
                                 /*seed_success=*/false);
                    RC_ASSERT(ui2.diagnostics ==
                              std::vector<std::string>{"a target path is required"});
                    RC_ASSERT(ui2.success == false);
                    break;
                }

                // ── 8.3: replacement by a CLEAN (successful) save ──
                default: {
                    const int stale_n = *rc::gen::inRange(1, 5);
                    std::vector<std::string> stale;
                    stale.reserve(stale_n);
for (int i = 0; i < stale_n; ++i) {
                        stale.push_back("stale-diagnostic-" + std::to_string(i));
                    }
                    const std::vector<std::string> stale_copy = stale;

                    TempPath out;
                    BenchConfig save_cfg = base;
                    sanitize_strings(save_cfg);
                    const ConfigUiState ui =
                        run_save(out.str(), save_cfg, stale, /*seed_success=*/false);

                    // A successful write replaces the stale diagnostics with an
                    // empty list and sets success.
                    for (const std::string& s : stale_copy) {
                        RC_ASSERT(std::find(ui.diagnostics.begin(),
                                            ui.diagnostics.end(),
                                            s) == ui.diagnostics.end());
                    }
                    RC_ASSERT(ui.diagnostics.empty());
                    RC_ASSERT(ui.success == true);
                    RC_ASSERT(ui.success_detail == out.str());

                    // A SECOND operation (empty-path save) replaces the success
                    // with the required-path diagnostic and clears success.
                    const ConfigUiState ui2 =
                        run_save(/*path=*/"", base, ui.diagnostics,
                                 /*seed_success=*/ui.success);
                    RC_ASSERT(ui2.diagnostics ==
                              std::vector<std::string>{"a target path is required"});
                    RC_ASSERT(ui2.success == false);
                    break;
                }
            }
        });
}

const heaven::testing::Register g_register_diagnostics_order{
    &run_p13_diagnostics_order_property};

} // namespace
