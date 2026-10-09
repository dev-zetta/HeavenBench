/*
 * HeavenBench — Property test for the surface-wetness response
 *
 * Feature: weather-and-rendering-features,: Wetness terms are monotonic with fixed endpoints
 *
 *
 * WeatherState::wetness_global() packs the per-view wetness term read by the
 * lit materials as getMaterialGlobal0():
 *   (specular boost, diffuse darken, wet amount, 1).
 * It returns exactly (0, 0, 0, 1) at effective intensity 0 (the Filament
 * default global, so the lit shaders take their no-op path) and
 *   (max_spec_boost_at_1 * i, max_diffuse_darken_at_1 * i, i, 1)
 * for i > 0.
 *
 * This property asserts, for any two effective intensities a <= b in [0, 1]:
 *   - the specular-boost term (g.x) is monotonic non-decreasing
 *   - the diffuse-darken term (g.y) is monotonic non-decreasing
 *   - the wet-amount term (g.z) is monotonic non-decreasing
 * and the fixed endpoints hold by construction:
 *   - at intensity 0 the global is exactly (0, 0, 0, 1)              (parity)
 *   - at intensity 1 each term equals its configured max endpoint
 *     (g.x == max_spec_boost_at_1, g.y == max_diffuse_darken_at_1,
 *      g.z == 1), with g.w == 1 throughout.
 *
 * Intensities are generated from a finite, bounded integer grid scaled into
 * [0.0, 1.0] in 1/1000 steps, so the sample includes the exact endpoints 0.0
 * and 1.0 by construction (no NaN/Inf to undefine the ordering checks). A
 * fresh WeatherState (default WeatherConfig) is commanded to each intensity
 * via set_intensity(), so the test exercises the real public mapping rather
 * than reconstructing it.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB)
 * in benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
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

// Command a fresh WeatherState (default config) to the given intensity and
// return the packed wetness global. set_intensity clamps to [0,1]; for a
// just-constructed (dry, not drying) state a positive command snaps the
// effective intensity to that value and a zero command leaves it dry, so the
// returned global is wetness_global() evaluated at exactly `intensity`.
glm::vec4 wetness_at(const heaven::WeatherConfig& cfg, float intensity) {
    // Construct a DRY state (rain_intensity 0) so that set_intensity() snaps the
    // effective intensity to exactly `intensity` — including the 0 case. The
    // renderer's default rain_intensity is now medium (0.5); constructing from
    // that and commanding 0 would instead START a dry-down (effective stays 0.5
    // until time advances), which is not what this pure-mapping test wants. The
    // construction default is covered separately by unit_weather_cli_defaults.
    heaven::WeatherConfig start_dry = cfg;
    start_dry.rain_intensity = 0.0f;
    heaven::WeatherState ws{start_dry};
    ws.set_intensity(intensity);
    return ws.wetness_global();
}

bool run_wetness_monotonic_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 7: wetness terms are monotonic with fixed endpoints "
        "(Validates: Requirements 3.1, 3.2)",
        [] {
            // Two intensities drawn from a finite [0.0, 1.0] grid in 1/1000
            // steps; the grid spans the exact endpoints 0.0 and 1.0. Order
            // them so a <= b for the monotonicity comparison.
            const auto gen_intensity = rc::gen::map(
                rc::gen::inRange<int>(0, 1001),
                [](int v) { return static_cast<float>(v) / 1000.0f; });

            float a = *gen_intensity;
            float b = *gen_intensity;
            if (a > b) std::swap(a, b);

            const heaven::WeatherConfig cfg{}; // default response endpoints

            const glm::vec4 ga = wetness_at(cfg, a);
            const glm::vec4 gb = wetness_at(cfg, b);

            // Specular-boost term is monotonic non-decreasing. The term
            // is max_spec_boost_at_1 * i for a non-negative endpoint, so float
            // multiplication is monotonic and an exact <= holds.
            RC_ASSERT(ga.x <= gb.x);

            // Diffuse-darken term is monotonic non-decreasing.
            RC_ASSERT(ga.y <= gb.y);

            // Wet-amount term is monotonic non-decreasing (it equals the
            // effective intensity that drives both terms above).
            RC_ASSERT(ga.z <= gb.z);

            // The packed w component is always 1 (the Filament default).
            RC_ASSERT(ga.w == 1.0f);
            RC_ASSERT(gb.w == 1.0f);

            // Fixed endpoint at intensity 0: the global is exactly (0,0,0,1),
            // so the lit shaders take their no-op (Shipping_Look) path.
            const glm::vec4 g0 = wetness_at(cfg, 0.0f);
            RC_ASSERT(g0.x == 0.0f);
            RC_ASSERT(g0.y == 0.0f);
            RC_ASSERT(g0.z == 0.0f);
            RC_ASSERT(g0.w == 1.0f);

            // Fixed endpoint at intensity 1: each term equals its configured
            // max endpoint (g.z == 1), with w == 1.
            const glm::vec4 g1 = wetness_at(cfg, 1.0f);
            RC_ASSERT(g1.x == cfg.max_spec_boost_at_1);
            RC_ASSERT(g1.y == cfg.max_diffuse_darken_at_1);
            RC_ASSERT(g1.z == 1.0f);
            RC_ASSERT(g1.w == 1.0f);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_wetness_monotonic{
    run_wetness_monotonic_property};

} // namespace
