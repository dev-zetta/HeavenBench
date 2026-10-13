// Feature: cli-argument-parser-refactor,  &  &
/*
 * HeavenBench — Property tests for the CLI parser's value-bearing flags
 * and for byte-for-byte weather/quality parity with the shared legacy helpers.
 *
 * Feature: cli-argument-parser-refactor
 *   •: Value flags consume and assign their token
 *
 *   •: Weather-family parity
 *
 *   •: Quality-family parity
 *
 *
 * The parse driver (heaven::parse_cli) is fully implemented at this point in
 * the refactor, so these properties exercise the PUBLIC entry point directly:
 * they build a real argv, call heaven::parse_cli(argc, argv), and assert on the
 * resulting BenchConfig / diagnostics.
 *
 * (value flags consume and assign):
 *   For each Arity::Value flag and a well-formed value token, parse_cli sets the
 *   flag's declared field to the parsed value. Coverage:
 *     • string flags  (--data, --node)   → field == verbatim token
 *     • integer flags (--width, --height) → field == strict-parsed int
 *     • float flags   (--cloth-rate, --tess-scale/-factor/-distance, --hdr-nits,
 *                      --hdr-paper-white) → field == strict-parsed float
 *     • --time-of-day → field == wrap_24(parsed float)
 *     • --api VALUE   → graphics_api stays GraphicsApi::Filament, no diagnostic
 *   A dedicated "looks-like-a-flag" sub-property feeds a value token that is
 *   itself a known flag name (e.g. `--data --no-grass`): the token must be
 *   CONSUMED and assigned to the field, never re-interpreted as a separate flag
 *   (so the decoy flag's controlled field keeps its default) —.
 *
 * (family parity):
 *   For a random argv composed of the weather (resp. quality) flag family with
 *   arbitrary value tokens (in-range, out-of-range, and junk), the features /
 *   weather (resp. quality) fields produced by parse_cli equal those produced
 *   by the shared engine/types.h oracle apply_weather_args (resp.
 *   apply_quality_args) over the SAME argv — by construction, since the parser
 *   routes the families through the same pure helpers. Each clamp / ignore /
 *   reject is additionally reported as exactly one diagnostic
 *  the diagnostic count is checked against the count independently
 *   derived from the same helpers token-by-token.
 *
 * A completeness guard asserts that every Arity::Value entry in the live
 * flag_table() is covered by one of the three properties here, so a value flag
 * added to the table later cannot silently go untested.
 *
 * parse_cli and the oracles are pure (no I/O, no GPU), so this runs on the
 * host. The file self-registers with the RapidCheck harness in test_main.cpp
 * via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rapidcheck.h>

#include "engine/cli_parser.h"
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

// Format a double so std::strtod recovers it exactly: "%.17g" round-trips any
// IEEE-754 double, so the (float) cast the strict parser applies lands on the
// same float we computed for the expectation.
std::string format_exact(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return {buf};
}

// Run the real parse driver over a token list (argv[0] is a dummy program name,
// as in a real launch).
ParseResult run_parse(const std::vector<std::string>& tokens) {
    std::vector<const char*> argv;
    argv.reserve(tokens.size() + 1);
    argv.push_back("heaven_benchmark");
    for (const std::string& t : tokens) argv.push_back(t.c_str());
    return heaven::parse_cli(argv);
}

// ── Coverage model: which value flags each property tests ───────

// Single source of truth for the test's value-flag coverage. The completeness
// guard cross-checks this against the live flag_table() so the property set
// cannot drift away from the table (one table, no untested flag).
const std::vector<std::string_view>& p5_string_flags() {
    static const std::vector<std::string_view> v = {
        "--data", "--node", "--tess-cache"};
    return v;
}
const std::vector<std::string_view>& p5_int_flags() {
    static const std::vector<std::string_view> v = {"--width", "--height"};
    return v;
}
const std::vector<std::string_view>& p5_float_flags() {
    static const std::vector<std::string_view> v = {
        "--cloth-rate", "--tess-scale", "--tess-factor", "--tess-distance",
        "--hdr-nits", "--hdr-paper-white"};
    return v;
}
const std::vector<std::string_view>& p5_other_value_flags() {
    // --time-of-day (wrapped) and --api (no-op) are exercised by their own
    // dedicated sub-properties below.
    static const std::vector<std::string_view> v = {"--time-of-day", "--api"};
    return v;
}
const std::vector<std::string_view>& weather_value_flags() {
    static const std::vector<std::string_view> v = {"--rain-intensity",
                                                    "--dry-down-seconds"};
    return v;
}
const std::vector<std::string_view>& quality_value_flags() {
    static const std::vector<std::string_view> v = {
        "--soft-shadow-quality", "--contact-shadow-quality", "--cloud-quality",
        "--ssgi-quality", "--point-light-quality", "--foliage-quality",
        "--msaa-quality"};
    return v;
}
const std::vector<std::string_view>& bloom_value_flags() {
    static const std::vector<std::string_view> v = {"--bloom-strength"};
    return v;
}

// The used-asset-packer value flag. Like --config / --write-config it carries
// no BenchConfig field — it activates a startup ACTION and is special-cased by
// name in the parse driver, recording its token on ParseResult.pack_target
// It is modelled by run_p5_pack_assets_property() below
// against the public parse_cli entry point, and listed here so the completeness
// guard stays a one-to-one image of the Arity::Value half of flag_table().
const std::vector<std::string_view>& pack_value_flags() {
    static const std::vector<std::string_view> v = {"--pack-assets"};
    return v;
}

// ── Startup-action value flags NOT modelled by this test ────────

// --config / --write-config are Arity::Value entries in flag_table(), but they
// are NOT renderer-Setting flags: they carry no BenchConfig field. They are
// startup actions captured on a side channel (ParseResult.config_path /
// write_config_path) and handled specially in the parse driver. Their behaviour
// is owned and modelled by the benchmark-config-file tests
// (prop_config_cli_path.cpp Properties 1 & 2 cover --config path capture;
// unit_config_cli_edges.cpp covers both flags' trailing/empty edges), so they
// legitimately do not map to a BenchConfig field and are excluded from the
// completeness guard below. The guard is NOT weakened for any other flag.
const std::set<std::string_view>& startup_action_value_flags() {
    static const std::set<std::string_view> v = {
        "--config", "--write-config", "--build-tessellation-cache",
        "--benchmark-loading", "--pack-texture-set",
        // Transient capture sidechannel is exercised by
        // unit_visual_capture_cli.cpp, including finite/range checks,
        // missing values, conflicts, repeated flags and seen-key isolation.
        "--camera-pose", "--capture-camera-to", "--camera-tour-time", "--camera-fov",
        "--capture", "--capture-frames", "--capture-count", "--capture-fps", "--profile", "--profile-seconds",
        // Transient trace controls have finite/path/combination tests in unit_microprofiler_cli.cpp.
        "--microprofile", "--microprofile-seconds"};
    return v;
}

// ──  value flags modelled by another CLI tests ────

// The advanced-materials-and-grading spec adds the tone-mapper / grading /
// physical-camera / knob value flags to flag_table(). Their parsing, clamping,
// and reject/clamp diagnostics are owned and modelled by THAT CLI tests
// (prop_cli_phase8_toggles.cpp and the CLI-defaults unit test), exactly
// as --config / --write-config are owned by the benchmark-config-file spec.
// They are excluded from this guard (mirroring startup_action_value_flags
// above) so the cli-argument-parser-refactor suite stays a one-to-one image of
// the flags IT owns without duplicating the coverage here. The guard is
// NOT weakened for any cli-argument-parser-refactor flag.
const std::set<std::string_view>& phase8_value_flags() {
    static const std::set<std::string_view> v = {
        "--tone-mapper", "--grading-contrast", "--grading-saturation",
        "--white-balance", "--grading-slope", "--grading-offset",
        "--grading-power", "--aperture", "--shutter", "--iso",
        "--specular-aa-quality", "--dynamic-resolution-quality",
        "--quality-preset", "--preset", "--material-workflow",
        "--ao-quality", "--bloom-quality", "--dof-quality", "--taa-quality",
        "--sun-shadow-map", "--point-shadow-map",
        "--cpu-stress-workers", "--cpu-stress-seconds",
        // Parsing and invalid-value handling are covered by
        // unit_texture_quality.cpp.
        "--texture-quality",
        // Grouped named levels, ordered preset/manual overrides, invalid values
        // and saved-control replay are covered by unit_tessellation_levels.cpp.
        "--tess-level",
    };
    return v;
}

// ── Music value flags modelled by the background-music-playback spec ────

// --music-volume is an Arity::Value entry whose parse/clamp/ignore semantics
// (parse_music_volume, with clamp + "not a number" diagnostics) are owned and
// modelled by the background-music-playback own CLI tests
// (unit_music_volume_cli.cpp). Like the grading knobs above it clamps
// and emits diagnostics, so it does not fit the P5 "assign verbatim, no
// diagnostic" shape and is excluded from this guard (mirroring
// phase8_value_flags). The guard is NOT weakened for any
// cli-argument-parser-refactor flag.
const std::set<std::string_view>& music_value_flags() {
    static const std::set<std::string_view> v = {"--music-volume"};
    return v;
}

// Read the string / int / float field a given value flag drives. The switch
// here is the test's mirror of the flag_table() handlers' target fields.
std::string read_string_field(const BenchConfig& c, std::string_view flag) {
    if (flag == "--data") return c.data_dir;
    if (flag == "--node") return c.node_file;
    if (flag == "--tess-cache") return c.tessellation.cache_file;
    if (flag == "--quality-preset") return c.quality_preset_label;
    return {};
}
int read_int_field(const BenchConfig& c, std::string_view flag) {
    if (flag == "--width")  return c.width;
    if (flag == "--height") return c.height;
    return 0;
}
float read_float_field(const BenchConfig& c, std::string_view flag) {
    if (flag == "--cloth-rate")      return c.cloth_sim_hz;
    if (flag == "--tess-scale")      return c.tessellation.scale;
    if (flag == "--tess-factor")     return c.tessellation.factor;
    if (flag == "--tess-distance")   return c.tessellation.distance;
    if (flag == "--hdr-nits")        return c.hdr_peak_nits;
    if (flag == "--hdr-paper-white") return c.hdr_paper_white_nits;
    return 0.0f;
}

// ── Completeness guard ──────────────────────────────────────────

// Every Arity::Value entry in the live table must be covered by one of the
// three properties (P5 non-family flags, P7 weather, P8 quality). Adding a
// value flag to the table without modelling it here fails this guard rather
// than silently going untested.
bool run_value_flag_coverage_guard() {
    std::set<std::string_view> modelled;
    auto add_all = [&](const std::vector<std::string_view>& v) {
        for (std::string_view n : v) modelled.insert(n);
    };
    add_all(p5_string_flags());
    add_all(p5_int_flags());
    add_all(p5_float_flags());
    add_all(p5_other_value_flags());
    add_all(weather_value_flags());
    add_all(quality_value_flags());
    add_all(bloom_value_flags());
    add_all(pack_value_flags());

    std::set<std::string_view> table_value_flags;
    for (const heaven::FlagSpec& s : heaven::flag_table()) {
        if (s.arity == heaven::Arity::Value) table_value_flags.insert(s.name);
    }

    bool ok = true;
    for (std::string_view n : table_value_flags) {
        // Startup-action value flags (--config / --write-config) carry no
        // BenchConfig Setting and are modelled by the benchmark-config-file
        // tests; skip them rather than treating them as unmodelled.
        if (startup_action_value_flags().contains(n)) continue;
        // value flags are owned by the advanced-materials-and-grading
        // CLI tests; skip them here for the same reason.
        if (phase8_value_flags().contains(n)) continue;
        // --music-volume is owned by the background-music-playback CLI
        // tests (it clamps + warns like the knobs); skip it here too.
        if (music_value_flags().contains(n)) continue;
        // Scene defaults and ordered overrides are checked independently.
        if (n == "--test-scene") continue;
        // Provider identities, invalid-value retention, precedence and the
        // diagnostic scoring contract are covered by unit_frame_generation.
        if (n == "--frame-generation") continue;
        if (!modelled.contains(n)) {
            std::printf("[prop_cli_value_flags] table value flag '%.*s' is not "
                        "modelled by any property\n",
                        static_cast<int>(n.size()), n.data());
            ok = false;
        }
    }
    for (std::string_view n : modelled) {
        if (!table_value_flags.contains(n)) {
            std::printf("[prop_cli_value_flags] modelled value flag '%.*s' is not "
                        "an Arity::Value entry in the table\n",
                        static_cast<int>(n.size()), n.data());
            ok = false;
        }
    }
    return ok;
}

// ── Generators ──────────────────────────────────────────────────

// A path/identifier-like token over a null-free, whitespace-free alphabet so it
// round-trips through argv's NUL-terminated const char* without truncation.
std::string gen_word_token() {
    static const std::string alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123456789-_./";
    const std::string s = *rc::gen::container<std::string>(
        rc::gen::elementOf(alphabet));
    return s;
}

// A family value token: a mix of in/out-of-range integers, fixed-point floats,
// small valid-looking levels, and non-numeric junk. The SAME token feeds both
// parse_cli and the oracle, so its content never breaks parity; the variety
// just exercises the accept / clamp / ignore / reject paths.
std::string gen_family_value() {
    const int kind = *rc::gen::inRange(0, 4);
    switch (kind) {
        case 0: // integers spanning valid and out-of-range levels/budgets
            return std::to_string(*rc::gen::inRange(-6, 19));
        case 1: { // fixed-point float in [-2.0, 6.0]
            const int g = *rc::gen::inRange(-200, 601);
            return format_exact(static_cast<double>(g) / 100.0);
        }
        case 2: { // non-numeric junk
            static const std::vector<std::string> junk = {
                "abc", "x", "", " ", "drizzle", "1.2.3", "nan", "--no-rain"};
            return *rc::gen::elementOf(junk);
        }
        default: // small ints that hit valid {0,1,2} levels and {1,2,4} msaa
            return std::to_string(*rc::gen::inRange(0, 5));
    }
}

// ── a: string value flags assign their token verbatim ─
bool run_p5_string_flags_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 5: string value flags "
        "consume and assign their token (Validates: Requirements 5.1, 5.2, 5.9)",
        [] {
            const std::string_view flag = *rc::gen::elementOf(p5_string_flags());
            const std::string value = gen_word_token();

            const ParseResult r = run_parse({std::string(flag), value});

            RC_ASSERT(read_string_field(r.config, flag) == value);
            RC_ASSERT(r.diagnostics.empty());
            RC_ASSERT(!r.help_requested);
        });
}

// ── b: integer value flags assign the strict-parsed int ─
bool run_p5_int_flags_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 5: integer value flags "
        "consume and assign their token (Validates: Requirements 5.3, 5.4, 5.9)",
        [] {
            const std::string_view flag = *rc::gen::elementOf(p5_int_flags());
            const int value = *rc::gen::inRange(-1'000'000'000, 1'000'000'001);
            const std::string tok = std::to_string(value);

            const ParseResult r = run_parse({std::string(flag), tok});

            RC_ASSERT(read_int_field(r.config, flag) == value);
            RC_ASSERT(r.diagnostics.empty());
        });
}

// ── c: float value flags assign the strict-parsed float ─
bool run_p5_float_flags_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 5: float value flags "
        "consume and assign their token (Validates: Requirements 5.5, 5.6, 5.7, "
        "5.8, 5.9)",
        [] {
            const std::string_view flag = *rc::gen::elementOf(p5_float_flags());
            // Bounded fixed-point grid → finite values that round-trip through
            // %.17g to the same float the strict parser yields.
            const int g = *rc::gen::inRange(-10'000'000, 10'000'001);
            const double d = static_cast<double>(g) / 1000.0; // [-10000, 10000]
            const std::string tok = format_exact(d);

            const ParseResult r = run_parse({std::string(flag), tok});

            RC_ASSERT(read_float_field(r.config, flag) == static_cast<float>(d));
            RC_ASSERT(r.diagnostics.empty());
        });
}

// ── d: --time-of-day assigns wrap_24(parsed float) ────
bool run_p5_time_of_day_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 5: --time-of-day "
        "consumes and assigns wrap_24(value) (Validates: Requirements 5.9, 6.1)",
        [] {
            const int g = *rc::gen::inRange(-100'000, 100'001);
            const double d = static_cast<double>(g) / 100.0; // [-1000, 1000]
            const std::string tok = format_exact(d);

            const ParseResult r = run_parse({"--time-of-day", tok});

            RC_ASSERT(r.config.time_of_day ==
                      heaven::wrap_24(static_cast<float>(d)));
            RC_ASSERT(r.config.time_of_day >= 0.0f);
            RC_ASSERT(r.config.time_of_day < 24.0f);
            RC_ASSERT(r.diagnostics.empty());
        });
}

// ── e: --api consumes its value and stays a no-op ─────
bool run_p5_api_noop_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 5: --api consumes its "
        "value token and leaves graphics_api at Filament (Validates: "
        "Requirements 5.9, 9.1)",
        [] {
            const std::string value = gen_word_token();

            const ParseResult r = run_parse({"--api", value});

            // graphics_api is untouched (the only backend) and the consumed
            // value is never reported as an unknown flag.
            RC_ASSERT(r.config.graphics_api == heaven::GraphicsApi::Filament);
            RC_ASSERT(r.diagnostics.empty());
        });
}

// ── f: a flag-like value token is consumed, not re-parsed ─

// Feeds a value token that is itself a known flag name (a "decoy") to a value
// flag. The decoy must be CONSUMED as the value, never applied: the field the
// decoy would otherwise control keeps its default, and no diagnostic is emitted
// (and 9.1 for --api). This is the `--data --no-grass`
// scenario generalised over the value flags that assign their token verbatim
// plus --api.
struct Decoy {
    std::string flag;          // the decoy flag name fed as a value
    std::function<bool(const BenchConfig&)> at_default; // true iff not applied
};

const std::vector<Decoy>& decoys() {
    static const std::vector<Decoy> d = {
        {"--no-grass",  [](const BenchConfig& c) { return c.features.grass == true; }},
        {"--fullscreen",[](const BenchConfig& c) { return c.fullscreen == false; }},
        {"--hdr",       [](const BenchConfig& c) { return c.hdr == false; }},
        {"--benchmark", [](const BenchConfig& c) { return c.benchmark_mode == false; }},
        {"--no-msaa",   [](const BenchConfig& c) { return c.features.msaa == true; }},
    };
    return d;
}

bool run_p5_flag_like_value_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 5: a flag-like value "
        "token is consumed, not re-interpreted (Validates: Requirements 5.9, "
        "9.1)",
        [] {
            // A value flag that assigns its token verbatim (--data/--node) or
            // swallows it (--api) — all leave the decoy's field at default.
            static const std::vector<std::string_view> consumers = {
                "--data", "--node", "--api"};
            const std::string_view flag = *rc::gen::elementOf(consumers);
            const Decoy& decoy = *rc::gen::elementOf(decoys());

            const ParseResult r = run_parse({std::string(flag), decoy.flag});

            // The decoy flag was consumed as the value, so its controlled field
            // stays at its default (it was NOT applied as a separate flag).
            RC_ASSERT(decoy.at_default(r.config));
            // For --data/--node the token is assigned verbatim.
            if (flag == "--data" || flag == "--node") {
                RC_ASSERT(read_string_field(r.config, flag) == decoy.flag);
            }
            // The consumed token is never reported as an unknown option.
            RC_ASSERT(r.diagnostics.empty());
        });
}

// ──: weather-family parity ───────────────────────────
bool run_p7_weather_parity_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 7: weather-family "
        "parity with apply_weather_args (Validates: Requirements 7.1, 7.2, 7.3, "
        "7.4, 7.5, 7.6, 7.7, 11.3)",
        [] {
            const int count = *rc::gen::inRange(0, 8);

            std::vector<std::string> tokens;
            int expected_diags = 0; // clamp/ignore reports the parser must emit
            for (int i = 0; i < count; ++i) {
                const int kind = *rc::gen::inRange(0, 4);
                switch (kind) {
                    case 0:
                        tokens.emplace_back("--no-rain");
                        break;
                    case 1:
                        tokens.emplace_back("--no-wet-reflections");
                        break;
                    case 2: {
                        const std::string v = gen_family_value();
                        tokens.emplace_back("--rain-intensity");
                        tokens.push_back(v);
                        // Parser reports exactly when parse_rain_intensity flags
                        // the value as clamped or ignored.
                        const heaven::RainIntensityParse p =
                            heaven::parse_rain_intensity(v.c_str());
                        if (p.ignored || p.clamped) ++expected_diags;
                        break;
                    }
                    default: {
                        const std::string v = gen_family_value();
                        tokens.emplace_back("--dry-down-seconds");
                        tokens.push_back(v); // no diagnostic on this flag
                        break;
                    }
                }
            }

            // Build the shared argv once; feed it to both the parser and oracle.
            std::vector<const char*> argv;
            argv.reserve(tokens.size() + 1);
            argv.push_back("heaven_benchmark");
            for (const std::string& t : tokens) argv.push_back(t.c_str());
            const ParseResult r = heaven::parse_cli(argv);

            heaven::AppConfig oracle;
            heaven::apply_cli_arguments(oracle, argv);

            // The features + weather fields equal the oracle's by construction.
            RC_ASSERT(r.config.features.rain == oracle.features.rain);
            RC_ASSERT(r.config.features.wet_reflections ==
                      oracle.features.wet_reflections);
            RC_ASSERT(r.config.weather.rain_intensity ==
                      oracle.weather.rain_intensity);
            RC_ASSERT(r.config.weather.dry_down_seconds ==
                      oracle.weather.dry_down_seconds);

            // Each clamp/ignore is reported exactly once; nothing else is.
            RC_ASSERT(static_cast<int>(r.diagnostics.size()) == expected_diags);
        });
}

// ──: quality-family parity ───────────────────────────
bool run_p8_quality_parity_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 8: quality-family "
        "parity with apply_quality_args (Validates: Requirements 8.1, 8.2, 8.3, "
        "8.4, 8.5, 8.6, 8.7, 11.3)",
        [] {
            // Default knobs supply the constant bounds the clamp helpers read;
            // those bounds are never mutated by any flag, so deriving the
            // expected diagnostic per token from a default-constructed knob set
            // matches what parse_cli computes against config.quality.
            const heaven::QualityKnobs def_knobs;

            const std::vector<std::string_view>& flags = quality_value_flags();
            const int count = *rc::gen::inRange(0, 9);

            std::vector<std::string> tokens;
            int expected_diags = 0;
            for (int i = 0; i < count; ++i) {
                const std::string_view flag = *rc::gen::elementOf(flags);
                const std::string v = gen_family_value();
                tokens.emplace_back(flag);
                tokens.push_back(v);

                // Mirror the parser's per-token diagnostic decision using the
                // SAME shared helpers.
                if (flag == "--soft-shadow-quality" ||
                    flag == "--contact-shadow-quality" ||
                    flag == "--cloud-quality" || flag == "--ssgi-quality") {
                    const int value =
                        heaven::parse_int_prefix(v).value_or(0);
                    if (heaven::clamp_quality_level(value).clamped)
                        ++expected_diags;
                } else if (flag == "--point-light-quality") {
                    const int value =
                        heaven::parse_int_prefix(v).value_or(0);
                    if (heaven::clamp_quality_shadow_light_budget(
                            value, def_knobs).clamped)
                        ++expected_diags;
                } else if (flag == "--foliage-quality") {
                    const float value =
                        heaven::parse_float_prefix(v).value_or(0.0f);
                    if (heaven::clamp_quality_foliage_density(
                            value, def_knobs).clamped)
                        ++expected_diags;
                } else if (flag == "--msaa-quality") {
                    // rejected depends only on the requested value, not on the
                    // retained previous count, so order-independent counting is
                    // sound.
                    const int value =
                        heaven::parse_int_prefix(v).value_or(0);
                    if (heaven::apply_msaa_samples(value,
                                                   def_knobs.msaa_samples)
                            .rejected)
                        ++expected_diags;
                }
            }

            std::vector<const char*> argv;
            argv.reserve(tokens.size() + 1);
            argv.push_back("heaven_benchmark");
            for (const std::string& t : tokens) argv.push_back(t.c_str());
            const ParseResult r = heaven::parse_cli(argv);

            heaven::AppConfig oracle;
            heaven::apply_cli_arguments(oracle, argv);

            // Every quality field equals the oracle's by construction.
            RC_ASSERT(r.config.quality.soft_shadow_level ==
                      oracle.quality.soft_shadow_level);
            RC_ASSERT(r.config.quality.contact_shadow_level ==
                      oracle.quality.contact_shadow_level);
            RC_ASSERT(r.config.quality.cloud_quality ==
                      oracle.quality.cloud_quality);
            RC_ASSERT(r.config.quality.ssgi_quality ==
                      oracle.quality.ssgi_quality);
            RC_ASSERT(r.config.quality.shadow_light_budget ==
                      oracle.quality.shadow_light_budget);
            RC_ASSERT(r.config.quality.foliage_density ==
                      oracle.quality.foliage_density);
            RC_ASSERT(r.config.quality.msaa_samples ==
                      oracle.quality.msaa_samples);

            // Each clamp/reject is reported exactly once; nothing else is.
            RC_ASSERT(static_cast<int>(r.diagnostics.size()) == expected_diags);
        });
}

// ── g: --pack-assets consumes an OPTIONAL value into pack_target ─

// --pack-assets activates the used-asset packer (a startup ACTION) and takes an
// OPTIONAL Pack_Target directory. Mirrors 's "consume and assign"
// intent for the one value flag whose target lives on ParseResult, not
// BenchConfig:
//   • with a non-dash value token: pack_requested is set, pack_target == the
//     token verbatim, no diagnostic, BenchConfig untouched (used-asset-packer

//   • bare/trailing with NO value: pack_requested is STILL set (so "flag
//     present" is distinguishable from "flag absent"), pack_target stays unset,
//     and NO diagnostic is emitted — the value is optional and main() defaults
//     the Pack_Target to "assets" (used-asset-packer).
//   • followed by a dash-token (e.g. --pack-assets --no-grass): the dash-token
//     is NOT consumed as the target, so pack_target stays unset and the decoy
//     flag parses normally (used-asset-packer).
bool run_p5_pack_assets_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 5: --pack-assets "
        "consumes an optional non-dash value into ParseResult.pack_target "
        "(Validates: used-asset-packer Requirements 1.1, 1.3; Requirements "
        "5.9, 11.1)",
        [] {
            // A non-dash value: gen a word token and guarantee it does not begin
            // with '-' (the parser leaves dash-tokens for normal parsing).
            std::string value = gen_word_token();
            if (value.empty() || value.front() == '-') value.insert(value.begin(), 'a');

            // With a non-dash value: the token is consumed into pack_target
            // verbatim.
            const ParseResult r = run_parse({"--pack-assets", value});
            RC_ASSERT(r.pack_requested);
            RC_ASSERT(r.pack_target.has_value());
            RC_ASSERT(*r.pack_target == value);
            RC_ASSERT(r.diagnostics.empty());
            RC_ASSERT(!r.help_requested);
            // The packer is a startup action, not a Setting: BenchConfig stays
            // at its defaults.
            const BenchConfig d;
            RC_ASSERT(r.config.data_dir == d.data_dir);
            RC_ASSERT(r.config.node_file == d.node_file);

            // Bare/trailing with no value: pack_requested still set, pack_target
            // unset, and NO diagnostic (the value is optional now).
            const ParseResult r2 = run_parse({"--pack-assets"});
            RC_ASSERT(r2.pack_requested);
            RC_ASSERT(!r2.pack_target.has_value());
            RC_ASSERT(r2.diagnostics.empty());

            // Followed by a dash-token: it is NOT consumed as the target.
            // pack_target stays unset, pack_requested is true, and the decoy
            // flag (--no-grass) parses normally (features.grass disabled).
            const ParseResult r3 = run_parse({"--pack-assets", "--no-grass"});
            RC_ASSERT(r3.pack_requested);
            RC_ASSERT(!r3.pack_target.has_value());
            RC_ASSERT(r3.diagnostics.empty());
            RC_ASSERT(r3.config.features.grass == false);
        });
}

bool run_bloom_strength_property() {
    return rc::check("Requested bloom strength uses its bounded value and consumes only its token", [] {
        const double raw = *rc::gen::inRange(-2000, 2001) / 1000.0;
        const auto expected = heaven::clamp_bloom_strength(static_cast<float>(raw));
        const auto result = run_parse({"--bloom-strength", format_exact(raw), "--no-grass"});
        RC_ASSERT(result.config.bloom_strength == expected.value);
        RC_ASSERT(result.diagnostics.size() == (expected.clamped ? 1u : 0u));
        RC_ASSERT(result.seen_keys.contains("bloom_strength"));
        RC_ASSERT(!result.config.features.grass);
    });
}

bool run_cli_value_flags_properties() {
    bool ok = true;
    ok = run_value_flag_coverage_guard() && ok;
    ok = run_p5_string_flags_property() && ok;
    ok = run_p5_int_flags_property() && ok;
    ok = run_p5_float_flags_property() && ok;
    ok = run_p5_time_of_day_property() && ok;
    ok = run_p5_api_noop_property() && ok;
    ok = run_p5_flag_like_value_property() && ok;
    ok = run_p5_pack_assets_property() && ok;
    ok = run_p7_weather_parity_property() && ok;
    ok = run_p8_quality_parity_property() && ok;
    ok = run_bloom_strength_property() && ok;
    return ok;
}

const heaven::testing::Register g_register_cli_value_flags{
    &run_cli_value_flags_properties};

} // namespace
