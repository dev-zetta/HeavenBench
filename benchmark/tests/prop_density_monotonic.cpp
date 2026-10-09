// Feature: weather-and-rendering-features,: Density is monotonic non-decreasing in intensity, within caps
/*
 * Density is monotonic non-decreasing in intensity, within caps.
 *
 *
 * The weather system reports the *active* precipitation and splash density as a
 * [0,1] fraction (WeatherState::streak_fraction / splash_fraction); the
 * Rain_Feature turns those into concrete counts by multiplying by the pool caps
 * kMaxStreaks (20000) and kMaxSplashes (4096) and rounding. This
 * property pins down the two guarantees that matter for the implementation:
 *
 *   - Monotonicity: for any two effective intensities a <= b in [0,1],
 *     the active streak count round(streak_fraction * kMaxStreaks) and the
 *     active splash count round(splash_fraction * kMaxSplashes) at a are each
 *     <= the corresponding count at b. Raising intensity never lowers density.
 *
 *   - Caps: both counts always lie within their bounds —
 *     [0, kMaxStreaks] for streaks and [0, kMaxSplashes] for splashes — for
 *     every intensity in [0,1].
 *
 * The two intensities are produced by commanding two WeatherState instances via
 * set_intensity (the same entry point the renderer uses), so the property is
 * exercised through the real clamp + mapping path rather than the raw field.
 * Intensities are drawn from a bounded integer grid mapped into [0,1] (finite,
 * no NaN/Inf) and sorted so a <= b; the endpoints 0.0 and 1.0 are inside the
 * generated range so the clear-weather and full-storm extremes are covered.
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

// Active count the Rain_Feature derives from a [0,1] fraction: round(frac * cap).
// std::lround matches the current "round(... * cap)" and keeps the result an
// integer count comparable against the pool bounds.
long active_count(float fraction, int cap) {
    return std::lround(static_cast<double>(fraction) * static_cast<double>(cap));
}

bool run_density_monotonic_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 2: density is monotonic non-decreasing in intensity, within "
        "caps (Validates: Requirements 1.1, 2.3, 4.3)",
        [] {
            // Two intensities on a fine [0,1] grid (0.000.. 1.000), ordered so
            // a <= b. The grid endpoints exercise clear weather (0.0) and full
            // storm (1.0); every value is finite.
            const auto intensity_gen =
                rc::gen::map(rc::gen::inRange<int>(0, 1001),
                             [](int v) { return static_cast<float>(v) * 0.001f; });

            float a = *intensity_gen;
            float b = *intensity_gen;
            if (a > b) std::swap(a, b);
            RC_ASSERT(a <= b);

            // Drive two states through the real command path (clamp + mapping).
            heaven::WeatherState lo;
            heaven::WeatherState hi;
            lo.set_intensity(a);
            hi.set_intensity(b);

            // Active counts at the lower and higher intensity.
            const long streaks_lo =
                active_count(lo.streak_fraction(), heaven::kMaxStreaks);
            const long streaks_hi =
                active_count(hi.streak_fraction(), heaven::kMaxStreaks);
            const long splashes_lo =
                active_count(lo.splash_fraction(), heaven::kMaxSplashes);
            const long splashes_hi =
                active_count(hi.splash_fraction(), heaven::kMaxSplashes);

            // ── Monotonic non-decreasing in intensity  ──
            RC_ASSERT(streaks_lo <= streaks_hi);
            RC_ASSERT(splashes_lo <= splashes_hi);

            // ── Counts stay within their caps (streaks,  splashes) ──
            for (long s : {streaks_lo, streaks_hi}) {
                RC_ASSERT(s >= 0);
                RC_ASSERT(s <= static_cast<long>(heaven::kMaxStreaks));
            }
            for (long s : {splashes_lo, splashes_hi}) {
                RC_ASSERT(s >= 0);
                RC_ASSERT(s <= static_cast<long>(heaven::kMaxSplashes));
            }
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_density_monotonic{&run_density_monotonic_property};

} // namespace
