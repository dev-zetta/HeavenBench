/*
 * HeavenBench —: Reported figures are rounded correctly.
 *
 *
 *
 * The Score_Calculator reports Average_FPS rounded to one decimal place and
 * Score rounded to the nearest integer (heaven::round_fps / heaven::round_score
 * in src/benchmark/score_calculator.{h,cpp}). This property checks both facets
 * across many inputs:
 *
 *   round_fps(x):  the rounded value stays within half a tenth of the input
 *                  (|round_fps(x) - x| <= 0.05) AND carries at most one decimal
 *                  place (round_fps(x) * 10 is an integer, modulo float error).
 *   round_score(x): the rounded value stays within half a unit of the input
 *                  (|round_score(x) - x| <= 0.5), i.e. it is a nearest integer.
 *
 * Inputs are finite, non-negative doubles of modest magnitude (Average_FPS and
 * Score are never negative — they derive from frames/time >= 0). Generating
 * them from a fine fixed-point grid exercises fractional positions across the
 * whole rounding domain, including values near the.5 tie boundary. Small
 * epsilons absorb the representation error introduced by the divide-by-ten in
 * round_fps.
 *
 * This file self-registers with the RapidCheck harness in test_main.cpp via
 * heaven::testing::Register, exactly like the other benchmark test TUs.
 */
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/score_calculator.h"

namespace heaven::testing {

// Mirror of the registry hooks defined in test_main.cpp. registry() is defined
// there (external linkage); Register is an identical inline definition, which
// ODR permits across translation units (this is how a shared header would
// behave). Each test TU appends a callable returning true on success.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Generate a finite, non-negative double in [0, hi] on a fine fixed-point grid
// (1e-4 spacing). This keeps magnitudes modest (avoiding float-granularity
// trouble near very large values) while still landing on many fractional
// positions, including ones close to the.5 rounding tie.
rc::Gen<double> finite_nonneg(int64_t hi_units) {
    return rc::gen::map(rc::gen::inRange<int64_t>(0, hi_units + 1),
                        [](int64_t n) { return static_cast<double>(n) / 10000.0; });
}

// Returns true when every RapidCheck case
// passes. Runs two checks of 100 cases each (RapidCheck's default
// maxSuccess), so the property is exercised over >= 100 iterations.
bool property_14_reported_figures_rounded() {
    // round_fps: within 0.05 of the input and at most one decimal place.
    // x ranges over [0, 10000] (a generous Average_FPS span).
    bool ok = rc::check(
        "Property 14: round_fps is within 0.05 and has <= 1 decimal place",
        [] {
            const double x = *finite_nonneg(100'000'000);  // 10000.0 / 1e-4 grid
            const double r = heaven::round_fps(x);

            // Within half a tenth of the input (tiny epsilon absorbs the
            // divide-by-ten representation error at the.5 tie).
            RC_ASSERT(std::abs(r - x) <= 0.05 + 1e-9);

            // At most one decimal place: r * 10 is (approximately) an integer.
            RC_ASSERT(std::abs(r * 10.0 - std::round(r * 10.0)) < 1e-6);
        });

    // round_score: nearest integer, hence within 0.5 of the input. x ranges
    // over [0, 300000], comfortably covering Score = Average_FPS * 25.19 for
    // an Average_FPS up to ~10000.
    ok = rc::check(
             "Property 14: round_score is the nearest integer (within 0.5)",
             [] {
                 const double x = *finite_nonneg(3'000'000'000);  // 300000.0 / 1e-4 grid
                 const long long s = heaven::round_score(x);
                 RC_ASSERT(std::abs(static_cast<double>(s) - x) <= 0.5 + 1e-9);
             }) &&
         ok;

    return ok;
}

// Register the property at static-init time.
const heaven::testing::Register reg_property_14{&property_14_reported_figures_rounded};

} // namespace
