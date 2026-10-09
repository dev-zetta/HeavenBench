// Feature: cli-argument-parser-refactor,: Time-of-day normalization
/*
 * HeavenBench — Property test for the CLI parser's pure numeric primitives
 *
 * Feature: cli-argument-parser-refactor,: Time-of-day normalization
 * (and supports 11.2)
 *
 * This translation unit exercises the three pure, host-testable parsing
 * primitives declared in engine/cli_parser.h (namespace heaven):
 *
 *   • wrap_24(float)        — normalises an hour value into [0.0, 24.0) by
 *                             adding/subtracting 24.0 as needed. This is the
 *                             core of the --time-of-day flag.
 *   • parse_int_strict(sv)  — full-token integer parse: accepts a well-formed
 *                             integer token, rejects (nullopt) any token with
 *                             trailing junk or that is otherwise not a complete
 *                             integer (supports the report-and-leave-unchanged
 *                             behaviour of).
 *   • parse_float_strict(sv)— full-token float parse with the same accept/reject
 *                             contract (supports).
 *
 * (the named property this file validates):
 *   For any finite numeric H, wrap_24(H) lies in [0.0, 24.0) AND differs from H
 *   by an integer multiple of 24.0. Generated H is drawn from a bounded
 *   fixed-point grid so the values stay finite, the legacy add/subtract-24 loop
 *   terminates quickly, and float accumulation error stays well under the
 *   tolerance used for the "integer multiple of 24" check.
 *
 * SUPPORTING properties (strict parsers, supporting):
 *   • A token formatted from a value round-trips: parse_*_strict accepts it and
 *     returns the same value.
 *   • A well-formed numeric token with appended trailing junk is rejected
 *     (returns nullopt), because strtol/strtod do not consume to the NUL.
 *
 * The primitives are pure (no I/O, no GPU), so they run on the host. This file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined. RapidCheck's default of 100 successful iterations
 * meets the "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <string>
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

// Format a double so std::strtod recovers it exactly: "%.17g" round-trips any
// IEEE-754 double, so the (float) cast parse_float_strict applies lands on the
// same float we computed for the expectation.
std::string format_exact(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return {buf};
}

// ──: time-of-day normalization (wrap_24) ─────────────

// Generate a finite hour value from a bounded fixed-point grid: g in
// [-100000, 100000] mapped to g/100.0 gives the range [-1000.0, 1000.0] in
// 0.01 steps. Bounding the magnitude keeps the legacy add/subtract-24 loop to
// at most ~42 iterations and keeps float accumulation error tiny relative to
// the multiple-of-24 residual check.
bool run_wrap_24_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 6: Time-of-day "
        "normalization (Validates: Requirements 6.1)",
        [] {
            const int g = *rc::gen::inRange<int>(-100'000, 100'001);
            const float h = static_cast<float>(static_cast<double>(g) / 100.0);

            const float result = heaven::wrap_24(h);

            // The normalised hour is always in the half-open day [0.0, 24.0).
            RC_ASSERT(result >= 0.0f);
            RC_ASSERT(result < 24.0f);

            // result and h differ by an integer multiple of 24.0. Recover the
            // nearest integer multiple from the difference and assert the
            // residual is within tolerance (the loop accumulates a handful of
            // 24.0f additions, so a small absolute tolerance is sound for the
            // bounded input range).
            const float diff = h - result;
            const float n    = std::round(diff / 24.0f);
            const float residual = diff - n * 24.0f;
            RC_ASSERT(std::fabs(residual) <= 1e-2f);
        });
}

// ── Supporting: parse_int_strict accept / reject  ────────

// Well-formed integers (full token) are accepted and round-trip; the same
// integer token with appended trailing junk is rejected.
bool run_parse_int_strict_property() {
    // Non-digit suffixes that std::strtol cannot consume, so a token ending in
    // one leaves *end != '\0' and parse_int_strict must reject it. None can
    // begin or extend an integer.
    static const std::vector<std::string> junk_suffixes = {
        "abc", "px", " ", ".5", "x", "!!", "e3", ",", "/2", "deg"};

    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 6 (support): "
        "parse_int_strict accepts well-formed integers, rejects trailing junk "
        "(Validates: Requirements 11.2)",
        [] {
            // Keep within int range; std::strtol returns long and the helper
            // casts to int, so a value formatted with %d round-trips exactly.
            const int value = *rc::gen::inRange<int>(-1'000'000'000, 1'000'000'001);
            const std::string tok = std::to_string(value);

            // Well-formed: accepted and equal to the original value.
            const auto ok = heaven::parse_int_strict(tok);
            RC_ASSERT(ok.has_value());
            RC_ASSERT(*ok == value);

            // Trailing junk: rejected (nullopt), field would be left unchanged.
            const std::string suffix = *rc::gen::elementOf(junk_suffixes);
            const auto bad = heaven::parse_int_strict(tok + suffix);
            RC_ASSERT(!bad.has_value());
        });
}

// ── Supporting: parse_float_strict accept / reject  ──────
bool run_parse_float_strict_property() {
    // Suffixes std::strtod cannot consume after a decimal number. Excludes
    // 'e'/'E'/'p'/'P' (exponent markers) and '.' so the suffix cannot extend a
    // valid float; each forces *end != '\0' → rejection.
    static const std::vector<std::string> junk_suffixes = {
        "abc", "px", " ", "x", "!!", ",5", "/2", "deg", "qq", "unit"};

    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 6 (support): "
        "parse_float_strict accepts well-formed floats, rejects trailing junk "
        "(Validates: Requirements 11.2)",
        [] {
            // Bounded fixed-point grid → finite, non-NaN values that round-trip
            // through %.17g exactly to the same float.
            const int g = *rc::gen::inRange<int>(-10'000'000, 10'000'001);
            const double d = static_cast<double>(g) / 1000.0; // [-10000, 10000]
            const std::string tok = format_exact(d);

            // Well-formed: accepted and equal to the (float) of the value.
            const auto ok = heaven::parse_float_strict(tok);
            RC_ASSERT(ok.has_value());
            RC_ASSERT(*ok == static_cast<float>(d));

            // Trailing junk: rejected (nullopt).
            const std::string suffix = *rc::gen::elementOf(junk_suffixes);
            const auto bad =
                heaven::parse_float_strict(tok + suffix);
            RC_ASSERT(!bad.has_value());
        });
}

bool run_structured_number_parse_examples() {
    using heaven::NumberParseError;

    const auto empty = heaven::parse_int_strict("");
    const auto invalid = heaven::parse_float_strict("not-a-number");
    const auto trailing = heaven::parse_int_strict("42px");
    const auto overflow = heaven::parse_int_strict("999999999999999999999999");
    const auto nan = heaven::parse_float_strict("nan");
    const auto infinity = heaven::parse_float_strict("inf");
    const auto tolerant_int = heaven::parse_int_prefix("  -17items");
    const auto tolerant_float = heaven::parse_float_prefix("\t2.5f");
    const auto explicit_plus = heaven::parse_int_strict("+42");
    const auto consumed =
        heaven::parse_number_prefix_with_consumed<float>("  1.25f, rest");

    return !empty && empty.error() == NumberParseError::Empty &&
           !invalid && invalid.error() == NumberParseError::Invalid &&
           !trailing && trailing.error() == NumberParseError::TrailingCharacters &&
           !overflow && overflow.error() == NumberParseError::OutOfRange &&
           !nan && nan.error() == NumberParseError::NonFinite &&
           !infinity && infinity.error() == NumberParseError::NonFinite &&
           tolerant_int && *tolerant_int == -17 &&
           tolerant_float && *tolerant_float == 2.5f &&
           explicit_plus && *explicit_plus == 42 &&
           consumed && consumed->value == 1.25f && consumed->consumed == 6;
}

// ──: strict integer parse rejects out-of-range  ─

// parse_int_strict must return the exact value for any integer string whose
// value fits in `int`, and std::nullopt for any integer string whose value
// falls outside the representable `int` range. Use an explicit 64-bit domain
// for the rejected values: Windows has 32-bit `long` even on x64, so a domain
// between INT_MAX and LONG_MAX would be empty there. Both the wider integer
// strings and strings beyond 64-bit range must be rejected on every target.

// Build a decimal string whose magnitude is guaranteed to exceed any 64-bit
// integer: a random run of 20..30 digits (INT64_MAX has 19 digits), with a
// non-zero leading digit, optionally negated.
std::string make_beyond_int64_string(bool negative, int digit_count,
                                    const std::vector<int>& digits) {
    std::string s;
    if (negative) {
        s.push_back('-');
    }
    // First digit is 1..9 so the magnitude truly has `digit_count` digits.
    s.push_back(static_cast<char>('1' + (digits.front() % 9)));
    for (int i = 1; i < digit_count; ++i) {
        s.push_back(static_cast<char>('0' + (digits[i] % 10)));
    }
    return s;
}

bool run_parse_int_strict_range_property() {
    return rc::check(
        "Feature: codebase-hardening, Property 21: Strict integer parse rejects "
        "out-of-range (Validates: Requirements 16.1, 16.2, 16.3)",
        [] {
            // Class 1 — in `int` range: parsed exactly, no loss.
            {
                const int value = *rc::gen::inRange<int>(INT_MIN, INT_MAX);
                const std::string tok = std::to_string(
                    static_cast<long long>(value));
                const auto r = heaven::parse_int_strict(tok);
                RC_ASSERT(r.has_value());
                RC_ASSERT(*r == value);
                // INT_MAX itself must round-trip (inRange's upper bound is
                // exclusive, so assert the boundary explicitly).
                const auto hi =
                    heaven::parse_int_strict(std::to_string(
                        static_cast<long long>(INT_MAX)));
                RC_ASSERT(hi.has_value() && *hi == INT_MAX);
            }

            // Class 2 — above INT_MAX but within the explicit 64-bit domain.
            {
                const std::int64_t above = *rc::gen::inRange<std::int64_t>(
                    static_cast<std::int64_t>(INT_MAX) + 1,
                    std::numeric_limits<std::int64_t>::max());
                const auto r =
                    heaven::parse_int_strict(std::to_string(above));
                RC_ASSERT(!r.has_value());
                RC_ASSERT(!heaven::parse_int_strict(std::to_string(
                    static_cast<std::int64_t>(INT_MAX) + 1)));
                RC_ASSERT(!heaven::parse_int_strict(std::to_string(
                    std::numeric_limits<std::int64_t>::max())));
            }

            // Class 3 — below INT_MIN but within the explicit 64-bit domain.
            {
                const std::int64_t below = *rc::gen::inRange<std::int64_t>(
                    std::numeric_limits<std::int64_t>::min(),
                    static_cast<std::int64_t>(INT_MIN));
                const auto r =
                    heaven::parse_int_strict(std::to_string(below));
                RC_ASSERT(!r.has_value());
                RC_ASSERT(!heaven::parse_int_strict(std::to_string(
                    static_cast<std::int64_t>(INT_MIN) - 1)));
                RC_ASSERT(!heaven::parse_int_strict(std::to_string(
                    std::numeric_limits<std::int64_t>::min())));
            }

            // Class 4 — magnitude exceeds the 64-bit integer range.
            {
                const bool negative = *rc::gen::arbitrary<bool>();
                const int digit_count = *rc::gen::inRange<int>(20, 31);
                const std::vector<int> digits = *rc::gen::container<std::vector<int>>(
                    static_cast<std::size_t>(digit_count),
                    rc::gen::inRange<int>(0, 10));
                const std::string tok =
                    make_beyond_int64_string(negative, digit_count, digits);
                const auto r = heaven::parse_int_strict(tok);
                RC_ASSERT(!r.has_value());
            }
        });
}

bool run_cli_numeric_parse_properties() {
    bool ok = true;
    ok = run_wrap_24_property() && ok;
    ok = run_parse_int_strict_property() && ok;
    ok = run_parse_int_strict_range_property() && ok;
    ok = run_parse_float_strict_property() && ok;
    ok = run_structured_number_parse_examples() && ok;
    return ok;
}

const heaven::testing::Register g_register_cli_numeric_parse{
    &run_cli_numeric_parse_properties};

} // namespace
