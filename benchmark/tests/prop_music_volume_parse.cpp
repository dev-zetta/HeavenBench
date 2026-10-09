// Feature: background-music-playback,: Music volume parse and clamp
/*
 * HeavenBench — Property test for `--music-volume` argument parsing
 *
 * Feature: background-music-playback,: Music volume parse and clamp
 *
 *
 * parse_music_volume(const char*) is the pure (no-I/O) core the CLI funnels
 * the `--music-volume V` argument through. It mirrors parse_rain_intensity
 * exactly, only the retained default differs (1.0, not 0.0). The contract
 * (engine/types.h) has three branches, and this property exercises every one
 * of them with inputs constructed so each branch is reached by construction:
 *
 *   • Numeric in [0, 1]            → accepted as-is: the returned value equals
 *     the parsed number within float tolerance, with clamped == false and
 *     ignored == false (— the supplied value initialises Music_Volume).
 *
 *   • Numeric outside [0, 1]       → pulled to the nearest bound: a value below
 *     0 returns exactly 0.0 and a value above 1 returns exactly 1.0, with
 *     clamped == true and ignored == false (— out-of-range clamps to the
 *     nearest bound). Non-finite values and numeric overflow are rejected by
 *     the shared strict parser.
 *
 *   • Non-numeric / junk / empty / null → ignored: the parser retains the
 *     default 1.0 with ignored == true and clamped == false (— an
 *     unparseable argument is rejected and Music_Volume stays at its default).
 *     This includes trailing-garbage tokens like "0.5abc", non-finite values,
 *     hexadecimal floats, the empty string, and a null pointer.
 *
 * Junk strings are generated so they can be classified independently of the
 * function under test (no circularity): a curated list of structured edge
 * cases plus a generator that prepends a character the strict parser can
 * never start a number with. The whole token is therefore junk regardless of
 * its tail, guaranteeing the Ignored expectation independently.
 *
 * The mapping is pure (engine/types.h is header-only, GLM-only, no GPU deps),
 * so it runs on the host. This translation unit self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point; no second main() is defined.
 */
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

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

// The retained Music_Volume default for the ignored case. Out-of-range
// tokens never fall back to this — they clamp to the nearest bound instead.
constexpr float k_default_volume = 1.0f;

// What the parser is expected to do with a given input string. Each generated
// case carries its own independently-derived expectation, so the assertion is
// a direct comparison against the contract rather than against a re-derivation
// of the parser's own logic.
enum class Kind { Accepted, Clamped, Ignored };

struct Case {
    std::string text;            // the argument text (ignored when is_null)
    bool        is_null = false; // pass nullptr instead of text.c_str()
    Kind        kind    = Kind::Ignored;
    float       expected = k_default_volume; // Accepted: parsed value;
                                             // Clamped: 0 or 1; Ignored: 1.0
};

// Format a double exactly: "%.17g" is enough to round-trip any IEEE-754
// double, so the (float) cast the parser applies lands
// on the same float we computed for the expectation.
std::string format_exact(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return {buf};
}

// Numeric strings whose value lies in [0, 1] → Accepted as-is. An integer grid
// over [0, 1e6] maps onto the closed unit interval (0.0 and 1.0 exactly
// reachable, interior densely covered).
rc::Gen<Case> accepted_grid_gen() {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001), [](int g) {
        const double d = static_cast<double>(g) / 1'000'000.0;
        return Case{format_exact(d), false, Kind::Accepted, static_cast<float>(d)};
    });
}

// Hand-picked accepted forms covering numeric spellings the grid does not emit.
// The shared strict parser deliberately trims leading ASCII whitespace and
// accepts one explicit leading plus.
rc::Gen<Case> accepted_forms_gen() {
    return rc::gen::element<Case>(
        Case{"0",       false, Kind::Accepted, 0.0f},
        Case{"1",       false, Kind::Accepted, 1.0f},
        Case{"0.0",     false, Kind::Accepted, 0.0f},
        Case{"1.0",     false, Kind::Accepted, 1.0f},
        Case{".5",      false, Kind::Accepted, 0.5f},
        Case{"+0.25",   false, Kind::Accepted, 0.25f},
        Case{"5e-1",    false, Kind::Accepted, 0.5f},
        Case{"1e0",     false, Kind::Accepted, 1.0f},
        Case{"1.",      false, Kind::Accepted, 1.0f},
        Case{"  0.75",  false, Kind::Accepted, 0.75f});
}

// Numeric strings < 0 → clamped to exactly 0.0 (clamped flag set).
rc::Gen<Case> clamped_low_gen() {
    return rc::gen::map(rc::gen::inRange<int>(-1'000'000, 0), [](int g) {
        const double d = static_cast<double>(g) / 1'000.0; // [-1000.0, -0.001]
        return Case{format_exact(d), false, Kind::Clamped, 0.0f};
    });
}

// Numeric strings > 1 → clamped to exactly 1.0 (clamped flag set).
rc::Gen<Case> clamped_high_gen() {
    return rc::gen::map(rc::gen::inRange<int>(1'000'001, 100'000'001), [](int g) {
        const double d = static_cast<double>(g) / 1'000'000.0; // [1.000001, 100.0]
        return Case{format_exact(d), false, Kind::Clamped, 1.0f};
    });
}

// Finite out-of-range values clamp. Non-finite values and numeric overflow are
// strict-parse failures and therefore take the ignored/default path.
rc::Gen<Case> clamped_edges_gen() {
    return rc::gen::element<Case>(
        Case{"-0.0001",   false, Kind::Clamped, 0.0f},
        Case{"1.0001",    false, Kind::Clamped, 1.0f},
        Case{"-100",      false, Kind::Clamped, 0.0f},
        Case{"42",        false, Kind::Clamped, 1.0f},
        Case{"inf",       false, Kind::Ignored, k_default_volume},
        Case{"+inf",      false, Kind::Ignored, k_default_volume},
        Case{"Infinity",  false, Kind::Ignored, k_default_volume},
        Case{"1e400",     false, Kind::Ignored, k_default_volume},
        Case{"-inf",      false, Kind::Ignored, k_default_volume},
        Case{"-Infinity", false, Kind::Ignored, k_default_volume},
        Case{"-1e400",    false, Kind::Ignored, k_default_volume});
}

// Curated junk: structured tokens that strict parsing rejects (trailing garbage,
// stray punctuation, incomplete exponent, double sign), the NaN spellings the
// explicit isnan guard rejects, the empty/whitespace inputs, and the null
// pointer.
rc::Gen<Case> junk_curated_gen() {
    return rc::gen::element<Case>(
        Case{"",        false, Kind::Ignored, k_default_volume},
        Case{"abc",     false, Kind::Ignored, k_default_volume},
        Case{"0.5abc",  false, Kind::Ignored, k_default_volume}, // trailing garbage
        Case{"1.0 ",    false, Kind::Ignored, k_default_volume}, // trailing space
        Case{"1,5",     false, Kind::Ignored, k_default_volume}, // comma decimal sep
        Case{"--1",     false, Kind::Ignored, k_default_volume},
        Case{".",       false, Kind::Ignored, k_default_volume},
        Case{"+",       false, Kind::Ignored, k_default_volume},
        Case{"-",       false, Kind::Ignored, k_default_volume},
        Case{"e5",      false, Kind::Ignored, k_default_volume}, // exponent, no mantissa
        Case{"1.0e",    false, Kind::Ignored, k_default_volume}, // dangling exponent
        Case{"0x",      false, Kind::Ignored, k_default_volume}, // hex prefix, no digits
        Case{"0x1p-1",  false, Kind::Ignored, k_default_volume}, // strict parser rejects hex floats
        Case{"1 2",     false, Kind::Ignored, k_default_volume},
        Case{"   ",     false, Kind::Ignored, k_default_volume}, // whitespace only
        Case{"nan",     false, Kind::Ignored, k_default_volume}, // rejected by isnan guard
        Case{"NaN",     false, Kind::Ignored, k_default_volume},
        Case{"-nan",    false, Kind::Ignored, k_default_volume},
        Case{"hello",   false, Kind::Ignored, k_default_volume},
        Case{"",        true,  Kind::Ignored, k_default_volume}); // null pointer
}

// Generated junk: a leading character strict parsing can never begin a number
// with, followed by an arbitrary tail. The whole token is junk no matter the
// tail, so the Ignored
// expectation is sound without consulting the parser. The leading set excludes
// digits, sign, dot, whitespace, and 'i'/'I'/'n'/'N' (which start inf/nan).
rc::Gen<Case> junk_generated_gen() {
    const std::string lead_chars =
        "abcdfghjklmopqrstuvwxyzABCDFGHJKLMOPQRSTUVWXYZ@#$%^&*?/\\<>~`";
    const std::string tail_chars =
        "abcdefghijklmnopqrstuvwxyz0123456789 .,+-eE!@#$%^&*()";

    return rc::gen::map(
        rc::gen::pair(rc::gen::elementOf(lead_chars),
                      rc::gen::container<std::string>(rc::gen::elementOf(tail_chars))),
        [](const std::pair<char, std::string>& p) {
            std::string s;
            s.push_back(p.first);
            s += p.second;
            return Case{std::move(s), false, Kind::Ignored, k_default_volume};
        });
}

bool run_music_volume_parse_property() {
    // RapidCheck's default is 100 successful iterations (maxSuccess = 100),
    // which meets the "≥ 100 iterations" requirement; raise via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: background-music-playback, Property 1: Music volume parse and "
        "clamp (Validates: Requirements 3.1, 3.2, 3.4, 3.5)",
        [] {
            // Weight the three branches roughly evenly; the accepted and junk
            // branches each blend a dense grid/generator with explicit edges.
            const Case c = *rc::gen::weightedOneOf<Case>({
                {3, accepted_grid_gen()},
                {1, accepted_forms_gen()},
                {2, clamped_low_gen()},
                {2, clamped_high_gen()},
                {1, clamped_edges_gen()},
                {2, junk_curated_gen()},
                {2, junk_generated_gen()},
            });

            const char* arg = c.is_null ? nullptr : c.text.c_str();
            const heaven::MusicVolumeParse r = heaven::parse_music_volume(arg);

            // The returned value is always a usable Music_Volume in [0, 1],
            // whatever the branch (it is what initialises the parameter).
            RC_ASSERT(r.value >= 0.0f && r.value <= 1.0f);

            // The two result flags are mutually exclusive by contract: a case
            // is accepted, clamped, or ignored — never both clamped and ignored.
            RC_ASSERT(!(r.clamped && r.ignored));

            switch (c.kind) {
                case Kind::Accepted:
                    // Round-trips the in-range value, reports acceptance.
                    RC_ASSERT(!r.ignored);
                    RC_ASSERT(!r.clamped);
                    RC_ASSERT(std::fabs(r.value - c.expected) <= 1e-6f);
                    break;

                case Kind::Clamped:
                    // Numeric-out-of-range pulled to the nearest bound, flagged
                    // never falls back to the default.
                    RC_ASSERT(!r.ignored);
                    RC_ASSERT(r.clamped);
                    RC_ASSERT(r.value == c.expected); // exactly 0.0 or 1.0
                    break;

                case Kind::Ignored:
                    // Non-numeric input retains the default 1.0 and reports
                    // ignored; the clamp flag stays clear.
                    RC_ASSERT(r.ignored);
                    RC_ASSERT(!r.clamped);
                    RC_ASSERT(r.value == k_default_volume);
                    break;
            }
        });
}

const heaven::testing::Register g_register_music_volume_parse{
    &run_music_volume_parse_property};

} // namespace
