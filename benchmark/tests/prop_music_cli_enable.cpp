// Feature: background-music-playback,
/*
 * HeavenBench — Property test for the CLI music-enable resolution.
 *
 * Feature: background-music-playback
 *   •: CLI music-enable resolves to the last toggle, else the default
 *
 *
 * The `--music` / `--no-music` flags were added to flag_table() (cli_parser.cpp)
 * as Arity::None toggles that set config.music.enabled to true / false. Because
 * the parse driver applies flags left to right, the LAST music toggle present on
 * the command line wins, with no special casing. When neither toggle appears the
 * field keeps its default of true.
 *
 * This property builds a random command line interleaving three kinds of
 * element:
 *   • `--music`        → expected enabled becomes true
 *   • `--no-music`     → expected enabled becomes false
 *   • an unrelated known flag (a toggle, or a value flag carrying a well-formed
 *     adjacent value) → must not touch config.music.enabled
 *
 * It tracks the expected enabled value by replaying the same left-to-right rule
 * (default true, overwritten by each music toggle in order) and asserts the
 * parsed config.music.enabled equals the effect of the last music toggle (else
 * the default true). Unrelated value flags always carry their value
 * adjacently, so an unrelated token can never land in a value-consuming slot and
 * displace a music toggle.
 *
 * parse_cli is pure (no I/O, no GPU), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/CMakeLists.txt
 * picks it up without editing the entry point. RapidCheck's default of 100
 * successful iterations meets the "≥ 100 iterations" requirement (raise via
 * RC_PARAMS=max_success=…).
 */
#include <cstdio>
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

// Unrelated KNOWN toggles (single token) that must never touch music.enabled.
// None of these is a music flag.
const std::vector<std::string>& unrelated_toggles() {
    static const std::vector<std::string> t = {
        "--fullscreen", "--no-grass", "--hdr", "--benchmark", "--no-sky",
        "--no-god-rays", "--clouds",
    };
    return t;
}

// Unrelated KNOWN value flags carrying a well-formed adjacent value (two
// tokens). The value is adjacent so an unrelated token can never fall into a
// value-consuming slot and swallow a following music toggle.
struct ValueFlag {
    std::string flag;
    std::string value;
};

const std::vector<ValueFlag>& unrelated_value_flags() {
    static const std::vector<ValueFlag> v = {
        {"--width", "1280"},
        {"--height", "720"},
        {"--data", "some_dir"},
        {"--time-of-day", "12.0"},
    };
    return v;
}

// Guard: none of the "unrelated" flags may be a music flag, or the property's
// independence assumption breaks. Also confirm --music / --no-music are real
// table entries.
bool run_unrelated_pool_guard() {
    bool ok = true;
    auto is_music = [](std::string_view n) {
        return n == "--music" || n == "--no-music" || n == "--music-volume";
    };
    for (const std::string& tok : unrelated_toggles()) {
        if (is_music(tok)) {
            std::printf("[prop_music_cli_enable] unrelated toggle '%s' is a music flag\n",
                        tok.c_str());
            ok = false;
        }
    }
    for (const ValueFlag& vf : unrelated_value_flags()) {
        if (is_music(vf.flag)) {
            std::printf("[prop_music_cli_enable] unrelated value flag '%s' is a music flag\n",
                        vf.flag.c_str());
            ok = false;
        }
    }
    bool have_music = false, have_no_music = false;
    for (const heaven::FlagSpec& s : heaven::flag_table()) {
        if (s.name == "--music") have_music = true;
        if (s.name == "--no-music") have_no_music = true;
    }
    if (!have_music || !have_no_music) {
        std::printf("[prop_music_cli_enable] flag_table() is missing --music and/or --no-music\n");
        ok = false;
    }
    return ok;
}

bool run_p3_music_enable_property() {
    return rc::check(
        "Feature: background-music-playback, Property 3: CLI music-enable "
        "resolves to the last toggle, else the default (Validates: Requirements "
        "2.1, 2.2, 2.3, 2.5)",
        [] {
            const int count = *rc::gen::inRange(0, 12);

            std::vector<std::string> tokens;

            // The default for config.music.enabled is true. Replaying the
            // same left-to-right rule the parser uses, each music toggle
            // overwrites the expected value; the last one present therefore wins
            // When no music toggle is generated this stays true.
            bool expected_enabled = true;

            for (int i = 0; i < count; ++i) {
                const int kind = *rc::gen::inRange(0, 4);
                if (kind == 0) {
                    // --music → enable.
                    tokens.emplace_back("--music");
                    expected_enabled = true;
                } else if (kind == 1) {
                    // --no-music → disable.
                    tokens.emplace_back("--no-music");
                    expected_enabled = false;
                } else if (kind == 2) {
                    // Unrelated toggle: must not affect music.enabled.
                    tokens.push_back(*rc::gen::elementOf(unrelated_toggles()));
                } else {
                    // Unrelated value flag + its well-formed adjacent value.
                    const ValueFlag& vf = *rc::gen::elementOf(unrelated_value_flags());
                    tokens.push_back(vf.flag);
                    tokens.push_back(vf.value);
                }
            }

            const ParseResult r = run_parse(tokens);

            // config.music.enabled equals the effect of the last music toggle,
            // else the default true.
            RC_ASSERT(r.config.music.enabled == expected_enabled);
        });
}

bool run_music_cli_enable_properties() {
    bool ok = true;
    ok = run_unrelated_pool_guard() && ok;
    ok = run_p3_music_enable_property() && ok;
    return ok;
}

const heaven::testing::Register g_register_music_cli_enable{
    &run_music_cli_enable_properties};

} // namespace
