/*
 * HeavenBench — unit test: --music-volume CLI diagnostics
 *
 * — Validates:
 *
 * The `--music-volume V` value flag is parsed by heaven::parse_cli, which
 * returns a ParseResult carrying both the resulting BenchConfig and the
 * left-to-right diagnostics vector. The flag's handler (engine/cli_parser.cpp)
 * delegates the numeric parse/clamp/reject to the pure parse_music_volume
 * helper and owns ONLY the diagnostic text; the "missing value" case is handled
 * by the generic value-flag driver. This test pins one example per path,
 * asserting on the exact strings the implementation produces today:
 *
 *   • In-range value: `--music-volume 0.5` ⇒ music.volume == 0.5 and NO
 *     diagnostic is emitted for the flag.
 *   • Out-of-range value: `--music-volume 2.0` ⇒ music.volume clamped to
 *     1.0 with a warning naming the original "2.0" and the clamped value; the
 *     below-zero `--music-volume -0.5` ⇒ clamped to 0.0 with the matching
 *     warning.
 *   • Non-numeric value: `--music-volume loud` ⇒ default 1.0 retained
 *     with a warning naming the rejected token.
 *   • Missing value: `--music-volume` as the final token ⇒ the generic
 *     "expected a value" diagnostic and music.volume left at its default 1.0.
 *
 * The baseline for "default" is a default-constructed BenchConfig
 * (music.volume == 1.0). parse_cli is pure (no I/O, no GPU), so this runs on
 * the host. The file self-registers with the shared `heaven_benchmark_tests`
 * harness (test_main.cpp) at static-init time via heaven::testing::Register,
 * following the same pattern as the other unit translation units; the
 * registered callable returns true on success and false (after printing a
 * diagnostic) on failure. It is picked up by the tests glob with no CMake edit.
 */
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "engine/cli_parser.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// is a free function with external linkage defined there; the Register helper's
// definition is identical so it can be reused from this translation unit.
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
// as in a real launch), exactly as prop_cli_diagnostics.cpp does.
ParseResult run_parse(const std::vector<std::string>& tokens) {
    std::vector<const char*> argv;
    argv.reserve(tokens.size() + 1);
    argv.push_back("heaven_benchmark");
    for (const std::string& t : tokens) argv.push_back(t.c_str());
    return heaven::parse_cli(argv);
}

bool unit_music_volume_cli() {
    bool ok = true;

    // The default Music_Volume on a fresh BenchConfig is the single source of
    // truth for the "retained default" assertions (1.0).
    const BenchConfig def;

    // ── In-range value set silently  ─────────────────────
    // A well-formed decimal inside [0,1] is stored verbatim and the handler
    // emits no diagnostic.
    {
        const ParseResult r = run_parse({"--music-volume", "0.5"});
        if (std::fabs(r.config.music.volume - 0.5f) > 1e-6f) {
            std::printf("[unit_music_volume_cli] FAILED: --music-volume 0.5 ⇒ "
                        "%.9g, expected 0.5\n",
                        static_cast<double>(r.config.music.volume));
            ok = false;
        }
        if (!r.diagnostics.empty()) {
            std::printf("[unit_music_volume_cli] FAILED: --music-volume 0.5 emitted "
                        "%zu diagnostic(s), expected none (first: '%s')\n",
                        r.diagnostics.size(),
                        r.diagnostics.empty() ? "" : r.diagnostics[0].c_str());
            ok = false;
        }
    }

    // ── Out-of-range value clamped with a warning  ───────
    // Above the upper bound clamps to 1.0; the warning names the original token
    // "2.0" and the clamped value formatted with two decimals.
    {
        const ParseResult r = run_parse({"--music-volume", "2.0"});
        if (std::fabs(r.config.music.volume - 1.0f) > 1e-6f) {
            std::printf("[unit_music_volume_cli] FAILED: --music-volume 2.0 ⇒ "
                        "%.9g, expected clamp to 1.0\n",
                        static_cast<double>(r.config.music.volume));
            ok = false;
        }
        const std::string expected =
            "WARN: --music-volume '2.0' out of [0,1]; clamped to 1.00";
        if (r.diagnostics.size() != 1u || r.diagnostics[0] != expected) {
            std::printf("[unit_music_volume_cli] FAILED: --music-volume 2.0 "
                        "diagnostics mismatch; expected exactly one '%s', got %zu "
                        "(first: '%s')\n",
                        expected.c_str(), r.diagnostics.size(),
                        r.diagnostics.empty() ? "" : r.diagnostics[0].c_str());
            ok = false;
        }
    }
    {
        // Below the lower bound clamps to 0.0 with the matching warning.
        const ParseResult r = run_parse({"--music-volume", "-0.5"});
        if (std::fabs(r.config.music.volume - 0.0f) > 1e-6f) {
            std::printf("[unit_music_volume_cli] FAILED: --music-volume -0.5 ⇒ "
                        "%.9g, expected clamp to 0.0\n",
                        static_cast<double>(r.config.music.volume));
            ok = false;
        }
        const std::string expected =
            "WARN: --music-volume '-0.5' out of [0,1]; clamped to 0.00";
        if (r.diagnostics.size() != 1u || r.diagnostics[0] != expected) {
            std::printf("[unit_music_volume_cli] FAILED: --music-volume -0.5 "
                        "diagnostics mismatch; expected exactly one '%s', got %zu "
                        "(first: '%s')\n",
                        expected.c_str(), r.diagnostics.size(),
                        r.diagnostics.empty() ? "" : r.diagnostics[0].c_str());
            ok = false;
        }
    }

    // ── Non-numeric value rejected, default 1.0 retained  ─
    // A token that is not a valid decimal is ignored: the field keeps its 1.0
    // default and the warning names the rejected token.
    {
        const ParseResult r = run_parse({"--music-volume", "loud"});
        if (std::fabs(r.config.music.volume - def.music.volume) > 1e-6f) {
            std::printf("[unit_music_volume_cli] FAILED: --music-volume loud ⇒ "
                        "%.9g, expected retained default %.9g\n",
                        static_cast<double>(r.config.music.volume),
                        static_cast<double>(def.music.volume));
            ok = false;
        }
        const std::string expected =
            "WARN: --music-volume 'loud' is not a number; using default 1.0";
        if (r.diagnostics.size() != 1u || r.diagnostics[0] != expected) {
            std::printf("[unit_music_volume_cli] FAILED: --music-volume loud "
                        "diagnostics mismatch; expected exactly one '%s', got %zu "
                        "(first: '%s')\n",
                        expected.c_str(), r.diagnostics.size(),
                        r.diagnostics.empty() ? "" : r.diagnostics[0].c_str());
            ok = false;
        }
    }

    // ── Missing value: --music-volume as the final token  ─
    // The generic value-flag driver emits "expected a value" and leaves the
    // field at its default 1.0 (no handler call).
    {
        const ParseResult r = run_parse({"--music-volume"});
        if (std::fabs(r.config.music.volume - def.music.volume) > 1e-6f) {
            std::printf("[unit_music_volume_cli] FAILED: trailing --music-volume ⇒ "
                        "%.9g, expected retained default %.9g\n",
                        static_cast<double>(r.config.music.volume),
                        static_cast<double>(def.music.volume));
            ok = false;
        }
        const std::string expected = "--music-volume: expected a value";
        if (r.diagnostics.size() != 1u || r.diagnostics[0] != expected) {
            std::printf("[unit_music_volume_cli] FAILED: trailing --music-volume "
                        "diagnostics mismatch; expected exactly one '%s', got %zu "
                        "(first: '%s')\n",
                        expected.c_str(), r.diagnostics.size(),
                        r.diagnostics.empty() ? "" : r.diagnostics[0].c_str());
            ok = false;
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_music_volume_cli{unit_music_volume_cli};

} // namespace
