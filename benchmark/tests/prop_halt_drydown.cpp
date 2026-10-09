/*
 * Feature: weather-and-rendering-features,: Raising intensity mid-dry-down halts the dry-down
 *
 *
 * IF Rain_Intensity rises above 0.0 while a dry-down
 * transition is in progress, THEN the Wetness_System SHALL halt the dry-down
 * and resume the wetness contribution corresponding to the current
 * Rain_Intensity.
 *
 * WeatherState models the dry-down as a linear decay of effective_intensity()
 * from `drying_from` to 0 over the configured dry-down duration. The property
 * states that for ANY in-progress dry-down — started from any wet intensity,
 * with any dry-down duration, observed at any partial elapsed time and reached
 * through any subdivision of that elapsed time into frame steps — commanding a
 * fresh positive intensity `v` via set_intensity(v) must:
 *   1. clear the drying state (the dry-down is halted), and
 *   2. snap effective_intensity() to exactly `v` (resume the commanded
 *      wetness), and
 *   3. keep effective_intensity() pinned at `v` under any further advance(dt),
 *      because no dry-down is in progress to decay it.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB)
 * in benchmark/CMakeLists.txt picks it up without editing the entry point.
 */
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "engine/types.h"

namespace heaven::testing {

// Mirrors the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is identical across test TUs (ODR-safe).
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Relative comparison with an absolute floor. The commanded value is stored
// verbatim (effective_intensity() returns target_intensity when not drying),
// so the difference is typically exactly zero; the epsilon only guards residual
// float rounding while staying far tighter than any spurious decay would be.
bool approx_equal(float a, float b) {
    const float diff  = std::abs(a - b);
    const float scale = std::max({1.0f, std::abs(a), std::abs(b)});
    return diff <= 1e-6f * scale;
}

bool run_halt_drydown_property() {
    // RapidCheck's default is 100 iterations (maxSuccess = 100), meeting the
    // "≥ 100 iterations" requirement; it can be raised via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Property 6: raising intensity mid-dry-down halts the dry-down "
        "(Validates: Requirements 3.5)",
        [] {
            // Initial wet intensity in (0, 1] — generated in milli-units so the
            // surface starts genuinely wet (a dry-down only begins from > 0).
            const float start =
                *rc::gen::map(rc::gen::inRange<int>(1, 1001),
                              [](int m) { return static_cast<float>(m) / 1000.0f; });

            // Configurable dry-down duration across the supported [0.5, 30.0] s
            // range. The duration only affects the decay rate; the halt
            // behaviour must hold for every duration.
            const float dry_seconds =
                *rc::gen::map(rc::gen::inRange<int>(500, 30001),
                              [](int ms) { return static_cast<float>(ms) / 1000.0f; });

            // Construct already-wet at `start`, then command 0.0 to start a
            // dry-down from the current wetness.
            heaven::WeatherConfig cfg;
            cfg.rain_intensity   = start;
            cfg.dry_down_seconds = dry_seconds;
            heaven::WeatherState ws{cfg};
            ws.set_intensity(0.0f);
            RC_ASSERT(ws.drying); // a dry-down is now in progress

            // Advance PARTWAY through the dry-down so it is still in progress
            // when we raise the intensity. The partial elapsed time is an
            // arbitrary fraction of the duration (kept strictly below the full
            // duration so drying does not complete), reached through an
            // arbitrary number of frame steps to cover "any subdivision".
            const float frac =
                *rc::gen::map(rc::gen::inRange<int>(0, 950),
                              [](int p) { return static_cast<float>(p) / 1000.0f; });
            const int steps = *rc::gen::inRange<int>(1, 32);
            const float partial_total = frac * dry_seconds;
            const float step_dt = partial_total / static_cast<float>(steps);
            for (int i = 0; i < steps; ++i) {
                ws.advance(step_dt);
            }
            // The dry-down must still be in progress before we raise intensity.
            RC_ASSERT(ws.drying);

            // Command a fresh positive intensity mid-dry-down.
            const float v =
                *rc::gen::map(rc::gen::inRange<int>(1, 1001),
                              [](int m) { return static_cast<float>(m) / 1000.0f; });
            ws.set_intensity(v);

            // 1. The dry-down is halted.
            RC_ASSERT(!ws.drying);
            // 2. effective_intensity() resumes the commanded value exactly.
            RC_ASSERT(approx_equal(ws.effective_intensity(), v));

            // 3. With no dry-down in progress, effective_intensity() stays
            //    pinned at `v` under ANY further time advance — no residual
            //    decay leaks through. Use a sequence of arbitrary positive
            //    frame deltas, including ones larger than the dry-down duration.
            const auto dt_gen =
                rc::gen::map(rc::gen::inRange<int>(0, 60001),
                             [](int ms) { return static_cast<float>(ms) / 1000.0f; });
            const std::vector<float> further = *rc::gen::container<std::vector<float>>(dt_gen);
            for (float dt : further) {
                ws.advance(dt);
                RC_ASSERT(!ws.drying);
                RC_ASSERT(approx_equal(ws.effective_intensity(), v));
            }
        });
}

const heaven::testing::Register reg_halt_drydown{&run_halt_drydown_property};

} // namespace
