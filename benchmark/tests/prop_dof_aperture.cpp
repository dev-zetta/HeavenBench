/*
 * HeavenBench — Property test for depth-of-field vs f-number
 *
 * Feature: advanced-materials-and-grading,: Depth-of-field deepens monotonically with f-number
 *
 *
 * ties the physical camera's depth-of-field to its aperture:
 * a wider aperture (smaller f-number) produces a shallower depth of field (more
 * background blur) than a narrower one. The pure CPU mirror is
 * heaven::dof_metric(float f_number) in engine/types.h — a relative
 * circle-of-confusion / blur proxy (focal_length / f_number, the constant
 * folded to 1) whose only meaningful contract is its monotonicity: the metric
 * is strictly DECREASING in the f-number over positive inputs, with
 * non-positive / NaN f-numbers collapsing to 0 (no blur).
 *
 * pins that contract over many valid f-numbers drawn from the
 * authored aperture range [PhysicalCameraConfig::k_aperture_lo,
 * k_aperture_hi] — the single source of truth shared with the CLI clamp helper
 * and the other property tests:
 *
 *   - Positivity: every valid (positive) f-number yields a strictly positive
 *     metric (some blur), so the screen-space DOF proxy is well defined across
 *     the whole aperture range.
 *
 *   - Strict monotonic decrease: for two valid f-numbers, the larger always
 *     yields a metric less than or equal to the smaller, and for two DISTINCT
 *     f-numbers the larger yields a STRICTLY smaller metric — equivalently a
 *     wider aperture (smaller f-number) gives a strictly larger blur metric
 *     (shallower DOF) than a narrower aperture. Equal f-numbers give equal
 *     metrics.
 *
 *   - Determinism / purity: calling the metric twice with the same f-number
 *     yields the same answer (no hidden state, no time/RNG/disk influence).
 *
 *   - Degenerate inputs: non-positive and NaN f-numbers collapse to 0 (no
 *     blur), and any valid positive f-number outblurs that floor.
 *
 * The generator draws f-numbers as fixed-point fractions of the authored
 * aperture range so both the "two distinct" and "two equal" branches are
 * exercised, and a separate branch injects non-positive / NaN inputs to cover
 * the degenerate floor.
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

// Draw a valid f-number within the authored aperture range
// [k_aperture_lo, k_aperture_hi] as a fixed-point fraction of the span, so
// distinct and coincident draws both occur and the generator stays strictly
// inside the positive, well-defined region of dof_metric.
float gen_valid_f_number() {
    constexpr float lo = heaven::PhysicalCameraConfig::k_aperture_lo;
    constexpr float hi = heaven::PhysicalCameraConfig::k_aperture_hi;
    const float t = static_cast<float>(*rc::gen::inRange<int>(0, 1001)) / 1000.0f;
    return lo + t * (hi - lo);
}

bool run_dof_aperture_monotonic_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 8: depth-of-field deepens monotonically with f-number "
        "(Validates: Requirements 11.3)",
        [] {
            // Two independently drawn valid f-numbers, ordered so `lo_f` is the
            // wider aperture (smaller f-number) and `hi_f` the narrower one.
            const float a = gen_valid_f_number();
            const float b = gen_valid_f_number();
            const float lo_f = std::min(a, b);
            const float hi_f = std::max(a, b);

            const float m_lo = heaven::dof_metric(lo_f);
            const float m_hi = heaven::dof_metric(hi_f);

            // ── Positivity  ──
            // Every valid (positive) f-number yields some blur.
            RC_ASSERT(m_lo > 0.0f);
            RC_ASSERT(m_hi > 0.0f);

            // ── Strict monotonic decrease  ──
            // A larger f-number never yields a larger metric; a wider aperture
            // (smaller f-number) gives a deeper blur (shallower DOF).
            RC_ASSERT(m_hi <= m_lo);
            if (hi_f > lo_f) {
                // Distinct f-numbers: the larger is STRICTLY less blurry.
                RC_ASSERT(m_hi < m_lo);
            } else {
                // Coincident f-numbers: equal metric.
                RC_ASSERT(m_hi == m_lo);
            }

            // ── Determinism / purity ──
            // Same f-number → same metric, with no hidden state between calls.
            RC_ASSERT(heaven::dof_metric(lo_f) == m_lo);
            RC_ASSERT(heaven::dof_metric(hi_f) == m_hi);

            // ── Degenerate inputs collapse to the no-blur floor ──
            // Non-positive and NaN f-numbers yield 0, and any valid positive
            // f-number outblurs that floor.
            const int degenerate = *rc::gen::inRange<int>(0, 3);
            float bad;
            if (degenerate == 0) {
                bad = std::numeric_limits<float>::quiet_NaN();
            } else if (degenerate == 1) {
                bad = 0.0f;
            } else {
                bad = -(static_cast<float>(*rc::gen::inRange<int>(1, 100000)) / 1000.0f);
            }
            RC_ASSERT(heaven::dof_metric(bad) == 0.0f);
            RC_ASSERT(m_lo > heaven::dof_metric(bad));
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_dof_aperture_monotonic{
    run_dof_aperture_monotonic_property};

} // namespace
