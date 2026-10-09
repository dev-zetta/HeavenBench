/*
 * HeavenBench — Property test for the overcast lighting tint
 *
 * Feature: weather-and-rendering-features,: Overcast tint reduces saturation and luminance monotonically
 *
 *
 * WeatherState::overcast_lighting(base) pushes both the sky colour  and
 * the ambient colour  toward an overcast look as Rain_Intensity rises:
 * first it mixes each colour toward its own Rec.709 luminance-grey by a
 * desaturation fraction (lowers saturation, preserves luminance) and then scales
 * the result by a luminance factor (lowers luminance, preserves saturation).
 * Both knobs grow with intensity, so:
 *
 *   - HSV saturation of the overcast sky/ambient colour is monotonic
 *     non-increasing in intensity, and
 *   - Rec.709 luminance of the overcast sky/ambient colour is monotonic
 *     non-increasing in intensity.
 *
 * At intensity 0 the function returns the base lighting unchanged, so the
 * overcast sky and ambient colours equal their base values exactly (the
 * Shipping_Look parity edge).
 *
 * The property is checked over arbitrary base sky/ambient colours (including
 * HDR channels > 1, near-grey, and saturated primaries), arbitrary overcast
 * desaturation/luminance endpoints, and any ordered pair of intensities
 * a <= b in [0, 1]. overcast_lighting is a member TEMPLATE requiring a lighting
 * type with sun_intensity / sky_color / ambient members, so this TU defines a
 * minimal local POD `TestLighting` to drive it — keeping the test header-only
 * and GLM-only with no rhi.h / Filament dependency.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
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

// Minimal lighting POD with exactly the members WeatherState::overcast_lighting
// touches (sun_intensity, sky_color, ambient). Stands in for rhi.h's
// LightingParams so this test stays GLM-only / header-only.
struct TestLighting {
    float     sun_intensity = 1.0f;
    glm::vec3 sky_color{0.0f};
    glm::vec3 ambient{0.0f};
};

// Rec.709 luminance — the same weights WeatherState::overcast_tint uses. The
// weights sum to 1.0, so a neutral grey's luminance equals its channel value.
[[nodiscard]] float luminance(const glm::vec3& c) {
    return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

// HSV saturation, (max - min) / max, which is invariant under a uniform
// positive scale of all channels (so the luminance-scaling step leaves it
// unchanged) and shrinks monotonically as a colour is mixed toward grey. A
// black/zero colour has undefined HSV saturation; report 0 there.
[[nodiscard]] float hsv_saturation(const glm::vec3& c) {
    const float mx = glm::max(c.r, glm::max(c.g, c.b));
    const float mn = glm::min(c.r, glm::min(c.g, c.b));
    if (mx <= 1e-6f) return 0.0f;
    return (mx - mn) / mx;
}

// A finite float generator over [lo, hi] built from a bounded integer range, so
// generated colours/intensities never include NaN/Inf (which would undefine the
// ordering comparisons).
[[nodiscard]] rc::Gen<float> gen_float(float lo, float hi) {
    return rc::gen::map(rc::gen::inRange<int>(0, 1001), [lo, hi](int k) {
        return lo + (hi - lo) * (static_cast<float>(k) / 1000.0f);
    });
}

// A colour with each channel in [0, 4] — covers near-black, near-grey,
// saturated primaries, and HDR values above 1.
[[nodiscard]] rc::Gen<glm::vec3> gen_color() {
    return rc::gen::apply(
        [](float r, float g, float b) { return glm::vec3(r, g, b); },
        gen_float(0.0f, 4.0f), gen_float(0.0f, 4.0f), gen_float(0.0f, 4.0f));
}

bool run_overcast_tint_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 12: overcast tint reduces sky/ambient saturation and "
        "luminance monotonically (Validates: Requirements 5.2, 5.3)",
        [] {
            // Arbitrary Shipping_Look base lighting.
            TestLighting base;
            base.sun_intensity = *gen_float(0.0f, 10.0f);
            base.sky_color     = *gen_color();
            base.ambient       = *gen_color();

            // Arbitrary overcast endpoints. desat in [0,1]; the luminance
            // reduction stays below 1.0 so the luminance-scale factor is
            // strictly positive (HSV saturation stays well defined).
            heaven::WeatherConfig cfg;
            cfg.overcast_desat_at_1 = *gen_float(0.0f, 1.0f);
            cfg.overcast_lum_at_1   = *gen_float(0.0f, 0.9f);

            // Two ordered intensities a <= b in [0, 1].
            const float x = *gen_float(0.0f, 1.0f);
            const float y = *gen_float(0.0f, 1.0f);
            const float a = std::min(x, y);
            const float b = std::max(x, y);

            heaven::WeatherState ws_a;
            ws_a.cfg = cfg;
            ws_a.set_intensity(a);
            heaven::WeatherState ws_b;
            ws_b.cfg = cfg;
            ws_b.set_intensity(b);

            const TestLighting out_a = ws_a.overcast_lighting(base);
            const TestLighting out_b = ws_b.overcast_lighting(base);

            // Monotonic non-increasing saturation and luminance for BOTH the
            // sky colour  and the ambient colour, as intensity
            // rises from a to b. A small tolerance absorbs float rounding.
            constexpr float kEps = 1e-4f;

            RC_ASSERT(hsv_saturation(out_b.sky_color) <=
                      hsv_saturation(out_a.sky_color) + kEps);
            RC_ASSERT(luminance(out_b.sky_color) <=
                      luminance(out_a.sky_color) + kEps);

            RC_ASSERT(hsv_saturation(out_b.ambient) <=
                      hsv_saturation(out_a.ambient) + kEps);
            RC_ASSERT(luminance(out_b.ambient) <=
                      luminance(out_a.ambient) + kEps);

            // At intensity 0 the overcast sky and ambient colours equal their
            // base values exactly (Shipping_Look parity).
            heaven::WeatherState ws0;
            ws0.cfg = cfg;
            ws0.set_intensity(0.0f);
            const TestLighting out0 = ws0.overcast_lighting(base);
            RC_ASSERT(out0.sky_color == base.sky_color);
            RC_ASSERT(out0.ambient == base.ambient);
            RC_ASSERT(out0.sun_intensity == base.sun_intensity);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_overcast_tint{
    run_overcast_tint_property};

} // namespace
