// Feature: weather-and-rendering-features,: Zero intensity reproduces the Shipping_Look state
/*
 * HeavenBench — Property test for the zero-intensity Shipping_Look state
 *
 * Zero intensity reproduces the Shipping_Look state.
 *
 *
 * This is the state-level guarantee underlying pixel-level Shipping_Look
 * parity: whenever the Weather_System's effective intensity is 0 — whether the
 * state is freshly constructed/clear, commanded straight to 0, or fully
 * dried-down after an arbitrary sequence of commands — every weather response
 * collapses to its no-op value:
 *
 *   - streak_fraction()  == 0          (zero precipitation)
 *   - splash_fraction()  == 0          (zero splash effects)
 *   - fog_density()      == 0          (shipping fog, no overcast add)
 *   - sun_scale()        == 1.0        (shipping sun intensity)
 *   - wetness_global()   == (0,0,0,1)  (Filament default
 *                                        global, so the lit shaders take their
 *                                        no-op path)
 *   - overcast_lighting(base) == base  (sun/sky/ambient unchanged),
 *                                        bit-identical for arbitrary base
 *                                        lighting
 *
 * Coverage of "all paths to the zero state" (the instruction for this task):
 * the test generates arbitrary base lighting and an arbitrary command sequence
 * (a random mix of set_intensity / advance over a randomised WeatherConfig),
 * then deterministically drives the state to effective intensity 0 by
 * commanding 0 and advancing past the maximum supported dry-down duration. A
 * random prefix can leave the state clear (covering the freshly-constructed /
 * commanded-to-0 paths) or wet/drying (covering the full-dry-down path), so the
 * single forcing step exercises every route into the zero state. The closing
 * advance is split into several sub-steps so reaching 0 via a multi-frame
 * dry-down is exercised too.
 *
 * The WeatherConfig response endpoints are also randomised: at the zero state
 * the responses must be the no-op values regardless of how the overcast/wetness
 * "look" is tuned, so a hard-coded config would under-test the guarantee.
 *
 * overcast_lighting() is a member TEMPLATE requiring a lighting type exposing
 * sun_intensity / sky_color / ambient, so this TU defines a minimal local POD
 * `TestLighting` to drive it — keeping the test header-only and GLM-only with
 * no rhi.h / Filament dependency.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <functional>
#include <vector>

#include <glm/glm.hpp>
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

// Minimal stand-in for LightingParams exposing exactly the three fields
// WeatherState::overcast_lighting() reads/writes. Using this instead of the
// real LightingParams keeps the test free of rhi.h (and its SDL include chain).
struct TestLighting {
    float     sun_intensity = 0.0f;
    glm::vec3 sky_color     = glm::vec3(0.0f);
    glm::vec3 ambient       = glm::vec3(0.0f);
};

// A finite float generator over [lo, hi] built from a bounded integer range, so
// generated values never include NaN/Inf (which would make the exact-equality
// comparisons meaningless / undefine the responses).
[[nodiscard]] rc::Gen<float> gen_float(float lo, float hi) {
    return rc::gen::map(rc::gen::inRange<int>(0, 1001), [lo, hi](int k) {
        return lo + (hi - lo) * (static_cast<float>(k) / 1000.0f);
    });
}

// A colour with each channel in [0, 4] — covers near-black, near-grey,
// saturated primaries, and HDR values above 1, so the "base returned unchanged"
// guarantee is exercised across the realistic lighting range.
[[nodiscard]] rc::Gen<glm::vec3> gen_color() {
    return rc::gen::apply(
        [](float r, float g, float b) { return glm::vec3(r, g, b); },
        gen_float(0.0f, 4.0f), gen_float(0.0f, 4.0f), gen_float(0.0f, 4.0f));
}

bool run_zero_shipping_look_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: weather-and-rendering-features, Property 15: Zero intensity "
        "reproduces the Shipping_Look state "
        "(Validates: Requirements 2.2, 3.6, 4.5, 5.5, 9.4)",
        [] {
            // Arbitrary Shipping_Look base lighting. The zero-state guarantee
            // must hold for any base, so generate a broad finite range.
            TestLighting base;
            base.sun_intensity = *gen_float(0.0f, 200000.0f);
            base.sky_color     = *gen_color();
            base.ambient       = *gen_color();

            // Randomised WeatherConfig: the no-op responses must hold no matter
            // how the overcast/wetness endpoints are tuned. Initial commanded
            // intensity spans [0,1] so the "freshly constructed clear" path
            // (rain_intensity == 0) and the "constructed wet" path are both
            // reachable; dry_down_seconds spans the supported window.
            heaven::WeatherConfig cfg;
            cfg.rain_intensity        = *gen_float(0.0f, 1.0f);
            cfg.dry_down_seconds      = *gen_float(0.5f, 30.0f);
            cfg.min_sun_scale_at_1    = *gen_float(0.0f, 1.0f);
            cfg.overcast_desat_at_1   = *gen_float(0.0f, 1.0f);
            cfg.overcast_lum_at_1     = *gen_float(0.0f, 0.9f);
            cfg.max_fog_density_at_1  = *gen_float(0.0f, 0.1f);
            cfg.max_spec_boost_at_1   = *gen_float(0.0f, 1.0f);
            cfg.max_diffuse_darken_at_1 = *gen_float(0.0f, 1.0f);

            heaven::WeatherState ws(cfg);

            // ── Arbitrary command sequence ─────────────────────────────────
            // A random mix of intensity commands and time advances. The prefix
            // can leave the state clear, wet, or mid-dry-down — every starting
            // configuration for the forced zero step below. An empty prefix
            // (n == 0) covers the freshly-constructed state directly.
            const int n = *rc::gen::inRange<int>(0, 25);
            for (int i = 0; i < n; ++i) {
                const bool do_command = *rc::gen::arbitrary<bool>();
                if (do_command) {
                    // Commands intentionally span outside [0,1] so set_intensity's
                    // clamp is exercised on the way to the zero state too.
                    ws.set_intensity(*gen_float(-0.5f, 1.5f));
                } else {
                    // Per-frame time steps spanning sub-millisecond to multi-
                    // second frames.
                    ws.advance(*gen_float(0.0f, 2.0f));
                }
            }

            // ── Drive deterministically to the zero state ──────────────────
            // Command clear, then advance well past the maximum supported
            // dry-down (30 s) so any in-progress dry-down completes and the
            // state lands at effective intensity 0 with no dry-down pending —
            // i.e. reaching 0 via a full dry-down when the prefix left it wet,
            // or a no-op when it was already clear. Split the advance into
            // several frames so the multi-frame path to 0 is exercised.
            ws.set_intensity(0.0f);
            const int steps = *rc::gen::inRange<int>(1, 16);
            const float total = *gen_float(31.0f, 120.0f);
            const float dt = total / static_cast<float>(steps);
            for (int i = 0; i < steps; ++i) ws.advance(dt);

            // Sanity: we are genuinely in the zero state (effective intensity
            // exactly 0, no dry-down in progress).
            RC_ASSERT(!ws.drying);
            RC_ASSERT(ws.effective_intensity() == 0.0f);

            // ── The Shipping_Look no-op responses ──────────────────────────
            // Precipitation and splash pools are empty.
            RC_ASSERT(ws.streak_fraction() == 0.0f);
            RC_ASSERT(ws.splash_fraction() == 0.0f);

            // No added overcast fog density — shipping fog config.
            RC_ASSERT(ws.fog_density() == 0.0f);

            // Shipping sun intensity multiplier is exactly 1.
            RC_ASSERT(ws.sun_scale() == 1.0f);

            // Wetness global is exactly the Filament default (0,0,0,1), so the
            // lit shaders take their bit-identical no-op path.
            const glm::vec4 g = ws.wetness_global();
            RC_ASSERT(g.x == 0.0f);
            RC_ASSERT(g.y == 0.0f);
            RC_ASSERT(g.z == 0.0f);
            RC_ASSERT(g.w == 1.0f);

            // Overcast lighting returns the base unchanged, bit-for-bit, for
            // arbitrary base lighting — sun, sky, and ambient all preserved

            const TestLighting out = ws.overcast_lighting(base);
            RC_ASSERT(out.sun_intensity == base.sun_intensity);
            RC_ASSERT(out.sky_color == base.sky_color);
            RC_ASSERT(out.ambient == base.ambient);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_zero_shipping_look{
    run_zero_shipping_look_property};

} // namespace
