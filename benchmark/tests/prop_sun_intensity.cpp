/*
 * HeavenBench — Property test for the overcast sun-intensity falloff
 *
 * Feature: weather-and-rendering-features,: Sun intensity is monotonic non-increasing with fixed endpoints
 *
 *
 * while Rain_Intensity is greater than 0.0 the Weather_System
 * reduces the directional (sun) light intensity monotonically as intensity
 * rises, reaching the configured minimum overcast sun intensity at intensity
 * 1.0 and approaching the Shipping_Look sun intensity as intensity approaches
 * 0.0.
 *
 * WeatherState::sun_scale() implements that mapping as
 *   mix(1.0, min_sun_scale_at_1, effective_intensity())
 * and overcast_lighting(base) applies it as
 *   out.sun_intensity = base.sun_intensity * sun_scale().
 *
 * For any two intensities a <= b in [0, 1] and any base lighting, the property
 * asserts:
 *   - monotonic non-increasing: sun intensity at a >= sun intensity at b,
 *   - fixed endpoint at 0: at effective intensity 0 the sun intensity equals
 *     the base (Shipping_Look) sun intensity exactly (overcast_lighting returns
 *     the base unchanged),
 *   - fixed endpoint at 1: at effective intensity 1 the sun intensity equals
 *     base.sun_intensity * min_sun_scale_at_1 (the configured minimum).
 *
 * overcast_lighting() is templated over the lighting type and only reads
 * `sun_intensity`, `sky_color`, and `ambient`, so the test drives it with a
 * minimal local struct mirroring those LightingParams fields. This keeps the
 * test pure (engine/types.h + GLM only) without pulling in rhi.h / SDL.
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

// Relative comparison with an absolute floor so the endpoint equalities stay
// meaningful across the magnitude range generated (sun intensity ranges from 0
// up to typical Filament lux values in the hundreds of thousands).
bool approx_equal(float a, float b) {
    const float diff  = std::abs(a - b);
    const float scale = std::max({1.0f, std::abs(a), std::abs(b)});
    return diff <= 1e-4f * scale;
}

bool run_sun_intensity_property() {
    // RapidCheck's default is 100 successful cases (TestParams::maxSuccess ==
    // 100), meeting the ">= 100 iterations" requirement; it can be raised via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Property 11: overcast sun intensity is monotonic non-increasing with "
        "fixed endpoints (Validates: Requirements 5.1)",
        [] {
            // Base (Shipping_Look) sun intensity: a non-negative value spanning
            // from a dark sky (0) up past the default 110000 lux the renderer
            // ships, so the multiplicative falloff is exercised across the real
            // magnitude range.
            const float base_sun =
                *rc::gen::map(rc::gen::inRange<int>(0, 200001),
                              [](int v) { return static_cast<float>(v); });

            // Configured minimum overcast sun-intensity multiplier in [0, 1].
            // The mapping mix(1, min, i) = 1 + (min - 1) * i is non-increasing
            // in i for any min <= 1, so the property must hold across all
            // configured minima, not just the default.
            const float min_sun_scale =
                *rc::gen::map(rc::gen::inRange<int>(0, 1001),
                              [](int v) { return static_cast<float>(v) / 1000.0f; });

            // Two effective intensities a <= b in [0, 1].
            const float i0 =
                *rc::gen::map(rc::gen::inRange<int>(0, 1001),
                              [](int v) { return static_cast<float>(v) / 1000.0f; });
            const float i1 =
                *rc::gen::map(rc::gen::inRange<int>(0, 1001),
                              [](int v) { return static_cast<float>(v) / 1000.0f; });
            const float a = std::min(i0, i1);
            const float b = std::max(i0, i1);

            heaven::WeatherConfig cfg;
            cfg.min_sun_scale_at_1 = min_sun_scale;

            TestLighting base;
            base.sun_intensity = base_sun;

            // Construct DRY (rain_intensity 0) states so set_intensity(x) snaps
            // effective_intensity() to exactly x for x in [0,1] — including the
            // 0 endpoint below. The renderer's default rain_intensity is now
            // medium (0.5); constructing from that and commanding 0 would START
            // a dry-down (effective stays 0.5) rather than reading the pure
            // intensity-0 mapping this test wants.
            heaven::WeatherConfig dry = cfg;
            dry.rain_intensity = 0.0f;

            heaven::WeatherState wa(dry);
            wa.set_intensity(a);
            heaven::WeatherState wb(dry);
            wb.set_intensity(b);

            const float sun_a = wa.overcast_lighting(base).sun_intensity;
            const float sun_b = wb.overcast_lighting(base).sun_intensity;

            // Monotonic non-increasing: more rain never raises sun intensity.
            // Allow a tiny relative slack for float rounding in the mix/scale.
            const float tol = 1e-4f * std::max({1.0f, std::abs(sun_a), std::abs(sun_b)});
            RC_ASSERT(sun_a + tol >= sun_b);

            // Fixed endpoint at intensity 0: equals the base sun intensity.
            heaven::WeatherState w0(dry);
            w0.set_intensity(0.0f);
            const float sun_0 = w0.overcast_lighting(base).sun_intensity;
            RC_ASSERT(approx_equal(sun_0, base_sun));

            // Fixed endpoint at intensity 1: equals base * min_sun_scale_at_1,
            // the configured minimum overcast sun intensity.
            heaven::WeatherState w1(dry);
            w1.set_intensity(1.0f);
            const float sun_1 = w1.overcast_lighting(base).sun_intensity;
            RC_ASSERT(approx_equal(sun_1, base_sun * min_sun_scale));
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_sun_intensity{
    run_sun_intensity_property};

} // namespace
