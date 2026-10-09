/*
 * HeavenBench — Property test for the clamp-and-flag family
 *
 * Feature: advanced-materials-and-grading,: Quality/parameter clamp-and-flag family is bounded and correctly flagged
 *
 *
 * adds a family of pure clamp-and-flag helpers that share one contract,
 * mirroring the existing clamp_quality_* precedent: each pulls an out-of-range
 * input to the nearest valid bound, collapses a NaN to its neutral /
 * Shipping_Look default, and reports the adjustment via a flag
 * (`clamped`/`rejected`) that is set if and only if the raw input lay outside
 * the valid range (NaN always reported). The bounds are the constexpr members
 * authored on the owning structs in engine/types.h /
 * engine/material_model_ref.h — the single source of truth shared by the CLI
 * parser  and this test.
 *
 * The family under test:
 *
 *   - clamp_clear_coat_strength(raw)        → [0, ∞)      (rejected flag)
 *   - clamp_grading_contrast(raw)           → [0, 2]
 *   - clamp_grading_saturation(raw)         → [0, 2]
 *   - clamp_grading_white_balance(raw)      → [-1, 1]
 *   - clamp_camera_aperture(raw)            → [0.5, 64]
 *   - clamp_camera_shutter(raw)             → [1/8000, 30]
 *   - clamp_camera_iso(raw)                 → [25, 204800]
 *   - clamp_dynres_min_scale(raw, knobs)    → [0.25, 1.0] ⊂ (0,1]
 *   - clamp_quality_level(raw) [specular-AA]→ {Low,Med,High} == [0,2]
 *
 * pins, for every helper and over many inputs that deliberately
 * straddle each range (below, inside, above) and explicitly inject NaN:
 *
 *   - Bounded: the returned value always lies within the helper's configured
 *     bounds  — including the NaN-default case, whose neutral
 *     default is itself in range.
 *   - Correctly flagged: `clamped`/`rejected` is true if and only if the raw
 *     input lay outside the valid range, and a NaN both collapses to the safe
 *     default and is reported flagged.
 *
 * Note the assertions are category-independent (they recompute the expected
 * clamp from the raw value), so float rounding at extreme bounds — e.g. a tiny
 * delta vanishing against the large ISO maximum — never produces a spurious
 * failure: the flag is asserted against `expected != raw`, not against which
 * generator branch produced the value.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include <rapidcheck.h>

#include "engine/material_model_ref.h"
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

// Generate a raw float that straddles [lo, hi]: below the low bound, inside the
// range, above the high bound, or NaN — so every case exercises both the
// pass-through and the clamp/NaN branches. The expectations downstream are
// category-independent (recomputed from `raw`), so the only role of the branch
// is to spread coverage across and beyond the range.
float gen_straddling_raw(float lo, float hi) {
    const int category = *rc::gen::inRange<int>(0, 4);
    switch (category) {
        case 0: { // NaN
            return std::numeric_limits<float>::quiet_NaN();
        }
        case 1: { // below the low bound
            const float delta = static_cast<float>(*rc::gen::inRange<int>(1, 100000)) / 1000.0f;
            return lo - delta;
        }
        case 2: { // above the high bound
            const float delta = static_cast<float>(*rc::gen::inRange<int>(1, 100000)) / 1000.0f;
            return hi + delta;
        }
        default: { // inside the range, including the bounds
            const float t = static_cast<float>(*rc::gen::inRange<int>(0, 1001)) / 1000.0f;
            return lo + t * (hi - lo);
        }
    }
}

// Assert the shared FloatKnobClamp contract for one float clamp helper: the
// result is always within [lo, hi]; a NaN collapses to `neutral_default` and is
// reported clamped; otherwise the result equals std::clamp(raw, lo, hi) and is
// reported clamped iff that differed from raw.
void check_float_clamp(const std::function<heaven::FloatKnobClamp(float)>& fn,
                       float lo, float hi, float neutral_default) {
    const float raw = gen_straddling_raw(lo, hi);
    const heaven::FloatKnobClamp c = fn(raw);

    // Bounded: always within the configured range.
    RC_ASSERT(c.value >= lo);
    RC_ASSERT(c.value <= hi);

    if (std::isnan(raw)) {
        // NaN collapses to the neutral / Shipping_Look default and is flagged

        RC_ASSERT(c.clamped);
        RC_ASSERT(c.value == neutral_default);
    } else {
        const float expected = std::clamp(raw, lo, hi);
        RC_ASSERT(c.value == expected);
        // Flagged iff the input was out of range.
        RC_ASSERT(c.clamped == (expected != raw));
    }
}

bool run_clamp_and_flag_family_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 5: quality/parameter clamp-and-flag family is bounded and "
        "correctly flagged (Validates: Requirements 1.6, 8.5, 10.5, 11.5, "
        "12.6, 15.3, 15.4, 15.5)",
        [] {
            // ── Grading controls  — neutral defaults in range ──
            check_float_clamp(heaven::clamp_grading_contrast,
                              heaven::ColorGradingConfig::k_contrast_lo,
                              heaven::ColorGradingConfig::k_contrast_hi,
                              heaven::ColorGradingConfig{}.contrast);
            check_float_clamp(heaven::clamp_grading_saturation,
                              heaven::ColorGradingConfig::k_saturation_lo,
                              heaven::ColorGradingConfig::k_saturation_hi,
                              heaven::ColorGradingConfig{}.saturation);
            check_float_clamp(heaven::clamp_grading_white_balance,
                              heaven::ColorGradingConfig::k_white_balance_lo,
                              heaven::ColorGradingConfig::k_white_balance_hi,
                              heaven::ColorGradingConfig{}.temperature);

            // ── Physical-camera parameters  ──
            check_float_clamp(heaven::clamp_camera_aperture,
                              heaven::PhysicalCameraConfig::k_aperture_lo,
                              heaven::PhysicalCameraConfig::k_aperture_hi,
                              heaven::PhysicalCameraConfig{}.aperture);
            check_float_clamp(heaven::clamp_camera_shutter,
                              heaven::PhysicalCameraConfig::k_shutter_lo,
                              heaven::PhysicalCameraConfig::k_shutter_hi,
                              heaven::PhysicalCameraConfig{}.shutter_speed);
            check_float_clamp(heaven::clamp_camera_iso,
                              heaven::PhysicalCameraConfig::k_iso_lo,
                              heaven::PhysicalCameraConfig::k_iso_hi,
                              heaven::PhysicalCameraConfig{}.sensitivity);

            // ── Dynamic-resolution minimum scale  ──
            // The bounds live on QualityKnobs as the single source of truth;
            // the default knobs carry the shipped (0, 1] sub-range.
            {
                const heaven::QualityKnobs k{};
                check_float_clamp(
                    [&](float raw) { return heaven::clamp_dynres_min_scale(raw, k); },
                    heaven::QualityKnobs::k_dynres_min_scale_lo, heaven::QualityKnobs::k_dynres_min_scale_hi,
                    heaven::QualityKnobs{}.dynres_min_scale);
                // The configured sub-range is genuinely within (0, 1].
                RC_ASSERT(k.k_dynres_min_scale_lo > 0.0f);
                RC_ASSERT(k.k_dynres_min_scale_hi <= 1.0f);
            }

            // ── Specular-AA level via clamp_quality_level  ──
            // Integer-valued knob: ordered levels [0, 2] == {Low, Med, High};
            // out-of-range integers clamp to the nearest level and are flagged.
            {
                const int raw = *rc::gen::inRange<int>(-1000, 1004);
                const heaven::QualityLevelClamp c = heaven::clamp_quality_level(raw);
                const int expected = std::clamp(raw, 0, 2);

                // Bounded within the valid ordered range.
                RC_ASSERT(static_cast<int>(c.value) >= 0);
                RC_ASSERT(static_cast<int>(c.value) <= 2);
                RC_ASSERT(static_cast<int>(c.value) == expected);
                // Flagged iff out of range.
                RC_ASSERT(c.clamped == (expected != raw));
            }

            // ── Clear-coat strength  — non-negative, reject flag ──
            // The only family member with a `rejected` flag and an open upper
            // bound: a negative strength (or NaN) is physically meaningless,
            // rejected, and collapsed to 0; a valid non-negative value passes
            // through unchanged and unflagged.
            {
                const int category = *rc::gen::inRange<int>(0, 3);
                float raw;
                if (category == 0) {
                    raw = std::numeric_limits<float>::quiet_NaN();
                } else if (category == 1) {
                    // Strictly negative.
                    raw = -(static_cast<float>(*rc::gen::inRange<int>(1, 100000)) / 1000.0f);
                } else {
                    // Non-negative (includes 0).
                    raw = static_cast<float>(*rc::gen::inRange<int>(0, 100000)) / 1000.0f;
                }

                const heaven::material_model_ref::ClearCoatStrengthClamp c =
                    heaven::material_model_ref::clamp_clear_coat_strength(raw);

                // Bounded to the valid non-negative range.
                RC_ASSERT(c.value >= 0.0f);

                if (std::isnan(raw) || raw < 0.0f) {
                    // Out of range: rejected, collapsed to the safe 0 default.
                    RC_ASSERT(c.rejected);
                    RC_ASSERT(c.value == 0.0f);
                } else {
                    // In range: passes through unchanged, unflagged.
                    RC_ASSERT(!c.rejected);
                    RC_ASSERT(c.value == raw);
                }
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_clamp_and_flag_family{
    run_clamp_and_flag_family_property};

} // namespace
