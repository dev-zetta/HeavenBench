/*
 * HeavenBench — Property test for the dynamic-resolution minimum scale
 *
 * Feature: advanced-materials-and-grading,: Dynamic-resolution minimum scale stays within (0, 1]
 *
 *
 * make the dynamic-resolution minimum render scale a
 * bounded quantity: Filament's frame-time controller scales the internal render
 * target between `dynres_min_scale` and 1.0, so the configured floor must never
 * leave the scale unbounded at either extreme. `clamp_dynres_min_scale(raw,
 * QualityKnobs)` (engine/types.h) enforces this by pulling any raw input into
 * the configured `[k_dynres_min_scale_lo, k_dynres_min_scale_hi]` band — a band
 * whose lower bound is strictly > 0 and whose upper bound is ≤ 1, i.e. a genuine
 * sub-range of the half-open interval (0, 1]. A NaN collapses to the default
 * (itself in range) and is reported clamped.
 *
 * (prop_grading_camera_clamp.cpp) already pins the generic
 * clamp-and-flag contract shared by the whole helper family. This test is
 * deliberately narrower: it focuses on the (0, 1] sub-range invariant that is
 * specific to dynamic resolution — every returned value, across out-of-range,
 * in-range, extreme, and NaN inputs, lands strictly above 0 and at or below 1
 * (and within the configured [lo, hi]) — and re-confirms the clamped flag tracks
 * out-of-range exactly, with NaN → default reported clamped.
 *
 * The generator straddles the configured band on purpose (below lo, inside,
 * above hi, the extreme ±inf / float limits, and NaN) so both the pass-through
 * and the clamp/NaN branches are exercised every run. The (0, 1] assertions are
 * category-independent (recomputed from `raw`), so float rounding at the bounds
 * never produces a spurious failure.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include <rapidcheck.h>

#include "engine/types.h"

// The registry lives in test_main.cpp (external linkage, no shared header), so
// mirror its minimal interface here to append our test. The struct definition
// is token-identical to the one in test_main.cpp, satisfying the ODR.
namespace heaven::testing {

using TestFn = std::function<bool()>;

std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Generate a raw min-scale request that straddles the configured [lo, hi] band:
// NaN, an extreme (±inf / float limits), below lo, above hi, or inside the
// range — so every case exercises both the pass-through and the clamp/NaN
// branches. The downstream expectations are recomputed from `raw`, so the
// branch only serves to spread coverage across, beyond, and outside the band.
float gen_min_scale_raw(float lo, float hi) {
    const int category = *rc::gen::inRange<int>(0, 6);
    switch (category) {
        case 0: { // NaN
            return std::numeric_limits<float>::quiet_NaN();
        }
        case 1: { // below the low bound (toward / past zero and negative)
            const float delta = static_cast<float>(*rc::gen::inRange<int>(1, 100000)) / 1000.0f;
            return lo - delta;
        }
        case 2: { // above the high bound (past 1.0)
            const float delta = static_cast<float>(*rc::gen::inRange<int>(1, 100000)) / 1000.0f;
            return hi + delta;
        }
        case 3: { // extreme magnitudes at both ends
            const std::array<float, 4> extremes = {
                -std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::max(),
            };
            return extremes[*rc::gen::inRange<std::size_t>(0, extremes.size())];
        }
        default: { // inside the range, including the bounds
            const float t = static_cast<float>(*rc::gen::inRange<int>(0, 1001)) / 1000.0f;
            return lo + t * (hi - lo);
        }
    }
}

bool run_dynres_min_scale_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 9: dynamic-resolution minimum scale stays within (0, 1] "
        "(Validates: Requirements 12.4, 15.3)",
        [] {
            // The bounds live on QualityKnobs as the single source of truth
            // shared with the CLI and this test.
            const heaven::QualityKnobs k{};
            const float lo = heaven::QualityKnobs::k_dynres_min_scale_lo;
            const float hi = heaven::QualityKnobs::k_dynres_min_scale_hi;

            // The configured band is genuinely a sub-range of (0, 1]: lower
            // bound strictly > 0, upper bound ≤ 1. This is the
            // invariant the whole property hangs on.
            RC_ASSERT(lo > 0.0f);
            RC_ASSERT(hi <= 1.0f);
            RC_ASSERT(lo <= hi);

            const float raw = gen_min_scale_raw(lo, hi);
            const heaven::FloatKnobClamp c =
                heaven::clamp_dynres_min_scale(raw, k);

            // ── (0, 1] sub-range invariant  ──
            // The returned scale is always strictly above 0 and at most 1,
            // regardless of how absurd the request was — so dynamic resolution
            // is never left unbounded at either extreme.
            RC_ASSERT(c.value > 0.0f);
            RC_ASSERT(c.value <= 1.0f);

            // And it lands inside the configured band specifically.
            RC_ASSERT(c.value >= lo);
            RC_ASSERT(c.value <= hi);

            if (std::isnan(raw)) {
                // NaN collapses to the default (itself in (0, 1]) and is
                // reported clamped.
                RC_ASSERT(c.clamped);
                RC_ASSERT(c.value == heaven::QualityKnobs{}.dynres_min_scale);
            } else {
                // Otherwise the result is the clamp to [lo, hi], reported
                // clamped iff that differed from the raw request.
                const float expected = std::clamp(raw, lo, hi);
                RC_ASSERT(c.value == expected);
                RC_ASSERT(c.clamped == (expected != raw));
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_dynres_min_scale{
    run_dynres_min_scale_property};

} // namespace
