// Feature: weather-and-rendering-features,: Dry-down is frame-rate independent and completes on schedule
/*
 * Dry-down is frame-rate independent and completes on schedule.
 *
 *
 * When Rain_Intensity drops from a positive value to 0.0 while surfaces are
 * still wet, WeatherState begins a dry-down: effective_intensity() decays
 * linearly from the wetness present when drying started (drying_from) to 0 over
 * the configured dry_down_seconds, advanced purely by accumulated wall-clock
 * time (advance(dt) just does `dry_elapsed += dt`). Because only the *total*
 * accumulated time matters, the transition is frame-rate independent: any
 * partition of the same elapsed time reaches the same state, within float
 * tolerance. This property pins down the three guarantees that matter for:
 *
 *   - Completes on schedule: starting wet and commanding 0, advancing by
 *     a total time >= dry_down_seconds drives effective_intensity() to exactly
 *     0.0, no matter how that time was subdivided into per-frame steps.
 *
 *   - Frame-rate independence of the partial state: for a total elapsed
 *     time t < dry_down_seconds, the effective intensity equals
 *     drying_from * (1 - t/d) within tolerance, and is the same whether the
 *     time arrives as one big step or many small steps.
 *
 *   - Monotonic decrease before full dry-down: while drying and not yet
 *     complete, each successive effective_intensity() reading is <= the
 *     previous one, and the wetness has strictly decreased from its start.
 *
 * The mapping is pure (WeatherState is header-only, GLM-only, no GPU deps), so
 * it is exercised on the host through the real command path (set_intensity +
 * advance). This translation unit self-registers with the RapidCheck harness in
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

// Map an integer grid onto a closed [lo, hi] float interval so the endpoints
// are exactly reachable while the interior is covered densely. Used for the
// dry-down duration, the starting wetness, and the elapsed-time fractions.
rc::Gen<float> ranged(float lo, float hi) {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001),
                        [lo, hi](int g) {
                            return lo + (static_cast<float>(g) / 1'000'000.0f) * (hi - lo);
                        });
}

// Build a fresh WeatherState that is wet at `start_wet` and has just been
// commanded to 0, i.e. a dry-down from drying_from == start_wet is now in
// progress with dry_elapsed == 0. Mirrors how the renderer drives the state.
heaven::WeatherState begin_dry_down(float dry_down_seconds, float start_wet) {
    heaven::WeatherConfig cfg{};
    cfg.dry_down_seconds = dry_down_seconds;
    heaven::WeatherState s(cfg);
    s.set_intensity(start_wet);  // wet: target_intensity = start_wet, not drying
    s.set_intensity(0.0f);       // command clear: begins a dry-down from start_wet
    return s;
}

bool run_dry_down_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: weather-and-rendering-features, Property 5: Dry-down is "
        "frame-rate independent and completes on schedule "
        "(Validates: Requirements 3.4)",
        [] {
            // Dry-down duration in the supported range [0.5, 30.0] s, so
            // clamp_dry_down_seconds() is the identity and d is the true period.
            const float d = *ranged(0.5f, 30.0f);
            // Starting wetness strictly above 0 so a real dry-down begins and
            // drying_from == start_wet.
            const float start_wet = *ranged(0.1f, 1.0f);

            // Tolerance covering float accumulation across the partition (the
            // only source of divergence, since advance() just sums dt). With
            // <= 500 steps over <= 30 s this stays well under 1e-2 of the [0,1]
            // effective-intensity range.
            constexpr float kTol = 1.0e-2f;

            // ── (A) Completes on schedule, independent of subdivision ──────
            // Advance by a total comfortably above d (>= 5% over) so float
            // accumulation can never leave us short of the boundary. The result
            // must be exactly 0 whether the time arrives in one step or many.
            {
                const float factor = *ranged(1.05f, 4.0f);
                const double total_over = static_cast<double>(factor) * d;
                const int n = *rc::gen::inRange<int>(1, 501);

                // Many equal small steps summing to ~total_over.
                heaven::WeatherState many = begin_dry_down(d, start_wet);
                const float dt = static_cast<float>(total_over / n);
                for (int i = 0; i < n; ++i) many.advance(dt);

                // One big step of the whole elapsed time.
                heaven::WeatherState one = begin_dry_down(d, start_wet);
                one.advance(static_cast<float>(total_over));

                // Both reach exactly the fully-dry state: drying ends and
                // effective_intensity() returns the commanded 0 exactly.
                RC_ASSERT(!many.drying);
                RC_ASSERT(!one.drying);
                RC_ASSERT(many.effective_intensity() == 0.0f);
                RC_ASSERT(one.effective_intensity() == 0.0f);
            }

            // ── (B) Partial dry-down: subdivision-independent + monotone ────
            // For a total elapsed time t = f * d strictly below d, the effective
            // intensity equals drying_from * (1 - t/d) within tolerance and is
            // the same for one big step vs many small steps.
            {
                const float f = *ranged(0.02f, 0.90f); // stay safely below d
                const double t = static_cast<double>(f) * d;
                const int n = *rc::gen::inRange<int>(2, 501);

                // Many equal steps, checking monotone non-increase as we go.
                heaven::WeatherState many = begin_dry_down(d, start_wet);
                float prev = many.effective_intensity(); // == start_wet
                RC_ASSERT(std::abs(prev - start_wet) <= kTol);
                const float dt = static_cast<float>(t / n);
                for (int i = 0; i < n; ++i) {
                    many.advance(dt);
                    const float cur = many.effective_intensity();
                    // Monotone non-increasing across frames.
                    RC_ASSERT(cur <= prev + 1.0e-6f);
                    prev = cur;
                }

                // One big step over the same total elapsed time.
                heaven::WeatherState one = begin_dry_down(d, start_wet);
                one.advance(static_cast<float>(t));

                const float eff_many = many.effective_intensity();
                const float eff_one  = one.effective_intensity();

                // Neither has completed (f <= 0.9 keeps us short of d), so both
                // are still drying with a positive effective intensity.
                RC_ASSERT(many.drying);
                RC_ASSERT(one.drying);

                // Subdivision independence: the two partitions of the same
                // elapsed time agree within tolerance.
                RC_ASSERT(std::abs(eff_many - eff_one) <= kTol);

                // On-schedule value: matches the analytic linear decay
                // drying_from * (1 - t/d).
                const float ref =
                    start_wet * static_cast<float>(1.0 - t / static_cast<double>(d));
                RC_ASSERT(std::abs(eff_one - ref) <= kTol);

                // Strictly decreased from the starting wetness (it is drying,
                // not static): f >= 0.02 guarantees a visible drop.
                RC_ASSERT(eff_many < start_wet);
            }
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_dry_down{&run_dry_down_property};

} // namespace
