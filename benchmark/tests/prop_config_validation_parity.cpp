// Feature: benchmark-config-file,: Validation parity between the
//                                              file and the command line
/*
 * HeavenBench — Property test for validation parity between the
 * Config_File and the command line.
 *
 * Feature: benchmark-config-file
 *   •: Validation parity between the file and the command line
 *
 *
 * For every VALIDATED Setting the implementation routes both the Config_File loader and
 * the corresponding CLI flag through the SAME shared engine/types.h clamp/reject
 * helper, so a value must validate identically regardless of source. This
 * property drives the SAME raw value through both entry points and asserts:
 *   (a) the resulting BenchConfig field is byte-for-byte equal, AND
 *   (b) the clamp/reject OUTCOME matches: either both emit no clamp/reject
 *       diagnostic, or both emit one and the file's diagnostic equals the CLI's
 *       after substituting the schema key for the CLI flag name.
 *
 * Making the two inputs comparable.
 *   The CLI receives a STRING token; the file receives a JSON number. The loader
 *   re-renders the parsed JSON number back to a token before validating /
 *   diagnosing:
 *     • quality levels + shadow_light_budget + msaa  → integer  std::format("{}", raw)
 *     • foliage_density                              → float    std::format("{}", raw)
 *                                                       (raw = static_cast<float>(json double))
 *     • rain_intensity                               → double   std::format("{}", raw)
 *                                                       (raw = the json double itself)
 *     • dry_down_seconds                             → no diagnostic; float field only
 *   So for each case we generate the value, format it to a CANONICAL token with
 *   the SAME std::format the loader uses for that key's category, feed that exact
 *   token to the CLI, and write the SAME token (a valid JSON number) into the
 *   file. Because the token is the shortest round-tripping representation, the
 *   loader's re-render reproduces the identical token — so the file and CLI
 *   diagnostics differ ONLY by the key↔flag name, which (b) substitutes away.
 *
 * Both paths start from a default-constructed BenchConfig (parse_cli always does;
 * load_config_file is given BenchConfig{} as its base), so the MSAA reject path
 * retains the SAME prior sample count on both sides.
 *
 * Generated values span below-range, in-range (including the bounds),
 * above-range, and — for MSAA — out-of-set, exercising every clamp/reject branch.
 *
 * load_config_file is pure with respect to the GPU (file I/O only, no
 * Filament/SDL/GPU), and parse_cli is pure, so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/CMakeLists.txt
 * picks it up without editing the entry point. RapidCheck's default of 100
 * successful iterations meets the "≥ 100 iterations" bar (raise via
 * RC_PARAMS=max_success=…).
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
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
using heaven::ConfigLoadResult;
using heaven::ParseResult;

// ── Temp-file plumbing (matches prop_config_apply.cpp) ──────────

// Write `content` to a unique path under the system temp directory and return
// it. Uniqueness comes from a process-wide atomic counter combined with the
// steady-clock tick, so concurrent/iterated cases never collide. Callers remove
// the file once the load completes.
std::string write_temp_json(const std::string& content) {
    namespace fs = std::filesystem;
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long id = counter.fetch_add(1);
    const long long tick =
        std::chrono::steady_clock::now().time_since_epoch().count();

    const fs::path path =
        fs::temp_directory_path() /
        std::format("heaven_cfg_parity_{}_{}.json", tick, id);

    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    os << content;
    os.close();
    return path.string();
}

void remove_temp(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec); // best-effort cleanup; ignore failure
}

// Load a one-key JSON object {key: token} onto `base` via a temp file. `token`
// is already a valid JSON number literal (the canonical numeric token).
ConfigLoadResult load_one_key(std::string_view key, const std::string& token,
                              const BenchConfig& base) {
    const std::string json = std::format("{{\"{}\": {}}}", key, token);
    const std::string path = write_temp_json(json);
    ConfigLoadResult r = heaven::load_config_file(path, base);
    remove_temp(path);
    return r;
}

// Run the real parse driver over {prog, flag, token}.
ParseResult run_parse(const std::string& flag, const std::string& token) {
    const char* const argv[] = {"heaven_benchmark", flag.c_str(), token.c_str()};
    return heaven::parse_cli(argv);
}

// Replace the first occurrence of `from` with `to` (used to turn the file's
// schema-key diagnostic into the CLI's flag-name diagnostic).
std::string replace_first(std::string s, const std::string& from, const std::string& to) {
    const auto pos = s.find(from);
    if (pos != std::string::npos) s.replace(pos, from.size(), to);
    return s;
}

// Diagnostic OUTCOME parity (b): both sides emit the same number of clamp/reject
// diagnostics (0 or 1 for a single validated key), and when one is emitted the
// file's text equals the CLI's after the key→flag substitution.
bool diagnostics_match(std::string_view schema_key, std::string_view cli_flag,
                       const std::vector<std::string>& file_d,
                       const std::vector<std::string>& cli_d) {
    if (file_d.size() != cli_d.size()) return false;
    if (file_d.size() > 1u) return false; // a single key yields at most one diagnostic
    if (file_d.empty()) return true;
    return replace_first(file_d[0], std::string(schema_key), std::string(cli_flag)) == cli_d[0];
}

// Canonical numeric token renderers, each matching the std::format category the
// loader uses for that key (see the file header). The SAME string is fed to the
// CLI and written as the JSON number.
std::string token_from_int(int v)       { return std::format("{}", v); }
std::string token_from_float(float v)   { return std::format("{}", v); }
std::string token_from_double(double v) { return std::format("{}", v); }

bool run_p6_validation_parity() {
    return rc::check(
        "Feature: benchmark-config-file, Property 6: validation parity between "
        "the file and the command line (Validates: Requirements 5.1, 5.2, 5.3, "
        "5.4, 5.5, 5.6)",
        [] {
            // Both paths start from defaults so the MSAA reject path retains the
            // same prior count on each side.
            const BenchConfig base;

            // Pick one validated Setting. 0..3 quality levels, 4 budget,
            // 5 foliage, 6 msaa, 7 rain, 8 dry-down.
            const int kind = *rc::gen::inRange(0, 9);

            switch (kind) {
                case 0:
                case 1:
                case 2:
                case 3: {
                    // ── Quality levels (clamp_quality_level, [0,2]) ──
                    // Span below/in/above range, hitting the 0 and 2 bounds.
                    struct LevelKey {
                        std::string_view schema;
                        std::string_view flag;
                        std::function<heaven::QualityLevel(const BenchConfig&)> get;
                    };
                    static const LevelKey levels[] = {
                        {"quality.soft_shadow_level", "--soft-shadow-quality",
                         [](const BenchConfig& c) { return c.quality.soft_shadow_level; }},
                        {"quality.contact_shadow_level", "--contact-shadow-quality",
                         [](const BenchConfig& c) { return c.quality.contact_shadow_level; }},
                        {"quality.cloud_quality", "--cloud-quality",
                         [](const BenchConfig& c) { return c.quality.cloud_quality; }},
                        {"quality.ssgi_quality", "--ssgi-quality",
                         [](const BenchConfig& c) { return c.quality.ssgi_quality; }},
                    };
                    const LevelKey& k = levels[kind];

                    const int raw = *rc::gen::inRange(-3, 7); // covers <0, {0,1,2}, >2
                    const std::string token = token_from_int(raw);

                    const ConfigLoadResult fr = load_one_key(k.schema, token, base);
                    const ParseResult cr = run_parse(std::string(k.flag), token);

                    RC_ASSERT(fr.parsed_ok);
                    RC_ASSERT(k.get(fr.config) == k.get(cr.config));
                    RC_ASSERT(diagnostics_match(k.schema, k.flag, fr.diagnostics, cr.diagnostics));
                    break;
                }
                case 4: {
                    // ── shadow_light_budget (clamp, [0,16]) ──
                    const int raw = *rc::gen::inRange(-5, 30); // covers <0, in-range incl bounds, >16
                    const std::string token = token_from_int(raw);

                    const ConfigLoadResult fr =
                        load_one_key("quality.shadow_light_budget", token, base);
                    const ParseResult cr = run_parse("--point-light-quality", token);

                    RC_ASSERT(fr.parsed_ok);
                    RC_ASSERT(fr.config.quality.shadow_light_budget ==
                              cr.config.quality.shadow_light_budget);
                    RC_ASSERT(diagnostics_match("quality.shadow_light_budget",
                                                "--point-light-quality",
                                                fr.diagnostics, cr.diagnostics));
                    break;
                }
                case 5: {
                    // ── foliage_density (clamp, [1.0, foliage_density_max]) ──
                    // n/100 spans below 1.0, in-range (incl the 1.0 lower bound
                    // at n=100), and above the upper bound (~2.15).
                    const int n = *rc::gen::inRange(0, 400);
                    const float raw = static_cast<float>(n) / 100.0f;
                    const std::string token = token_from_float(raw);

                    const ConfigLoadResult fr =
                        load_one_key("quality.foliage_density", token, base);
                    const ParseResult cr = run_parse("--foliage-quality", token);

                    RC_ASSERT(fr.parsed_ok);
                    RC_ASSERT(fr.config.quality.foliage_density ==
                              cr.config.quality.foliage_density);
                    RC_ASSERT(diagnostics_match("quality.foliage_density",
                                                "--foliage-quality",
                                                fr.diagnostics, cr.diagnostics));
                    break;
                }
                case 6: {
                    // ── msaa_samples (fixed set {1,2,4}, REJECT-and-retain) ──
                    // In-set values (incl the 1/2/4 members) accept;
                    // out-of-set values are rejected and the prior count retained.
                    const int raw = *rc::gen::elementOf(
                        std::vector<int>{-1, 0, 1, 2, 3, 4, 5, 8});
                    const std::string token = token_from_int(raw);

                    const ConfigLoadResult fr =
                        load_one_key("quality.msaa_samples", token, base);
                    const ParseResult cr = run_parse("--msaa-quality", token);

                    RC_ASSERT(fr.parsed_ok);
                    RC_ASSERT(fr.config.quality.msaa_samples ==
                              cr.config.quality.msaa_samples);
                    RC_ASSERT(diagnostics_match("quality.msaa_samples",
                                                "--msaa-quality",
                                                fr.diagnostics, cr.diagnostics));
                    break;
                }
                case 7: {
                    // ── rain_intensity (parse_rain_intensity, clamp [0,1]) ──
                    // n/100 spans below 0, in-range (incl the 0.0 and 1.0 bounds
                    // at n=0 / n=100), and above 1.
                    const int n = *rc::gen::inRange(-150, 251);
                    const double raw = static_cast<double>(n) / 100.0;
                    const std::string token = token_from_double(raw);

                    const ConfigLoadResult fr =
                        load_one_key("weather.rain_intensity", token, base);
                    const ParseResult cr = run_parse("--rain-intensity", token);

                    RC_ASSERT(fr.parsed_ok);
                    RC_ASSERT(fr.config.weather.rain_intensity ==
                              cr.config.weather.rain_intensity);
                    RC_ASSERT(diagnostics_match("weather.rain_intensity",
                                                "--rain-intensity",
                                                fr.diagnostics, cr.diagnostics));
                    break;
                }
                default: {
                    // ── dry_down_seconds (clamp_dry_down_seconds, [0.5,30]) ──
                    // Silent clamp on BOTH sides (the helper reports no flag), so
                    // the only parity assertion that bites is the field equality;
                    // both diagnostic vectors must be empty. n/100 spans below
                    // 0.5, in-range (incl the 0.5 / 30 bounds), and above 30.
                    const int n = *rc::gen::inRange(0, 3501);
                    const float raw = static_cast<float>(n) / 100.0f;
                    const std::string token = token_from_float(raw);

                    const ConfigLoadResult fr =
                        load_one_key("weather.dry_down_seconds", token, base);
                    const ParseResult cr = run_parse("--dry-down-seconds", token);

                    RC_ASSERT(fr.parsed_ok);
                    RC_ASSERT(fr.config.weather.dry_down_seconds ==
                              cr.config.weather.dry_down_seconds);
                    RC_ASSERT(diagnostics_match("weather.dry_down_seconds",
                                                "--dry-down-seconds",
                                                fr.diagnostics, cr.diagnostics));
                    break;
                }
            }
        });
}

bool run_config_validation_parity_properties() {
    return run_p6_validation_parity();
}

const heaven::testing::Register g_register_config_validation_parity{
    &run_config_validation_parity_properties};

} // namespace
