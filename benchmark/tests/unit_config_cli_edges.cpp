/*
 * HeavenBench — unit test: CLI config-flag help listing, parse edges,
 * and the CLI-over-default fallback when no config file is named.
 *
 * Feature: benchmark-config-file
 * — Validates:
 *
 * This deterministic unit test pins behaviour the property tests do not cover
 * directly, exercising the PUBLIC parse driver / help generator / overlay:
 *
 *   1. Help listing: generate_help() lists --config and
 *      --write-config (each as a FILE value flag) so a user can discover them.
 *   2. Trailing --config: when --config is the final token it records no
 *      path, emits exactly one "--config: expected a value" diagnostic, and
 *      parsing of earlier valid flags still applies (parsing continued).
 *   3. Empty --config value: an empty value token records no path, emits
 *      "--config: empty path ignored", and a later valid flag still applies.
 *   4. Trailing --write-config: records no path, emits one
 *      "--write-config: expected a value", and earlier flags still apply.
 *   5. --config absent ⇒ CLI-over-default fallback: with no file
 *      loaded, applying the CLI overlay onto a default BenchConfig yields a
 *      configuration equal to the CLI-over-default parse_cli result for the
 *      flags that were set.
 *
 * The exact diagnostic strings asserted here are taken verbatim from
 * cli_parser.cpp's parse driver (the source of truth):
 *   • trailing flag (no value): "<flag>: expected a value"
 *   • empty --config value:     "--config: empty path ignored"
 *
 * parse_cli / generate_help / apply_cli_overlay are pure (no I/O, no GPU), so
 * this runs on the host. The file self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point. This is
 * a deterministic unit test (no random input), so it does not use rc::check.
 */
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/cli_parser.h"
#include "engine/config_file.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
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

// Count how many diagnostics exactly equal `msg`.
std::size_t count_diag(const std::vector<std::string>& diags, std::string_view msg) {
    return static_cast<std::size_t>(
        std::count(diags.begin(), diags.end(), std::string(msg)));
}

bool unit_config_cli_edges() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_config_cli_edges] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    // ── 1. Help listing  ───────────────────────────

    // generate_help() is a one-to-one image of flag_table(), so both config
    // flags must appear. They are FILE value flags, so the help line carries
    // the "FILE" value label too.
    {
        const std::string help = heaven::generate_help();
        if (help.find("--config") == std::string::npos) {
            fail("generate_help() output does not contain '--config'");
        }
        if (help.find("--write-config") == std::string::npos) {
            fail("generate_help() output does not contain '--write-config'");
        }
        // The two flags advertise a FILE value token in the listing.
        if (help.find("--config FILE") == std::string::npos) {
            fail("generate_help() does not list '--config' with a FILE value label");
        }
        if (help.find("--write-config FILE") == std::string::npos) {
            fail("generate_help() does not list '--write-config' with a FILE value label");
        }
    }

    // ── 2. Trailing --config  ────────────────────────────

    // --config as the final token records no path, emits exactly one
    // "--config: expected a value" diagnostic, and does not crash. To prove
    // parsing continues, an earlier valid flag (--width 800) must still apply.
    {
        const ParseResult r = run_parse({"--width", "800", "--config"});
        if (r.config_path.has_value()) {
            fail("trailing --config recorded a config_path (expected none)");
        }
        if (count_diag(r.diagnostics, "--config: expected a value") != 1) {
            fail("trailing --config did not emit exactly one "
                 "'--config: expected a value' diagnostic");
        }
        if (r.config.width != 800) {
            fail("trailing --config aborted parsing: earlier --width 800 was "
                 "not applied");
        }
    }

    // ── 3. Empty --config value  ─────────────────────────

    // An empty value token records no path, emits "--config: empty path
    // ignored", and a later valid flag (--width 640) still applies, proving
    // parsing continued past the empty value.
    {
        const ParseResult r = run_parse({"--config", "", "--width", "640"});
        if (r.config_path.has_value()) {
            fail("empty --config value recorded a config_path (expected none)");
        }
        if (count_diag(r.diagnostics, "--config: empty path ignored") != 1) {
            fail("empty --config value did not emit exactly one "
                 "'--config: empty path ignored' diagnostic");
        }
        if (r.config.width != 640) {
            fail("empty --config value aborted parsing: later --width 640 was "
                 "not applied");
        }
    }

    // ── 4. Trailing --write-config  ──────────────────────

    // --write-config as the final token records no path and emits exactly one
    // "--write-config: expected a value" diagnostic; an earlier valid flag
    // still applies.
    {
        const ParseResult r = run_parse({"--write-config"});
        if (r.write_config_path.has_value()) {
            fail("trailing --write-config recorded a write_config_path "
                 "(expected none)");
        }
        if (count_diag(r.diagnostics, "--write-config: expected a value") != 1) {
            fail("trailing --write-config did not emit exactly one "
                 "'--write-config: expected a value' diagnostic");
        }
    }
    {
        const ParseResult r = run_parse({"--width", "800", "--write-config"});
        if (r.write_config_path.has_value()) {
            fail("trailing --write-config (after a flag) recorded a path");
        }
        if (count_diag(r.diagnostics, "--write-config: expected a value") != 1) {
            fail("trailing --write-config (after a flag) diagnostic count wrong");
        }
        if (r.config.width != 800) {
            fail("trailing --write-config aborted parsing: earlier --width 800 "
                 "was not applied");
        }
    }

    // ── 5. --config absent ⇒ resolved config == CLI-over-default  ──

    // With no --config given, no file is loaded; the effective configuration is
    // the CLI applied over defaults. Replicate that pipeline: start from a
    // default BenchConfig (no file load), parse the CLI, then apply_cli_overlay.
    // The overlaid config must equal the CLI-over-default parse result on the
    // fields the flags set. Pick two non-default values: --width 1234 (default
    // is 1920) and --no-grass (default features.grass is true).
    {
        const ParseResult r = run_parse({"--width", "1234", "--no-grass"});

        // No file was named.
        if (r.config_path.has_value()) {
            fail("scenario 5 unexpectedly recorded a config_path");
        }

        // Effective pipeline with no file: defaults, then CLI overlay.
        BenchConfig cfg = BenchConfig{};
        heaven::apply_cli_overlay(cfg, r);

        // The overlaid config equals the CLI-over-default result on the set
        // fields.
        if (cfg.width != r.config.width) {
            fail("overlaid width does not equal CLI-over-default width");
        }
        if (cfg.features.grass != r.config.features.grass) {
            fail("overlaid features.grass does not equal CLI-over-default value");
        }

        // And both reflect the CLI-supplied values (not the defaults), proving
        // the CLI-over-default result is what the overlay produced.
        if (cfg.width != 1234) {
            fail("overlaid width is not the CLI value 1234");
        }
        if (cfg.features.grass != false) {
            fail("overlaid features.grass is not the CLI value (false)");
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_config_cli_edges{&unit_config_cli_edges};

} // namespace
