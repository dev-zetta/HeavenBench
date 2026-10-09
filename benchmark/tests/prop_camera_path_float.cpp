// Feature: codebase-hardening,
/*
 * HeavenBench — Property test for non-throwing float parsing in the
 * camera-path loader (engine/camera_path.cpp).
 *
 * Feature: codebase-hardening
 *   •: Non-throwing float parse
 *
 *
 * replaces the five throwing std::stof sites in
 * camera_path.cpp with a non-throwing locale-independent numeric helper:
 *
 *     [[nodiscard]] float parse_float_or(const std::string& s, float fallback);
 *
 * That helper lives in an ANONYMOUS namespace inside camera_path.cpp and is
 * not exported via engine/camera_path.h, so it cannot be linked directly from
 * this translation unit. To pin its documented contract we replicate the EXACT
 * implementation here as a local oracle (kept byte-for-byte in step with
 * camera_path.cpp::parse_float_or — prefix parsing with fallback)
 * and assert the two invariants the implementation guarantees:
 *
 *   • PROPERTY A (— non-throwing): for ANY input string and any
 *     fallback, parse_float_or returns WITHOUT throwing. This is the whole
 *     point of moving off std::stof, which throws std::invalid_argument on a
 *     field with no numeric prefix and std::out_of_range on overflow. The test
 *     also pins the malformed/empty contract by example: where std::stof would
 *     throw std::invalid_argument (empty / no numeric prefix), parse_float_or
 *     instead yields the caller's fallback.
 *
 *   • PROPERTY B (— behavioural parity): for any WELL-FORMED float
 *     string, parse_float_or parses the same finite value as the shared strict
 *     parser.
 *
 * The helper is pure (no I/O, no GPU), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/
 * CMakeLists.txt picks it up without editing the entry point. RapidCheck's
 * default of 100 successful iterations meets the "≥ 100 iterations"
 * requirement (raise via RC_PARAMS=max_success=…).
 */
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "engine/number_parse.h"

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

// ── Oracle for camera_path.cpp::parse_float_or ──────────────────────────────
[[nodiscard]] float parse_float_or(const std::string& s, float fallback) {
    return heaven::parse_number_prefix<float>(s).value_or(fallback);
}

// ── Property A: arbitrary input never throws  ───────────────

// For any string and any fallback the helper completes normally. We additionally
// pin the malformed/empty contract: where std::stof would throw
// std::invalid_argument (no parseable numeric prefix), parse_float_or returns
// the fallback unchanged.
bool run_p20a_non_throwing_property() {
    return rc::check(
        "Feature: codebase-hardening, Property 20: non-throwing float parse — "
        "arbitrary input never throws (Validates: Requirements 15.1, 15.2)",
        [] {
            const std::string s = *rc::gen::arbitrary<std::string>();
            const float fallback = *rc::gen::element(
                0.0f, 1.0f, -1.0f, 60.0f, 18.0f);

            // The core guarantee: no exception escapes for ANY input. std::stof
            // would throw here for malformed/empty/overflowing fields.
            float parsed = 0.0f;
            bool threw = false;
            try {
                parsed = parse_float_or(s, fallback);
            } catch (...) {
                threw = true;
            }
            RC_ASSERT(!threw);

            if (!heaven::parse_number_prefix<float>(s)) {
                RC_ASSERT(parsed == fallback);
            }
        });
}

// ── Property B: well-formed input parses identically to strict parsing ───────

// Generate a well-formed float string by formatting a finite float, then assert
// parse_float_or agrees with the shared strict parser on it.
bool run_p20b_wellformed_parity_property() {
    return rc::check(
        "Feature: codebase-hardening, Property 20: non-throwing float parse — "
        "well-formed input parses identically to strict parsing "
        "(Validates: Requirements 15.3)",
        [] {
            const float v = *rc::gen::arbitrary<float>();
            // Restrict to finite values so the formatted string is a plain
            // well-formed float literal (no "inf"/"nan") and equality is
            // well-defined.
            RC_PRE(std::isfinite(v));

            const std::string s = std::to_string(v);

            const auto reference = heaven::parse_number_strict<float>(s);
            RC_PRE(reference.has_value());

            // The fallback must be irrelevant for well-formed input: the helper
            // returns the parsed value regardless of what fallback we pass.
            const float parsed = parse_float_or(s, 12345.678f);
            RC_ASSERT(parsed == *reference);
        });
}

// ── Deterministic example checks (make the contract concrete) ───────────────

// These spell out the boundary cases the properties cover: empty and
// non-numeric strings fall back; a numeric prefix with trailing junk parses the
// prefix; a leading-whitespace numeric parses.
bool run_p20_examples() {
    bool ok = true;
    auto expect = [&](const std::string& s, float fallback, float want) {
        const float got = parse_float_or(s, fallback);
        if (!(got == want)) {
            std::printf("[prop_camera_path_float] parse_float_or(\"%s\", %g) = %g, "
                        "expected %g\n", s.c_str(), fallback, got, want);
            ok = false;
        }
    };

    expect("", 60.0f, 60.0f);          // empty → fallback
    expect("abc", -1.0f, -1.0f);       // no numeric prefix → fallback
    expect("1.5", 0.0f, 1.5f);         // well-formed
    expect("-2.25", 0.0f, -2.25f);     // signed well-formed
    expect("3.0deg", 0.0f, 3.0f);      // numeric prefix + junk → parses prefix
    expect("  7.5", 0.0f, 7.5f);       // leading whitespace is skipped
    return ok;
}

bool run_camera_path_float_properties() {
    bool ok = true;
    ok = run_p20a_non_throwing_property() && ok;
    ok = run_p20b_wellformed_parity_property() && ok;
    ok = run_p20_examples() && ok;
    return ok;
}

const heaven::testing::Register g_register_camera_path_float{
    &run_camera_path_float_properties};

} // namespace
