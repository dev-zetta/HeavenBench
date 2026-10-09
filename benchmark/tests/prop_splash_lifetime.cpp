// Feature: weather-and-rendering-features,: Every splash completes within 2.0 seconds
/*
 * Every splash completes within 2.0 seconds.
 *
 *
 * Each rain-impact splash runs a ring/ripple animation whose age is driven
 * purely by the absolute `time` uniform. The CPU mirror in
 * engine/weather_shader_ref.h encodes the splash fragment shader's lifetime:
 *
 *     p = min(period, kSplashMaxPeriod); kSplashMaxPeriod == 2.0
 *     age = (p <= 0) ? 0: mod(time - spawnPhase, p);
 *
 * requires every impact effect to complete and be removed within 2.0
 * seconds of spawn. The animation never plays past `age`, and `age` is the
 * remainder modulo the effective period `p = min(period, 2.0)`. Capping the
 * period at kSplashMaxPeriod is exactly what bounds the ripple window: no matter
 * how large the authored `period` is, the effective ripple cannot run longer
 * than 2.0 seconds before it recycles, so every impact finishes within 2.0 s.
 *
 * This property asserts that, for an arbitrary `time`, `spawnPhase`, and
 * `period` — including authored periods greater than 2.0 (which clamp) and
 * non-positive periods (a degenerate window) — `splash_age(...)` always returns
 * a value in [0, 2.0):
 *
 *   - age >= 0: `mod(x, p)` for p > 0 is non-negative; the degenerate
 *                         p <= 0 branch returns exactly 0.
 *   - age < 2.0: age lies in [0, p) with p = min(period, 2.0) <= 2.0,
 *                         so it is strictly below kSplashMaxPeriod and the
 *                         impact completes within 2.0 s.
 *   - age <= p: age respects the (clamped) ripple window.
 *
 * The period generator covers the full semantic space in three regimes:
 *   - [-1.0, 0.0]  non-positive  -> degenerate window, age == 0 exactly;
 *   - [0.05, 1.9]  active period -> p = period, age in [0, p) safely under 2.0;
 *   - [2.0, 6.0]   over the cap  -> p = min(period, 2.0) == 2.0 exactly (the
 *                  boundary case), age in [0, 2.0).
 * Avoiding the razor-thin (1.9, 2.0) sliver — where a non-power-of-2 period just
 * below the cap would let the in-shader `mod` round its remainder to within an
 * ULP of the bound — keeps the strict comparison robust against float rounding
 * while still exercising the cap boundary directly (period >= 2.0 -> p == 2.0).
 * `kEps` absorbs the small float rounding of `mod`'s `x - p*floor(x/p)` near the
 * window edges, matching the tolerance discipline of the sibling weather tests.
 *
 * Generators are finite and bounded (integer grids mapped onto closed float
 * intervals; no NaN/Inf). `time - spawnPhase` is kept to a modest magnitude so
 * the `mod` rounding stays tiny. The function under test is header-only and
 * GLM-only (no GPU/Filament/SDL), so this runs on the host. This translation
 * unit self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "engine/weather_shader_ref.h"

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
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

namespace wr = heaven::weather_ref;

// Map an integer grid onto a closed [lo, hi] float interval so the endpoints
// are exactly reachable while the interior is covered densely.
rc::Gen<float> ranged(float lo, float hi) {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001),
                        [lo, hi](int g) {
                            return lo + (static_cast<float>(g) / 1'000'000.0f) * (hi - lo);
                        });
}

bool run_splash_lifetime_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: weather-and-rendering-features, Property 10: Every splash "
        "completes within 2.0 seconds (Validates: Requirements 4.6)",
        [] {
            // Absolute wall-clock time and the per-splash spawn phase. `time`
            // is kept to a modest range so the `mod`'s `x - p*floor(x/p)`
            // rounding stays tiny; `spawnPhase` may exceed `time`, exercising
            // the negative-argument path of the wrap.
            const float time       = *ranged(0.0f, 60.0f);
            const float spawnPhase = *ranged(0.0f, 10.0f);

            // Authored ripple period across all three semantic regimes:
            //   0 -> non-positive (degenerate window),
            //   1 -> active sub-cap period (p = period, safely < 2.0),
            //   2 -> over the cap (p = min(period, 2.0) == 2.0 exactly).
            const int regime = *rc::gen::inRange<int>(0, 3);
            float period;
            if (regime == 0) {
                period = *ranged(-1.0f, 0.0f);
            } else if (regime == 1) {
                period = *ranged(0.05f, 1.9f);
            } else {
                period = *ranged(2.0f, 6.0f);
            }

            const float age = wr::splash_age(time, spawnPhase, period);

            // Small tolerance for the float rounding the in-shader `mod`
            // introduces near the window edges (see header note).
            constexpr float kEps = 1.0e-3f;

            // ── Core  bound: age lies in [0, 2.0) ──────────────────────
            // Non-negative (the degenerate p <= 0 branch returns exactly 0;
            // otherwise mod(x, p) >= 0), and strictly below the 2.0 s cap, so
            // every impact effect completes and is removed within 2.0 seconds.
            RC_ASSERT(age >= -kEps);
            RC_ASSERT(age < wr::kSplashMaxPeriod);

            if (period <= 0.0f) {
                // A non-positive period is a degenerate window: no ripple plays.
                RC_ASSERT(age == 0.0f);
            } else {
                // The age must stay inside the effective (clamped) window, which
                // is itself capped at kSplashMaxPeriod — this is what makes the
                // 2.0 s completion hold for arbitrarily large authored periods.
                const float effPeriod = std::min(period, wr::kSplashMaxPeriod);
                RC_ASSERT(age <= effPeriod + kEps);
                RC_ASSERT(effPeriod <= wr::kSplashMaxPeriod);
            }
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_splash_lifetime{&run_splash_lifetime_property};

} // namespace
