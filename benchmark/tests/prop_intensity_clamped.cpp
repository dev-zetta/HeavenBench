// Feature: weather-and-rendering-features,: Intensity is always clamped to [0, 1]
/*
 * Intensity is always clamped to [0, 1].
 *
 *
 * Rain_Intensity is the single scalar that drives every weather response, and
 * the contract is that it can never escape the inclusive range [0.0, 1.0]
 * regardless of what is commanded, with out-of-range numeric input
 * snapped to the nearest bound. WeatherState::set_intensity is the one
 * entry point the renderer (and the CLI parse) funnels every commanded value
 * through, so this property hammers that entry point with arbitrary float
 * inputs — in-range, out-of-range (both signs, wide magnitude), the exact
 * bounds, and the IEEE specials +Inf / -Inf / NaN — applied as a *sequence*
 * against one evolving WeatherState.
 *
 * After each command the property asserts the two invariants that matter:
 *
 *   - Bounded stored intensity: `target_intensity` is finite and lies within
 *     [0.0, 1.0]. This holds for every input, including NaN: set_intensity
 *     clamps with glm::clamp and then routes any non-positive (or NaN) result
 *     through the dry-down branch that pins `target_intensity` to exactly 0.0.
 *
 *   - Bounded effective intensity: `effective_intensity()` — the value actually
 *     fed to precipitation, splashes, wetness, and the overcast atmosphere — is
 *     finite and within [0.0, 1.0]. While raining it equals `target_intensity`;
 *     during a dry-down it equals `drying_from * (1 - t)` with both factors in
 *     [0,1], so it can never leave the range.
 *
 *   - Clamp identity: for every *finite* command `v`, the stored
 *     `target_intensity` equals glm::clamp(v, 0, 1) — values below 0 snap to 0,
 *     values above 1 snap to 1, in-range values pass through. (NaN has no
 *     well-defined clamp, so the identity is checked only for finite inputs; the
 *     bounded-range invariant above still covers the NaN case.)
 *
 * The command sequence is generated so the edge cases are present by
 * construction: a [0,1] grid, a wide out-of-range grid spanning both signs, and
 * an explicit pick of {0, 1, +Inf, -Inf, NaN}. Applying them in sequence also
 * exercises the raining→dry-down→re-command transitions, confirming the bound
 * holds across state changes, not just from a fresh state.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
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

// A commanded value is "in the unit range" iff it is a finite float within
// [0.0, 1.0]. Used to assert the post-condition on both target_intensity and
// effective_intensity after every set_intensity call.
bool in_unit_range(float x) {
    return std::isfinite(x) && x >= 0.0f && x <= 1.0f;
}

bool run_intensity_clamped_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 1: intensity is always clamped to [0, 1] "
        "(Validates: Requirements 2.1, 2.5)",
        [] {
            // ── Command generator ────────────────────────────────────────
            // In-range values on a fine [0.0, 1.0] grid (includes 0.0 and 1.0).
            const auto in_range = rc::gen::map(
                rc::gen::inRange<int>(0, 1001),
                [](int v) { return static_cast<float>(v) / 1000.0f; });

            // Out-of-range values, both signs, spanning a wide magnitude so the
            // clamp is exercised well past the bounds (e.g. -100.0.. 100.0).
            const auto out_of_range = rc::gen::map(
                rc::gen::inRange<int>(-100000, 100001),
                [](int v) { return static_cast<float>(v) / 1000.0f; });

            // Exact bounds and IEEE specials: these are the inputs most likely
            // to break a clamp (NaN defeats naive comparisons; the infinities
            // sit at the extremes).
            const auto specials = rc::gen::element<float>(
                0.0f,
                1.0f,
                -0.0f,
                std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::quiet_NaN());

            const auto value_gen =
                rc::gen::weightedOneOf<float>({{3, in_range},
                                               {3, out_of_range},
                                               {2, specials}});

            // A sequence of commands applied to one evolving state, so the
            // invariant is checked across raining / dry-down / re-command
            // transitions and not only from a fresh state.
            const auto commands =
                *rc::gen::container<std::vector<float>>(value_gen);

            heaven::WeatherState ws; // default WeatherConfig (clear weather)

            // The freshly constructed state must already satisfy the bound.
            RC_ASSERT(in_unit_range(ws.target_intensity));
            RC_ASSERT(in_unit_range(ws.effective_intensity()));

            for (float v : commands) {
                ws.set_intensity(v);

                // ── Bounded stored intensity  ──
                RC_ASSERT(in_unit_range(ws.target_intensity));

                // ── Bounded effective intensity (drives every response) ──
                RC_ASSERT(in_unit_range(ws.effective_intensity()));

                // ── Clamp identity for finite commands  ──
                // glm::clamp snaps below-0 to 0 and above-1 to 1; for finite v
                // set_intensity stores exactly that. NaN has no defined clamp,
                // so the identity is asserted only for finite inputs — the
                // bounded-range checks above already cover the NaN case.
                if (std::isfinite(v)) {
                    RC_ASSERT(ws.target_intensity ==
                              glm::clamp(v, 0.0f, 1.0f));
                }
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_intensity_clamped{
    &run_intensity_clamped_property};

} // namespace
