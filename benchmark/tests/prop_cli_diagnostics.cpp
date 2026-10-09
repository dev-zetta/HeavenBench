// Feature: cli-argument-parser-refactor,  &  &
/*
 * HeavenBench — Property tests for the CLI parser's diagnostic reporting
 * (unknown / missing / malformed) and for the help text being a one-to-one
 * image of the registration table.
 *
 * Feature: cli-argument-parser-refactor
 *   •: Unknown-flag reporting without aborting
 *
 *   •: Missing/malformed value leaves the field unchanged
 *
 *   •: Help is a one-to-one image of the table
 *
 *
 * parse_cli and generate_help are fully implemented at this point in the
 * refactor, so these properties exercise the PUBLIC entry points directly:
 * they build a real argv, call heaven::parse_cli(argc, argv), and assert on the
 * resulting BenchConfig / diagnostics (and on the generate_help() string).
 *
 * (unknown-flag reporting):
 *   A random command line interleaves three kinds of element — a known toggle,
 *   a known value flag carrying a well-formed value (including --api, whose
 *   value is consumed as a no-op), and an arbitrary token that matches no table
 *   entry. The parser must emit exactly one "unknown option '<token>'"
 *   diagnostic per unknown element (in left-to-right order), still apply every
 *   known flag, and — when no unknown element is present — emit no diagnostic
 *   at all. Because every known element is well-formed, the
 *   ONLY diagnostics produced are the unknown-flag reports, so the diagnostics
 *   vector equals the expected unknown-message vector exactly. Known value
 *   flags always carry their value adjacently, so an unknown token can never
 *   land in a value-consuming position (the --api value, in particular, is
 *   never reported).
 *
 * (missing / malformed value, non-family numeric flags):
 *   For each non-weather/non-quality numeric value flag, two cases:
 *     • missing — the flag is the final token: parse_cli emits
 *       "<flag>: expected a value" and the target field stays at its default.
 *     • malformed — a non-numeric token follows: parse_cli emits
 *       "<flag>: ignoring non-integer value '<tok>'" (int flags) or
 *       "<flag>: ignoring non-number value '<tok>'" (float flags) and the
 *       target field stays at its default.
 *   The "default" baseline is a default-constructed BenchConfig, so an
 *   unchanged field is verified field-by-field.
 *
 * (help is a one-to-one image of the table):
 *   generate_help() emits exactly one line per FlagSpec in flag_table(), in
 *   table order, and no line for any flag absent from the table. Verified by
 *   splitting the output into lines, asserting the line count equals the table
 *   size, and asserting line i begins with the formatted prefix for table
 *   entry i ("  <name> "). parse_cli over {"prog", "--help"} sets
 *   help_requested == true (and applies nothing else).
 *
 * parse_cli / generate_help are pure (no I/O, no GPU), so this runs on the
 * host. The file self-registers with the RapidCheck harness in test_main.cpp
 * via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidcheck.h>

#include "engine/cli_parser.h"

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

// Run the real parse driver over a token list (argv[0] is a dummy program name,
// as in a real launch).
ParseResult run_parse(const std::vector<std::string>& tokens) {
    std::vector<const char*> argv;
    argv.reserve(tokens.size() + 1);
    argv.push_back("heaven_benchmark");
    for (const std::string& t : tokens) argv.push_back(t.c_str());
    return heaven::parse_cli(argv);
}

// ──: unknown-flag reporting without aborting ─────────

// A known toggle: its flag name plus a predicate confirming the parsed config
// shows the toggle was applied.
struct KnownToggle {
    std::string                              flag;
    std::function<bool(const BenchConfig&)>  applied;
};

const std::vector<KnownToggle>& known_toggles() {
    static const std::vector<KnownToggle> t = {
        {"--fullscreen", [](const BenchConfig& c) { return c.fullscreen == true; }},
        {"--no-grass",   [](const BenchConfig& c) { return c.features.grass == false; }},
        {"--hdr",        [](const BenchConfig& c) { return c.hdr == true; }},
        {"--benchmark",  [](const BenchConfig& c) { return c.benchmark_mode == true; }},
        {"--no-sky",     [](const BenchConfig& c) { return c.features.sky == false; }},
    };
    return t;
}

// A known value flag carrying a well-formed value: the flag, the value token,
// and a predicate confirming it was applied (or, for --api, that it stayed a
// no-op). The value tokens are deliberately non-flag-like so they are never
// themselves resolved as flags.
struct KnownValueFlag {
    std::string                              flag;
    std::string                              value;
    std::function<bool(const BenchConfig&)>  applied;
};

const std::vector<KnownValueFlag>& known_value_flags() {
    static const std::vector<KnownValueFlag> v = {
        {"--width", "1234", [](const BenchConfig& c) { return c.width == 1234; }},
        {"--data",  "some_dir", [](const BenchConfig& c) { return c.data_dir == "some_dir"; }},
        // --api consumes its value as a no-op: graphics_api stays Filament and
        // the consumed value is never reported as an unknown flag.
        {"--api",   "vulkan", [](const BenchConfig& c) {
            return c.graphics_api == heaven::GraphicsApi::Filament; }},
    };
    return v;
}

// Tokens guaranteed to match no entry in flag_table(). Verified against the
// live table by the guard below so this pool cannot silently collide with a
// real flag added later.
const std::vector<std::string>& unknown_tokens() {
    static const std::vector<std::string> u = {
        "--bogus", "--xyz", "--not-a-flag", "--qqq", "--no-such-flag",
        "-x", "--zzz123", "--api-typo",
    };
    return u;
}

// Guard: the unknown-token pool must contain nothing that resolves to a real
// flag, or the property's "unknown" assumption breaks.
bool run_unknown_pool_guard() {
    bool ok = true;
    for (const std::string& tok : unknown_tokens()) {
        for (const heaven::FlagSpec& s : heaven::flag_table()) {
            if (s.name == tok) {
                std::printf("[prop_cli_diagnostics] unknown-token pool entry '%s' "
                            "collides with a real flag in the table\n",
                            tok.c_str());
                ok = false;
            }
        }
    }
    return ok;
}

bool run_p9_unknown_reporting_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 9: unknown-flag "
        "reporting without aborting (Validates: Requirements 9.2, 10.1, 10.2, "
        "10.3)",
        [] {
            const int count = *rc::gen::inRange(0, 10);

            std::vector<std::string> tokens;
            std::vector<std::string> expected_diags;        // in emission order
            std::vector<std::function<bool(const BenchConfig&)>> applied_checks;

            for (int i = 0; i < count; ++i) {
                const int kind = *rc::gen::inRange(0, 3);
                if (kind == 0) {
                    // Known toggle (single token).
                    const KnownToggle& tg = *rc::gen::elementOf(known_toggles());
                    tokens.push_back(tg.flag);
                    applied_checks.push_back(tg.applied);
                } else if (kind == 1) {
                    // Known value flag + its well-formed value (two tokens). The
                    // value is adjacent so an unknown token can never fall into
                    // a value-consuming slot.
                    const KnownValueFlag& vf = *rc::gen::elementOf(known_value_flags());
                    tokens.push_back(vf.flag);
                    tokens.push_back(vf.value);
                    applied_checks.push_back(vf.applied);
                } else {
                    // An unknown token: reported exactly once, parsing continues.
                    const std::string& tok = *rc::gen::elementOf(unknown_tokens());
                    tokens.push_back(tok);
                    expected_diags.push_back(std::format("unknown option '{}'", tok));
                }
            }

            const ParseResult r = run_parse(tokens);

            // Exactly one diagnostic per unknown token, in left-to-right order,
            // and NOTHING else (every known element is well-formed). When no
            // unknown token is present this asserts an empty diagnostics vector

            RC_ASSERT(r.diagnostics == expected_diags);

            // Every known flag was still applied despite any interleaved unknowns
            // (— parsing continued).
            for (const auto& check : applied_checks) {
                RC_ASSERT(check(r.config));
            }

            // No unknown token is a value flag, so help is never requested here.
            RC_ASSERT(!r.help_requested);
        });
}

// ──: missing / malformed value leaves field unchanged ─

// One non-weather/non-quality numeric value flag: its name, whether it parses
// as an integer (vs. a float — selects the malformed-message wording), and a
// predicate confirming the target field still holds its default value.
struct NumericValueFlag {
    std::string                              flag;
    bool                                     is_int;     // true → "non-integer", false → "non-number"
    std::function<bool(const BenchConfig&)>  at_default; // target field unchanged
};

const std::vector<NumericValueFlag>& numeric_value_flags() {
    // Defaults come from a default-constructed BenchConfig (width=1920,
    // height=1080, cloth_sim_hz=60, tessellation.scale=0.7/factor=0.8/
    // distance=50, hdr_peak_nits=1000, hdr_paper_white_nits=400,
    // time_of_day=18.0). Each predicate compares the parsed field to the
    // matching default-constructed field so the baseline cannot drift.
    static const std::vector<NumericValueFlag> v = [] {
        const BenchConfig d; // defaults
        std::vector<NumericValueFlag> out;
        out.push_back({"--width", true,
            [value = d.width](const BenchConfig& c) { return c.width == value; }});
        out.push_back({"--height", true,
            [value = d.height](const BenchConfig& c) { return c.height == value; }});
        out.push_back({"--cloth-rate", false,
            [value = d.cloth_sim_hz](const BenchConfig& c) { return c.cloth_sim_hz == value; }});
        out.push_back({"--tess-scale", false,
            [value = d.tessellation.scale](const BenchConfig& c) { return c.tessellation.scale == value; }});
        out.push_back({"--tess-factor", false,
            [value = d.tessellation.factor](const BenchConfig& c) { return c.tessellation.factor == value; }});
        out.push_back({"--tess-distance", false,
            [value = d.tessellation.distance](const BenchConfig& c) { return c.tessellation.distance == value; }});
        out.push_back({"--hdr-nits", false,
            [value = d.hdr_peak_nits](const BenchConfig& c) { return c.hdr_peak_nits == value; }});
        out.push_back({"--hdr-paper-white", false,
            [value = d.hdr_paper_white_nits](const BenchConfig& c) { return c.hdr_paper_white_nits == value; }});
        out.push_back({"--time-of-day", false,
            [value = d.time_of_day](const BenchConfig& c) { return c.time_of_day == value; }});
        return out;
    }();
    return v;
}

// Non-numeric tokens rejected by BOTH parse_int_strict and parse_float_strict
// (so they exercise the malformed path for int and float flags alike). None can
// begin/extend a number, and none is a flag name.
const std::vector<std::string>& malformed_tokens() {
    static const std::vector<std::string> j = {
        "abc", "xyz", "px", "foo", "bar", "unit", "1.2.3", "12ab", "e5", "deg",
    };
    return j;
}

bool run_p10_missing_malformed_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 10: missing/malformed "
        "value leaves the field unchanged (Validates: Requirements 11.1, 11.2)",
        [] {
            const NumericValueFlag& nf = *rc::gen::elementOf(numeric_value_flags());

            // ── Missing value: the flag is the final token  ──
            {
                const ParseResult r = run_parse({nf.flag});
                const std::string expected =
                    std::format("{}: expected a value", nf.flag);
                RC_ASSERT(r.diagnostics.size() == 1u);
                RC_ASSERT(r.diagnostics[0] == expected);
                // Target field stays at its default.
                RC_ASSERT(nf.at_default(r.config));
            }

            // ── Malformed value: a non-numeric token follows  ──
            {
                const std::string tok = *rc::gen::elementOf(malformed_tokens());
                const ParseResult r = run_parse({nf.flag, tok});
                const std::string expected = nf.is_int
                    ? std::format("{}: ignoring non-integer value '{}'", nf.flag, tok)
                    : std::format("{}: ignoring non-number value '{}'", nf.flag, tok);
                RC_ASSERT(r.diagnostics.size() == 1u);
                RC_ASSERT(r.diagnostics[0] == expected);
                // Target field stays at its default (the junk was rejected).
                RC_ASSERT(nf.at_default(r.config));
            }
        });
}

// ──: help is a one-to-one image of the table ────────

// Deterministic (no random input): generate_help() must emit exactly one line
// per FlagSpec in flag_table(), in table order, with each line beginning with
// the formatted prefix for its entry ("  <name> "). Because generate_help only
// iterates the table, equal line/entry counts also proves no line exists for a
// flag absent from the table.
bool run_p11_help_one_to_one() {
    std::printf("[prop_cli_diagnostics] Feature: cli-argument-parser-refactor, "
                "Property 11: help is a one-to-one image of the table "
                "(Validates: Requirements 1.4, 12.1, 12.2, 12.3)\n");

    const std::vector<heaven::FlagSpec>& table = heaven::flag_table();
    const std::string help = heaven::generate_help();

    // Split the help text into lines on '\n'. Each generated line ends with a
    // newline, so the final split segment is empty and is dropped.
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : help) {
        if (ch == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    bool ok = true;
    if (!cur.empty()) {
        std::printf("[prop_cli_diagnostics] help text does not end with a newline; "
                    "trailing fragment '%s'\n", cur.c_str());
        ok = false;
    }

    // Exactly one line per table entry.
    if (lines.size() != table.size()) {
        std::printf("[prop_cli_diagnostics] help line count %zu != table size %zu\n",
                    lines.size(), table.size());
        ok = false;
    }

    // Line i is the image of table entry i: it begins with "  <name> ".
    const std::size_t n = std::min(lines.size(), table.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::string prefix =
            std::string("  ") + std::string(table[i].name) + " ";
        if (!lines[i].starts_with(prefix)) {
            std::printf("[prop_cli_diagnostics] help line %zu '%s' does not begin "
                        "with expected prefix '%s'\n",
                        i, lines[i].c_str(), prefix.c_str());
            ok = false;
        }
    }

    // parse_cli over --help sets help_requested and applies nothing else

    const ParseResult r = run_parse({"--help"});
    if (!r.help_requested) {
        std::printf("[prop_cli_diagnostics] parse_cli({--help}) did not set "
                    "help_requested\n");
        ok = false;
    }
    if (!r.diagnostics.empty()) {
        std::printf("[prop_cli_diagnostics] parse_cli({--help}) emitted "
                    "diagnostics unexpectedly\n");
        ok = false;
    }

    return ok;
}

bool run_cli_diagnostics_properties() {
    bool ok = true;
    ok = run_unknown_pool_guard() && ok;
    ok = run_p9_unknown_reporting_property() && ok;
    ok = run_p10_missing_malformed_property() && ok;
    ok = run_p11_help_one_to_one() && ok;
    return ok;
}

const heaven::testing::Register g_register_cli_diagnostics{
    &run_cli_diagnostics_properties};

} // namespace
