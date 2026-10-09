// Feature: benchmark-config-file,: Effective configuration follows
//                                              CLI > file > default precedence
//     (Validates:)
/*
 * HeavenBench — Property test for the CLI > file > default precedence of
 * the Effective_Configuration.
 *
 * Feature: benchmark-config-file
 *   •: Effective configuration follows CLI > file > default
 *       precedence
 *
 *
 * This test reproduces the exact startup pipeline main.cpp's resolve_config
 * uses (not yet written) — but built ONLY from the public,
 * GPU-free functions, never main.cpp:
 *
 *   1. Start from a default BenchConfig.
 *   2. If a file is named, overlay the file onto a default:
 *        cfg = load_config_file(path, BenchConfig{}).config   (file-over-default)
 *   3. Parse the command line and overlay it:
 *        ParseResult r = parse_cli(argv);
 *        apply_cli_overlay(cfg, r);                            (CLI-over-file)
 *
 * For a representative Setting in each FieldType / validation category the test
 * randomly decides whether the file names it (and to what value) and whether a
 * CLI flag sets it (and to what value), runs the pipeline, then asserts the
 * resulting field equals:
 *
 *     • the CLI-applied value          when the flag is present
 *     • else the applied file value    when the file names it
 *     • else the default value         when neither sets it
 *
 * The EXPECTED applied value is computed INDEPENDENTLY here, routing validated
 * Settings through the SAME shared engine/types.h helper the loader and the CLI
 * flag both use (parse_rain_intensity, apply_msaa_samples), so the test pins
 * precedence structurally without copying the pipeline's logic. Values are
 * chosen VALID-but-distinguishable (default != file != cli) so each precedence
 * branch is genuinely exercised.
 *
 * A dedicated second property targets  directly: a file value
 * that gets CLAMPED, with no CLI flag, must yield the clamped file value as the
 * Effective_Configuration.
 *
 * Representative Settings:
 *   • width                 — Int,   no validation        (--width W)
 *   • fullscreen            — Bool,  toggle (sets true)    (--fullscreen)
 *   • data_dir              — String,no validation         (--data DIR)
 *   • quality.msaa_samples  — Int,   fixed-set reject {1,2,4} (--msaa-quality N)
 *   • weather.rain_intensity— Float, clamp [0,1]           (--rain-intensity V)
 *
 * fullscreen is a Boolean_Toggle: a present flag can only set it true (there is
 * no CLI form that sets it false), so the CLI-present branch expects `true`.
 *
 * load_config_file / parse_cli / apply_cli_overlay are pure with respect to the
 * GPU (file I/O only, no Filament/SDL/GPU), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" bar (raise via RC_PARAMS=max_success=…).
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidcheck.h>

#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/types.h"

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
using heaven::ParseResult;

// ── Temp-file plumbing ──────────────────────────────────────────

// Write `content` to a unique path under the system temp directory and return
// it. Uniqueness comes from a process-wide atomic counter combined with the
// steady-clock tick, so iterated cases never collide. The file is removed once
// the load completes.
std::string write_temp_json(const std::string& content) {
    namespace fs = std::filesystem;
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long id = counter.fetch_add(1);
    const long long tick =
        std::chrono::steady_clock::now().time_since_epoch().count();

    const fs::path path =
        fs::temp_directory_path() /
        std::format("heaven_cfg_prec_{}_{}.json", tick, id);

    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    os << content;
    os.close();
    return path.string();
}

void remove_temp(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec); // best-effort cleanup; ignore failure
}

// ── The startup precedence pipeline (public functions only) ─────

// Mirrors main.cpp's resolve_config: optionally overlay a named Config_File
// onto a default BenchConfig (file-over-default), then overlay the parsed
// command line (CLI-over-file). When `file_json` is absent NO file is named, so
// cfg stays at its defaults — exactly as when `--config` is not given.
BenchConfig run_pipeline(const std::optional<std::string>& file_json,
                         const std::vector<std::string>& cli_tokens) {
    BenchConfig cfg; // defaults

    if (file_json) {
        const std::string path = write_temp_json(*file_json);
        cfg = heaven::load_config_file(path, BenchConfig{}).config;
        remove_temp(path);
    }

    std::vector<const char*> argv;
    argv.reserve(cli_tokens.size() + 1);
    argv.push_back("heaven_benchmark");
    for (const std::string& t : cli_tokens) argv.push_back(t.c_str());
    const ParseResult r =
        heaven::parse_cli(argv);

    heaven::apply_cli_overlay(cfg, r);
    return cfg;
}

// ── JSON value text builders (no escaping needed) ───────────────
std::string json_int(int v)    { return std::format("{}", v); }
std::string json_double(double v) { return std::format("{}", v); }
std::string json_bool(bool b)  { return b ? "true" : "false"; }
std::string json_string(const std::string& s) { return std::format("\"{}\"", s); }

// A one-key JSON object naming `key` with the already-rendered `value_text`.
std::string one_key(std::string_view key, const std::string& value_text) {
    return std::format("{{{}: {}}}", json_string(std::string(key)), value_text);
}

// Representative Setting keys + their CLI flags.
constexpr std::string_view k_width_key = "width";                 // Int, no validation
constexpr std::string_view k_data_key  = "data_dir";              // String, no validation
constexpr std::string_view k_msaa_key  = "quality.msaa_samples";  // Int, fixed-set reject
constexpr std::string_view k_rain_key  = "weather.rain_intensity";// Float, clamp [0,1]

// ──: CLI > file > default precedence ─────────────────
bool run_p7_precedence() {
    return rc::check(
        "Feature: benchmark-config-file, Property 7: effective configuration "
        "follows CLI > file > default precedence (Validates: Requirements 4.1, "
        "4.2, 4.3, 4.4)",
        [] {
            const int  kind         = *rc::gen::inRange(0, 5);
            const bool file_present = *rc::gen::arbitrary<bool>();
            const bool cli_present  = *rc::gen::arbitrary<bool>();

            switch (kind) {
                case 0: { // width — Int, no validation. default 1920.
                    // Distinguishable: file in [100,1900], cli in [2001,5000].
                    const int file_value = *rc::gen::inRange(100, 1901);
                    const int cli_value  = *rc::gen::inRange(2001, 5001);

                    const std::optional<std::string> file_json =
                        file_present
                            ? std::optional<std::string>(
                                  one_key(k_width_key, json_int(file_value)))
                            : std::nullopt;
                    std::vector<std::string> cli;
                    if (cli_present) { cli = {"--width", json_int(cli_value)}; }

                    const BenchConfig cfg = run_pipeline(file_json, cli);

                    const int expected =
                        cli_present  ? cli_value
                        : file_present ? file_value
                                       : 1920;
                    RC_ASSERT(cfg.width == expected);
                    break;
                }
                case 1: { // fullscreen — Bool toggle (sets true). default false.
                    const bool file_value = *rc::gen::arbitrary<bool>();

                    const std::optional<std::string> file_json =
                        file_present
                            ? std::optional<std::string>(one_key(
                                  "fullscreen", json_bool(file_value)))
                            : std::nullopt;
                    std::vector<std::string> cli;
                    if (cli_present) { cli = {"--fullscreen"}; }

                    const BenchConfig cfg = run_pipeline(file_json, cli);

                    // A present --fullscreen flag can only set the field true.
                    const bool expected =
                        cli_present  ? true
                        : file_present ? file_value
                                       : false;
                    RC_ASSERT(cfg.fullscreen == expected);
                    break;
                }
                case 2: { // data_dir — String, no validation. default "assets".
                    // Distinct prefixes keep file/cli/default all different.
                    const std::string suffix = *rc::gen::container<std::string>(
                        rc::gen::elementOf(std::string(
                            "abcdefghijklmnopqrstuvwxyz0123456789_-./")));
                    const std::string file_value = "file_" + suffix;
                    const std::string cli_value  = "cli_" + suffix;

                    const std::optional<std::string> file_json =
                        file_present
                            ? std::optional<std::string>(
                                  one_key(k_data_key, json_string(file_value)))
                            : std::nullopt;
                    std::vector<std::string> cli;
                    if (cli_present) { cli = {"--data", cli_value}; }

                    const BenchConfig cfg = run_pipeline(file_json, cli);

                    const std::string expected =
                        cli_present  ? cli_value
                        : file_present ? file_value
                                       : std::string("assets");
                    RC_ASSERT(cfg.data_dir == expected);
                    break;
                }
                case 3: { // quality.msaa_samples — fixed-set {1,2,4}. default 2.
                    // Use distinct valid samples so the file and CLI differ.
                    const bool swap = *rc::gen::arbitrary<bool>();
                    const int file_value = swap ? 4 : 2;
                    const int cli_value  = swap ? 2 : 4;

                    const std::optional<std::string> file_json =
                        file_present
                            ? std::optional<std::string>(
                                  one_key(k_msaa_key, json_int(file_value)))
                            : std::nullopt;
                    std::vector<std::string> cli;
                    if (cli_present) { cli = {"--msaa-quality", json_int(cli_value)}; }

                    const BenchConfig cfg = run_pipeline(file_json, cli);

                    // Valid samples route through apply_msaa_samples unchanged;
                    // compute the expectation through the shared helper anyway.
                    const int expected =
                        cli_present
                            ? heaven::apply_msaa_samples(cli_value, 2).value
                        : file_present
                            ? heaven::apply_msaa_samples(file_value, 2).value
                            : 2;
                    RC_ASSERT(cfg.quality.msaa_samples == expected);
                    break;
                }
                default: { // weather.rain_intensity — clamp [0,1]. default 0.5.
                    // In-range, distinguishable values: file in [0,0.48],
                    // cli in [0.51,1.0]; both != default 0.5.
                    const int file_n = *rc::gen::inRange(0, 49);   // 0.00..0.48
                    const int cli_n  = *rc::gen::inRange(51, 101); // 0.51..1.00
                    const std::string file_text = json_double(file_n / 100.0);
                    const std::string cli_text   = json_double(cli_n / 100.0);

                    const std::optional<std::string> file_json =
                        file_present
                            ? std::optional<std::string>(
                                  one_key(k_rain_key, file_text))
                            : std::nullopt;
                    std::vector<std::string> cli;
                    if (cli_present) { cli = {"--rain-intensity", cli_text}; }

                    const BenchConfig cfg = run_pipeline(file_json, cli);

                    // Both the file loader and the CLI flag route the value
                    // through parse_rain_intensity; mirror that for the
                    // expectation so the comparison is bit-exact.
                    const float expected =
                        cli_present
                            ? heaven::parse_rain_intensity(cli_text.c_str()).value
                        : file_present
                            ? heaven::parse_rain_intensity(file_text.c_str()).value
                            : 0.5f;
                    RC_ASSERT(cfg.weather.rain_intensity == expected);
                    break;
                }
            }
        });
}

// ──: clamped file value, no CLI flag ───────

// When the file names a Setting with an out-of-range value and NO CLI flag sets
// it, the Effective_Configuration must equal the CLAMPED file value (not the
// raw file value, and not the default). rain_intensity is the representative
// clamped Setting; a value above 1 clamps to 1.0.
bool run_p7_clamped_file_value_when_no_cli() {
    return rc::check(
        "Feature: benchmark-config-file, Property 7 (R4.4): a clamped file "
        "value is used as the effective value when no CLI flag is present "
        "(Validates: Requirements 4.4)",
        [] {
            // Above-range file values (1.01.. 3.00) all clamp to 1.0; this
            // guarantees clamped_value != raw_file_value and != default 0.5.
            const int file_n = *rc::gen::inRange(101, 301);
            const std::string file_text = json_double(file_n / 100.0);

            // The file names the Setting; the command line does NOT.
            const std::optional<std::string> file_json =
                one_key(k_rain_key, file_text);
            const BenchConfig cfg = run_pipeline(file_json, {});

            const float clamped =
                heaven::parse_rain_intensity(file_text.c_str()).value;

            // The clamp genuinely moved the value (sanity on the generator).
            RC_ASSERT(clamped == 1.0f);
            // Effective value is the clamped file value.
            RC_ASSERT(cfg.weather.rain_intensity == clamped);
        });
}

bool run_config_precedence_properties() {
    bool ok = true;
    ok = run_p7_precedence() && ok;
    ok = run_p7_clamped_file_value_when_no_cli() && ok;
    return ok;
}

const heaven::testing::Register g_register_config_precedence{
    &run_config_precedence_properties};

} // namespace
