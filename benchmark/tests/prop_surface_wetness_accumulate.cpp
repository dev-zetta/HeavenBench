/*
 * HeavenBench — Property test for the progressive surface-wetness accumulator
 *
 * Feature: weather-and-rendering-features — progressive surface wetness.
 * (time-based build-up / dry-out of the
 * accumulated surface wetness that lags the precipitation).
 *
 * WeatherState carries a SEPARATE accumulated wetness level, surface_wetness()
 * in [0,1], distinct from effective_intensity(). advance(dt) moves it toward a
 * TARGET equal to effective_intensity() — soaking up with the wet_soak_seconds
 * time constant while it is raining (target above the current level) and drying
 * out with the (longer) wet_dry_seconds time constant after the rain stops
 * (target below the current level). The update is the frame-rate-independent
 * exponential approach
 *     cur += (target - cur) * (1 - exp(-dt / tau))
 * which composes exactly across substeps, so reaching the same total elapsed
 * time as one big step or many small steps yields the same wetness (up to float
 * accumulation). This is what the renderer pushes to the wetness material global
 * via surface_wetness_global(), so the wet look builds up and dries out over
 * time rather than snapping on instantly.
 *
 * This property asserts:
 *   (1) a fresh dry WeatherState (no rain) starts at surface_wetness() == 0 and
 *       STAYS exactly 0 under advance() with no rain (Shipping_Look parity);
 *   (2) under continuous rain, surface_wetness() is monotonic NON-DECREASING
 *       over advancing time and approaches but never exceeds the target
 *       intensity;
 *   (3) after the rain stops, surface_wetness() is monotonic NON-INCREASING
 *       toward 0;
 *   (4) frame-rate independence: reaching the same total elapsed time as many
 *       small dt steps vs one big step gives the same surface_wetness() within a
 *       float tolerance;
 *   (5) surface_wetness() is bounded to [0,1] throughout.
 *
 * It uses the real WeatherState public API (set_intensity / advance /
 * surface_wetness) over a fresh dry state, so it exercises the genuine
 * accumulation path. This translation unit self-registers with the RapidCheck
 * harness in test_main.cpp via the heaven::testing::Register pattern, so the
 * file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the entry
 * point; no second main() is defined.
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

// Map an integer grid onto a closed [lo, hi] float interval so the endpoints
// are exactly reachable and no NaN/Inf is ever generated.
rc::Gen<float> ranged(float lo, float hi) {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001), [lo, hi](int g) {
        return lo + (static_cast<float>(g) / 1'000'000.0f) * (hi - lo);
    });
}

// A fresh DRY WeatherState (rain_intensity 0) with the given soak/dry time
// constants. surface_wetness() must be exactly 0 here.
heaven::WeatherState fresh_dry(float soak, float dry) {
    heaven::WeatherConfig cfg{};
    cfg.rain_intensity   = 0.0f;
    cfg.wet_soak_seconds = soak;
    cfg.wet_dry_seconds  = dry;
    return heaven::WeatherState{cfg};
}

bool run_surface_wetness_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: weather-and-rendering-features, progressive surface wetness: "
        "accumulator builds up, dries out, is frame-rate independent and bounded "
        "(Validates: Requirements 3.1, 3.2, 3.4)",
        [] {
            const float soak = *ranged(1.0f, 20.0f);
            const float dry  = *ranged(1.0f, 30.0f);
            const float target = *ranged(0.05f, 1.0f); // a real (positive) rain level

            // Float tolerance covering accumulation across the partition (the
            // only source of divergence): the exponential approach composes
            // exactly in exact arithmetic, so the spread is pure float rounding.
            constexpr float kTol = 1.0e-3f;

            // ── (1) Fresh dry state starts at 0 and stays 0 with no rain ────
            {
                heaven::WeatherState dryState = fresh_dry(soak, dry);
                RC_ASSERT(dryState.surface_wetness() == 0.0f);

                const int n = *rc::gen::inRange<int>(1, 200);
                const float dt = *ranged(0.001f, 2.0f);
                for (int i = 0; i < n; ++i) {
                    dryState.advance(dt);
                    // Never-wet dry state must remain bit-exactly 0 so the
                    // packed global stays (0,0,0,1) and the lit branch is
                    // bypassed (Shipping_Look parity).
                    RC_ASSERT(dryState.surface_wetness() == 0.0f);
                }
                // The packed accumulated global is the Filament default.
                const glm::vec4 gw = dryState.surface_wetness_global();
                RC_ASSERT(gw.x == 0.0f);
                RC_ASSERT(gw.y == 0.0f);
                RC_ASSERT(gw.z == 0.0f);
                RC_ASSERT(gw.w == 1.0f);
            }

            // ── (2) Continuous rain: monotone non-decreasing, never exceeds
            //        target, bounded in [0,1] ──────────────────────────────
            heaven::WeatherState soaking = fresh_dry(soak, dry);
            soaking.set_intensity(target); // start raining at `target`
            {
                const int n = *rc::gen::inRange<int>(2, 400);
                const float dt = *ranged(0.01f, 1.5f);
                float prev = soaking.surface_wetness();
                for (int i = 0; i < n; ++i) {
                    soaking.advance(dt);
                    const float cur = soaking.surface_wetness();
                    // Monotone non-decreasing while soaking.
                    RC_ASSERT(cur >= prev - 1.0e-6f);
                    // Never overshoots the target it is approaching.
                    RC_ASSERT(cur <= target + 1.0e-6f);
                    // Bounded.
                    RC_ASSERT(cur >= 0.0f && cur <= 1.0f);
                    prev = cur;
                }
            }

            // ── (3) Rain stops: monotone non-increasing toward 0 ───────────
            // Commanding clear starts a dry-down of effective_intensity()
            // itself (it decays over dry_down_seconds rather than snapping to
            // 0), and the accumulator chases that still-positive target — so
            // surfaces keep wetting until precipitation has actually reached 0.
            // Drain that effective dry-down first (advance well past the 30 s
            // max) so effective_intensity() is exactly 0; from there the
            // accumulator's target is 0 and the level must dry out monotonically.
            {
                soaking.set_intensity(0.0f); // command clear → effective dry-down begins
                soaking.advance(60.0f);      // complete the effective-intensity dry-down
                RC_ASSERT(soaking.effective_intensity() == 0.0f);

                const int n = *rc::gen::inRange<int>(2, 400);
                const float dt = *ranged(0.01f, 1.5f);
                float prev = soaking.surface_wetness();
                for (int i = 0; i < n; ++i) {
                    soaking.advance(dt);
                    const float cur = soaking.surface_wetness();
                    // Monotone non-increasing while drying out (target is 0).
                    RC_ASSERT(cur <= prev + 1.0e-6f);
                    // Bounded, and never goes below 0.
                    RC_ASSERT(cur >= 0.0f && cur <= 1.0f);
                    prev = cur;
                }
            }

            // ── (4) Frame-rate independence: many small steps == one big
            //        step over the same total elapsed time ──────────────────
            {
                const float total = *ranged(0.5f, 40.0f);
                const int steps = *rc::gen::inRange<int>(2, 500);

                heaven::WeatherState many = fresh_dry(soak, dry);
                many.set_intensity(target);
                const float dt = total / static_cast<float>(steps);
                for (int i = 0; i < steps; ++i) many.advance(dt);

                heaven::WeatherState one = fresh_dry(soak, dry);
                one.set_intensity(target);
                one.advance(total);

                RC_ASSERT(std::abs(many.surface_wetness() - one.surface_wetness()) <= kTol);
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_surface_wetness{
    run_surface_wetness_property};

} // namespace
