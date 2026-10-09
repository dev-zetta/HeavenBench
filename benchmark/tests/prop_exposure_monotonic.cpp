/*
 * HeavenBench — Property test for physical-camera exposure monotonicity
 *
 * Feature: advanced-materials-and-grading,: Exposure response is monotonic
 *
 *
 * makes the physically-based camera's exposure response
 * monotonic: when a change to aperture, shutter speed, or sensitivity raises the
 * computed exposure value, the scene renders brighter, and when it lowers it the
 * scene renders darker. The host-testable contract lives in the pure mirror
 * heaven::exposure_value(aperture, shutter, iso) in engine/types.h — the
 * single source of truth shared by the physical-camera View-option group and
 * this test. It returns the image-brightness multiplier the renderer applies to
 * scene luminance:
 *
 *     brightness = (shutter * iso) / (120 * aperture^2)
 *
 * (computed via Filament's EV100 = log2(aperture^2 / shutter * 100 / iso) and
 * brightness = 1 / (1.2 * 2^EV100), so a strictly higher brightness multiplier
 * is a brighter render and a strictly lower one a darker render).
 *
 * pins the photometric monotonicity over many valid triples drawn
 * from the PhysicalCameraConfig bounds (k_aperture_lo/hi, k_shutter_lo/hi,
 * k_iso_lo/hi — the single source of truth):
 *
 *   - Sensitivity (ISO): holding aperture and shutter fixed, increasing the ISO
 *     does not decrease brightness.
 *   - Shutter speed: holding aperture and ISO fixed, increasing the shutter time
 *     (longer exposure) does not decrease brightness.
 *   - Aperture (f-number): holding shutter and ISO fixed, increasing the
 *     f-number (a narrower aperture) does not increase brightness.
 *
 * Each case draws a base triple, then a second, ordered value for exactly one
 * parameter (the other two held fixed) so the isolated effect of that parameter
 * is what is asserted. Because brightness spans many orders of magnitude across
 * the bounds (≈6e-9 to ≈2e5) and is realised through a log2/exp2 round-trip, the
 * monotonicity is asserted with a small RELATIVE tolerance so float rounding at
 * the extremes can never flip a non-strict inequality, while still rejecting any
 * genuine reversal.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
#include <cmath>
#include <functional>
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

// Relative tolerance absorbing the float log2/exp2 round-trip in
// exposure_value. brightness is strictly positive over the valid bounds, so a
// relative bound is the natural way to compare values that range across many
// orders of magnitude without ever masking a real monotonicity reversal.
constexpr float kRelTol = 1e-3f;

// Draw a single value inside [lo, hi] by interpolating a 0..1 fraction, so the
// whole physical range (and both bounds) is sampled uniformly regardless of how
// large the span is (ISO reaches 204800).
float gen_in_range(float lo, float hi) {
    const float t = static_cast<float>(*rc::gen::inRange<int>(0, 1001)) / 1000.0f;
    return lo + t * (hi - lo);
}

// Draw two values inside [lo, hi] and return them ordered {smaller, larger}, so
// a parameter can be increased while staying within its physical bounds.
std::pair<float, float> gen_ordered_pair(float lo, float hi) {
    const float a = gen_in_range(lo, hi);
    const float b = gen_in_range(lo, hi);
    return {std::min(a, b), std::max(a, b)};
}

// brightness_high should be >= brightness_low (not decrease) within tolerance.
bool non_decreasing(float low_val, float high_val) {
    return high_val >= low_val * (1.0f - kRelTol);
}

// brightness_high should be <= brightness_low (not increase) within tolerance.
bool non_increasing(float low_val, float high_val) {
    return high_val <= low_val * (1.0f + kRelTol);
}

bool run_exposure_monotonicity_property() {
    using PCC = heaven::PhysicalCameraConfig;

    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 7: physical-camera exposure response is monotonic in each "
        "parameter (Validates: Requirements 11.2)",
        [] {
            // A base triple inside the PhysicalCameraConfig bounds.
            const float aperture = gen_in_range(PCC::k_aperture_lo, PCC::k_aperture_hi);
            const float shutter  = gen_in_range(PCC::k_shutter_lo,  PCC::k_shutter_hi);
            const float iso      = gen_in_range(PCC::k_iso_lo,      PCC::k_iso_hi);

            // Every valid triple yields a strictly positive brightness
            // multiplier, so the relative comparisons below are well-formed.
            const float base = heaven::exposure_value(aperture, shutter, iso);
            RC_ASSERT(base > 0.0f);

            // ── Sensitivity (ISO): increasing ISO must not darken  ──
            {
                const auto [iso_lo, iso_hi] = gen_ordered_pair(PCC::k_iso_lo, PCC::k_iso_hi);
                const float b_lo = heaven::exposure_value(aperture, shutter, iso_lo);
                const float b_hi = heaven::exposure_value(aperture, shutter, iso_hi);
                RC_ASSERT(non_decreasing(b_lo, b_hi));
            }

            // ── Shutter: a longer exposure must not darken  ──
            {
                const auto [sh_lo, sh_hi] = gen_ordered_pair(PCC::k_shutter_lo, PCC::k_shutter_hi);
                const float b_lo = heaven::exposure_value(aperture, sh_lo, iso);
                const float b_hi = heaven::exposure_value(aperture, sh_hi, iso);
                RC_ASSERT(non_decreasing(b_lo, b_hi));
            }

            // ── Aperture: a larger f-number must not brighten  ──
            {
                const auto [ap_lo, ap_hi] = gen_ordered_pair(PCC::k_aperture_lo, PCC::k_aperture_hi);
                const float b_lo = heaven::exposure_value(ap_lo, shutter, iso);
                const float b_hi = heaven::exposure_value(ap_hi, shutter, iso);
                // ap_hi is the larger (narrower) aperture → brightness must not
                // increase relative to the wider ap_lo.
                RC_ASSERT(non_increasing(b_lo, b_hi));
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_exposure_monotonicity{
    run_exposure_monotonicity_property};

} // namespace
