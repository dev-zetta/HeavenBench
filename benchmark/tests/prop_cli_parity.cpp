// Feature: cli-argument-parser-refactor,  &
/*
 * HeavenBench — Property and example tests for legacy parity and edges.
 *
 * Feature: cli-argument-parser-refactor
 *   •: Legacy parity over well-formed known flags
 *
 *   •: Empty command line yields the default configuration
 *
 *   • Example/edge cases:
 *       --api filament no-op
 *       --help signaling
 *       empty command line
 *       missing trailing value
 *       two malformed-value reports
 *
 * parse_cli is fully implemented at this point in the refactor (and main.cpp is
 * now a thin wrapper around it), so these tests exercise the PUBLIC entry point
 * directly: they build a real argv, call heaven::parse_cli(argc, argv), and
 * assert on the resulting BenchConfig / diagnostics.
 *
 * (legacy parity):
 *   A RapidCheck generator emits a well-formed known-flag command line — a
 *   random-length, random-ordered sequence of boolean toggles, alias members,
 *   and value flags, each value flag carrying a WELL-FORMED value token. The
 *   oracle is an INDEPENDENT reimplementation of the legacy parser semantics:
 *   it replays the SAME token sequence left-to-right with its own explicit
 *   field mapping (toggles + verbatim string / atoi int / atof float value
 *   assignment + a local 24-hour wrap for --time-of-day), and applies the
 *   weather and quality families by calling the established shared helpers
 *   apply_weather_args / apply_quality_args over the same argv (the current
 *   parity reference for those families). parse_cli(argv).config must equal the
 *   oracle config field-by-field.
 *
 *   Parity is defined over WELL-FORMED, KNOWN flags only (per the implementation): the
 *   non-family numeric flags therefore only ever receive well-formed value
 *   tokens, so the malformed-report path (a deliberate divergence from legacy)
 *   is never exercised here. The weather/quality families MAY receive arbitrary
 *   tokens because both parse_cli and the oracle route them through the same
 *   pure helpers, so they agree by construction regardless of the token. Both
 *   sides process tokens left-to-right, so a repeated flag's last occurrence
 *   wins identically.
 *
 * (empty command line):
 *   parse_cli over program-name-only argv yields a BenchConfig field-equal to a
 *   default-constructed BenchConfig, with no diagnostics and help_requested ==
 *   false.
 *
 * parse_cli and the oracle helpers are pure (no I/O, no GPU), so this runs on
 * the host. The file self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <cstdio>
#include <functional>
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

// Format a double so std::strtod / std::atof recovers it exactly: "%.17g"
// round-trips any IEEE-754 double, so the (float) cast both parse_cli's strict
// parser and the oracle's atof apply lands on the same float.
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

// ── Independent legacy-semantics oracle ─────────────────────────

// A from-scratch reimplementation of the pre-refactor parse_args() behaviour,
// written WITHOUT reference to flag_table(): the toggle/value field mapping is
// spelled out explicitly here so it is a genuinely independent reference, and
// the weather/quality families are delegated to the established shared helpers
// (the current parity reference for those families). Replays the token
// sequence left-to-right so a repeated flag's last occurrence wins, exactly as
// the parser does.
BenchConfig legacy_oracle(int argc, const char* const argv[]) {
    BenchConfig c; // default-constructed baseline

    // Local 24-hour wrap, an independent copy of the legacy add/subtract loop.
    auto wrap24 = [](float t) {
        while (t < 0.0f)   t += 24.0f;
        while (t >= 24.0f) t -= 24.0f;
        return t;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view tok(argv[i]);

        // ── Boolean toggles (explicit, independent field mapping) ──
        if (tok == "--fullscreen")              c.fullscreen = true;
        else if (tok == "--no-tessellation")    c.tessellation.enabled = false;
        else if (tok == "--no-heat-haze")     { c.heat_haze_enabled = false; c.features.heat_haze = false; }
        else if (tok == "--no-grass")           c.features.grass = false;
        else if (tok == "--no-particles")       c.features.particles = false;
        else if (tok == "--no-god-rays")        c.features.god_rays = false;
        else if (tok == "--no-volumetric-fog")  c.features.volumetric_fog = false;
        else if (tok == "--no-sky")             c.features.sky = false;
        else if (tok == "--no-lens-flares")     c.features.lens_flares = false;
        else if (tok == "--no-soft-shadows")    c.features.soft_shadows = false;
        else if (tok == "--no-contact-shadows") c.features.contact_shadows = false;
        else if (tok == "--clouds")             c.features.clouds = true;
        else if (tok == "--wind-debug")         c.wind_debug = true;
        else if (tok == "--hdr")                c.hdr = true;
        else if (tok == "--benchmark")          c.benchmark_mode = true;

        // ── Alias pairs (last occurrence wins via left-to-right replay) ──
        else if (tok == "--no-point-light-shadows") c.features.point_light_shadows = false;
        else if (tok == "--point-light-shadows")    c.features.point_light_shadows = true;
        else if (tok == "--no-msaa")                c.features.msaa = false;
        else if (tok == "--msaa")                   c.features.msaa = true;
        else if (tok == "--no-refraction")          c.features.refraction = false;
        else if (tok == "--refraction")             c.features.refraction = true;
        else if (tok == "--no-reflections")         c.features.reflections = false;
        else if (tok == "--reflections")            c.features.reflections = true;
        else if (tok == "--no-volumetric-clouds")   c.features.volumetric_clouds = false;
        else if (tok == "--volumetric-clouds")      c.features.volumetric_clouds = true;
        else if (tok == "--no-ssgi")                c.features.ssgi = false;
        else if (tok == "--ssgi")                   c.features.ssgi = true;
        else if (tok == "--no-massive-foliage")     c.features.massive_foliage = false;
        else if (tok == "--massive-foliage")        c.features.massive_foliage = true;
        else if (tok == "--no-ground-relief")       c.features.ground_relief = false;
        else if (tok == "--ground-relief")          c.features.ground_relief = true;

        // ── Non-family string value flags (verbatim assignment) ──
        else if (tok == "--data" && i + 1 < argc) c.data_dir = argv[++i];
        else if (tok == "--node" && i + 1 < argc) c.node_file = argv[++i];

        // ── Non-family integer value flags (well-formed inputs) ──
        else if (tok == "--width" && i + 1 < argc)
            c.width = heaven::parse_int_strict(argv[++i]).value_or(0);
        else if (tok == "--height" && i + 1 < argc)
            c.height = heaven::parse_int_strict(argv[++i]).value_or(0);

        // ── Non-family float value flags (well-formed inputs) ──
        else if (tok == "--cloth-rate" && i + 1 < argc)
            c.cloth_sim_hz =
                heaven::parse_float_strict(argv[++i]).value_or(0.0f);
        else if (tok == "--tess-scale" && i + 1 < argc)
            c.tessellation.scale =
                heaven::parse_float_strict(argv[++i]).value_or(0.0f);
        else if (tok == "--tess-factor" && i + 1 < argc)
            c.tessellation.factor =
                heaven::parse_float_strict(argv[++i]).value_or(0.0f);
        else if (tok == "--tess-distance" && i + 1 < argc)
            c.tessellation.distance =
                heaven::parse_float_strict(argv[++i]).value_or(0.0f);
        else if (tok == "--hdr-nits" && i + 1 < argc)
            c.hdr_peak_nits =
                heaven::parse_float_strict(argv[++i]).value_or(0.0f);
        else if (tok == "--hdr-paper-white" && i + 1 < argc)
            c.hdr_paper_white_nits =
                heaven::parse_float_strict(argv[++i]).value_or(0.0f);
        else if (tok == "--time-of-day" && i + 1 < argc)
            c.time_of_day = wrap24(
                heaven::parse_float_strict(argv[++i]).value_or(0.0f));

        // ── --api stays a no-op. Weather / quality flags skip their value here so it is
        //    not re-read as a flag; the field assignment is performed below by
        //    the shared apply_weather_args / apply_quality_args helpers. ──
        else if ((tok == "--api" || tok == "--rain-intensity" || tok == "--dry-down-seconds" ||
                  tok == "--soft-shadow-quality" || tok == "--contact-shadow-quality" ||
                  tok == "--cloud-quality" || tok == "--ssgi-quality" ||
                  tok == "--point-light-quality" || tok == "--foliage-quality" ||
                  tok == "--msaa-quality") && i + 1 < argc) {
            ++i;
        }

        // The weather toggles --no-rain / --no-wet-reflections carry no value
        // and fall through here untouched; apply_weather_args sets them below.
    }

    // The weather and quality families are the established parity reference:
    // applying them over the full argv (independent of the loop above, which
    // deliberately skipped their values) reproduces exactly what the legacy
    // parser delegated to these same helpers.
    heaven::apply_cli_arguments(
        c, std::span<const char* const>{
               argv, static_cast<std::size_t>(argc)});

    return c;
}

// Field-by-field equality assertion over every BenchConfig field a flag can
// touch (plus a few never-touched fields, to confirm the parser leaves them at
// their defaults). Floats are compared with exact == because parity is
// bit-exact by construction: both sides run identical numeric computations.
void assert_config_equal(const BenchConfig& got, const BenchConfig& want) {
    // BenchConfig-only fields.
    RC_ASSERT(got.graphics_api   == want.graphics_api);
    RC_ASSERT(got.benchmark_mode == want.benchmark_mode);

    // String / path fields.
    RC_ASSERT(got.data_dir   == want.data_dir);
    RC_ASSERT(got.node_file  == want.node_file);
    RC_ASSERT(got.shader_dir == want.shader_dir);

    // Window + display fields.
    RC_ASSERT(got.width      == want.width);
    RC_ASSERT(got.height     == want.height);
    RC_ASSERT(got.fullscreen == want.fullscreen);
    RC_ASSERT(got.hdr                  == want.hdr);
    RC_ASSERT(got.hdr_peak_nits        == want.hdr_peak_nits);
    RC_ASSERT(got.hdr_paper_white_nits == want.hdr_paper_white_nits);

    // Tessellation.
    RC_ASSERT(got.tessellation.enabled  == want.tessellation.enabled);
    RC_ASSERT(got.tessellation.scale    == want.tessellation.scale);
    RC_ASSERT(got.tessellation.factor   == want.tessellation.factor);
    RC_ASSERT(got.tessellation.distance == want.tessellation.distance);

    // Misc scalar fields.
    RC_ASSERT(got.time_of_day      == want.time_of_day);
    RC_ASSERT(got.heat_haze_enabled == want.heat_haze_enabled);
    RC_ASSERT(got.wind_debug       == want.wind_debug);
    RC_ASSERT(got.cloth_sim_hz     == want.cloth_sim_hz);

    // Feature toggles.
    RC_ASSERT(got.features.grass               == want.features.grass);
    RC_ASSERT(got.features.particles           == want.features.particles);
    RC_ASSERT(got.features.god_rays            == want.features.god_rays);
    RC_ASSERT(got.features.volumetric_fog      == want.features.volumetric_fog);
    RC_ASSERT(got.features.clouds              == want.features.clouds);
    RC_ASSERT(got.features.sky                 == want.features.sky);
    RC_ASSERT(got.features.lens_flares         == want.features.lens_flares);
    RC_ASSERT(got.features.heat_haze           == want.features.heat_haze);
    RC_ASSERT(got.features.rain                == want.features.rain);
    RC_ASSERT(got.features.wet_reflections     == want.features.wet_reflections);
    RC_ASSERT(got.features.soft_shadows        == want.features.soft_shadows);
    RC_ASSERT(got.features.contact_shadows     == want.features.contact_shadows);
    RC_ASSERT(got.features.point_light_shadows == want.features.point_light_shadows);
    RC_ASSERT(got.features.msaa                == want.features.msaa);
    RC_ASSERT(got.features.refraction          == want.features.refraction);
    RC_ASSERT(got.features.reflections         == want.features.reflections);
    RC_ASSERT(got.features.volumetric_clouds   == want.features.volumetric_clouds);
    RC_ASSERT(got.features.ssgi                == want.features.ssgi);
    RC_ASSERT(got.features.massive_foliage     == want.features.massive_foliage);
    RC_ASSERT(got.features.ground_relief       == want.features.ground_relief);

    // Weather fields the flags touch.
    RC_ASSERT(got.weather.rain_intensity   == want.weather.rain_intensity);
    RC_ASSERT(got.weather.dry_down_seconds == want.weather.dry_down_seconds);

    // Quality knobs the flags touch.
    RC_ASSERT(got.quality.soft_shadow_level    == want.quality.soft_shadow_level);
    RC_ASSERT(got.quality.contact_shadow_level == want.quality.contact_shadow_level);
    RC_ASSERT(got.quality.cloud_quality        == want.quality.cloud_quality);
    RC_ASSERT(got.quality.ssgi_quality         == want.quality.ssgi_quality);
    RC_ASSERT(got.quality.shadow_light_budget  == want.quality.shadow_light_budget);
    RC_ASSERT(got.quality.msaa_samples         == want.quality.msaa_samples);
    RC_ASSERT(got.quality.foliage_density      == want.quality.foliage_density);
}

// ── Generation pools ────────────────────────────────────────────

// All Arity::None flags (boolean toggles + alias members + the two weather
// toggles), EXCLUDING --help (which signals rather than configures and would
// derail parity). The oracle routes each appropriately.
const std::vector<std::string_view>& toggle_flags() {
    static const std::vector<std::string_view> v = {
        "--fullscreen", "--no-tessellation", "--no-heat-haze", "--no-grass",
        "--no-particles", "--no-god-rays", "--no-volumetric-fog", "--no-sky",
        "--no-lens-flares", "--no-soft-shadows", "--no-contact-shadows",
        "--clouds", "--wind-debug", "--hdr", "--benchmark", "--no-rain",
        "--no-wet-reflections",
        "--no-point-light-shadows", "--point-light-shadows", "--no-msaa", "--msaa",
        "--no-refraction", "--refraction", "--no-reflections", "--reflections",
        "--no-volumetric-clouds", "--volumetric-clouds", "--no-ssgi", "--ssgi",
        "--no-massive-foliage", "--massive-foliage",
        "--no-ground-relief", "--ground-relief",
    };
    return v;
}

const std::vector<std::string_view>& string_value_flags() {
    static const std::vector<std::string_view> v = {"--data", "--node"};
    return v;
}
const std::vector<std::string_view>& int_value_flags() {
    static const std::vector<std::string_view> v = {"--width", "--height"};
    return v;
}
const std::vector<std::string_view>& float_value_flags() {
    static const std::vector<std::string_view> v = {
        "--cloth-rate", "--tess-scale", "--tess-factor", "--tess-distance",
        "--hdr-nits", "--hdr-paper-white"};
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

// A path/identifier-like token over a null-free, whitespace-free alphabet so it
// round-trips through argv's NUL-terminated const char* without truncation.
std::string gen_word_token() {
    static const std::string alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123456789-_./";
    return *rc::gen::container<std::string>(rc::gen::elementOf(alphabet));
}

// A WELL-FORMED float token: a bounded fixed-point grid formatted via %.17g so
// it round-trips through atof/strtod to the same float on both sides.
std::string gen_float_token() {
    const int g = *rc::gen::inRange(-10'000'000, 10'000'001);
    return format_exact(static_cast<double>(g) / 1000.0); // [-10000, 10000]
}

// A family value token: a mix of in/out-of-range integers, fixed-point floats,
// and non-numeric junk. The SAME token feeds parse_cli and the oracle, so its
// content never breaks parity (both route through the same shared helpers); the
// variety just exercises the accept / clamp / ignore / reject paths.
std::string gen_family_value() {
    const int kind = *rc::gen::inRange(0, 4);
    switch (kind) {
        case 0: return std::to_string(*rc::gen::inRange(-6, 19));
        case 1: {
            const int g = *rc::gen::inRange(-200, 601);
            return format_exact(static_cast<double>(g) / 100.0);
        }
        case 2: {
            static const std::vector<std::string> junk = {
                "abc", "x", "drizzle", "1.2.3", "nan"};
            return *rc::gen::elementOf(junk);
        }
        default: return std::to_string(*rc::gen::inRange(0, 5));
    }
}

// ──: legacy parity over well-formed known flags ──────
bool run_p1_legacy_parity_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 1: legacy parity over "
        "well-formed known flags (Validates: Requirements 1.3, 13.1, 13.2, "
        "15.1, 15.2)",
        [] {
            const int steps = *rc::gen::inRange(0, 18);

            std::vector<std::string> tokens;
            for (int s = 0; s < steps; ++s) {
                const int kind = *rc::gen::inRange(0, 8);
                switch (kind) {
                    case 0: // boolean toggle / alias member (single token)
                        tokens.emplace_back(*rc::gen::elementOf(toggle_flags()));
                        break;
                    case 1: // string value flag + verbatim value
                        tokens.emplace_back(*rc::gen::elementOf(string_value_flags()));
                        tokens.push_back(gen_word_token());
                        break;
                    case 2: // integer value flag + well-formed int
                        tokens.emplace_back(*rc::gen::elementOf(int_value_flags()));
                        tokens.push_back(std::to_string(*rc::gen::inRange(-1'000'000, 1'000'001)));
                        break;
                    case 3: // float value flag + well-formed float
                        tokens.emplace_back(*rc::gen::elementOf(float_value_flags()));
                        tokens.push_back(gen_float_token());
                        break;
                    case 4: // --time-of-day + well-formed float
                        tokens.emplace_back("--time-of-day");
                        tokens.push_back(gen_float_token());
                        break;
                    case 5: // --api + consumed (no-op) value
                        tokens.emplace_back("--api");
                        tokens.push_back(gen_word_token());
                        break;
                    case 6: // weather value flag + arbitrary family value
                        tokens.emplace_back(*rc::gen::elementOf(weather_value_flags()));
                        tokens.push_back(gen_family_value());
                        break;
                    default: // quality value flag + arbitrary family value
                        tokens.emplace_back(*rc::gen::elementOf(quality_value_flags()));
                        tokens.push_back(gen_family_value());
                        break;
                }
            }

            // Build the shared argv once; feed it to both the parser and oracle.
            std::vector<const char*> argv;
            argv.reserve(tokens.size() + 1);
            argv.push_back("heaven_benchmark");
            for (const std::string& t : tokens) argv.push_back(t.c_str());
            const int argc = static_cast<int>(argv.size());

            const ParseResult r = heaven::parse_cli(argv);
            const BenchConfig oracle = legacy_oracle(argc, argv.data());

            assert_config_equal(r.config, oracle);
            // Help is never requested for a well-formed known-flag line.
            RC_ASSERT(!r.help_requested);
        });
}

// ──: empty command line yields the default configuration ──
bool run_p2_empty_command_line_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 2: empty command line "
        "yields the default configuration (Validates: Requirements 3.8, 6.2, "
        "15.3)",
        [] {
            // Program-name-only argv: no flags at all.
            const char* const argv[] = {"heaven_benchmark"};
            const ParseResult r = heaven::parse_cli(argv);

            const BenchConfig defaults; // default-constructed baseline
            assert_config_equal(r.config, defaults);
            RC_ASSERT(r.diagnostics.empty());
            RC_ASSERT(!r.help_requested);
        });
}

// ── Example / edge tests (deterministic) ────────────────────────
bool run_cli_parity_examples() {
    bool ok = true;
    const BenchConfig defaults;

    auto fail = [&](const char* msg) {
        std::printf("[prop_cli_parity] FAILED: %s\n", msg);
        ok = false;
    };

    // ── --api filament is a recognized no-op  ──
    {
        const ParseResult r = run_parse({"--api", "filament"});
        if (r.config.graphics_api != heaven::GraphicsApi::Filament)
            fail("--api filament should leave graphics_api at Filament (R9.1)");
        if (!r.diagnostics.empty())
            fail("--api filament should emit no diagnostic (R9.2: recognized, "
                 "value consumed)");
        if (r.help_requested)
            fail("--api filament should not request help");
    }

    // ── --help sets help_requested  ──
    {
        const ParseResult r = run_parse({"--help"});
        if (!r.help_requested)
            fail("--help should set help_requested (R12.1)");
        if (!r.diagnostics.empty())
            fail("--help should emit no diagnostic");
    }

    // ── Empty command line yields the default config  ──
    {
        const char* const argv[] = {"heaven_benchmark"};
        const ParseResult r = heaven::parse_cli(argv);
        if (r.config.width != defaults.width ||
            r.config.height != defaults.height ||
            r.config.time_of_day != defaults.time_of_day ||
            r.config.fullscreen != defaults.fullscreen ||
            r.config.benchmark_mode != defaults.benchmark_mode ||
            r.config.graphics_api != defaults.graphics_api)
            fail("empty command line should yield a default BenchConfig (R15.3)");
        if (!r.diagnostics.empty())
            fail("empty command line should emit no diagnostic (R15.3)");
        if (r.help_requested)
            fail("empty command line should not request help");
    }

    // ── Missing trailing value is reported; field unchanged  ──
    {
        const ParseResult r = run_parse({"--width"});
        if (r.diagnostics.size() != 1u ||
            r.diagnostics[0] != "--width: expected a value")
            fail("trailing --width should report '--width: expected a value' "
                 "(R11.1)");
        if (r.config.width != defaults.width)
            fail("trailing --width should leave width at its default (R11.1)");
    }

    // ── Two malformed values produce two reports; fields unchanged  ──
    {
        const ParseResult r = run_parse({"--width", "abc", "--height", "xyz"});
        if (r.diagnostics.size() != 2u)
            fail("two malformed numeric values should produce two diagnostics "
                 "(R11.2)");
        else {
            if (r.diagnostics[0] != "--width: ignoring non-integer value 'abc'")
                fail("first malformed report should name --width and 'abc' (R11.2)");
            if (r.diagnostics[1] != "--height: ignoring non-integer value 'xyz'")
                fail("second malformed report should name --height and 'xyz' (R11.2)");
        }
        if (r.config.width != defaults.width || r.config.height != defaults.height)
            fail("malformed --width/--height should leave both fields at their "
                 "defaults (R11.2)");
    }

    return ok;
}

bool run_cli_parity_properties() {
    bool ok = true;
    ok = run_p1_legacy_parity_property() && ok;
    ok = run_p2_empty_command_line_property() && ok;
    ok = run_cli_parity_examples() && ok;
    return ok;
}

const heaven::testing::Register g_register_cli_parity{&run_cli_parity_properties};

} // namespace
