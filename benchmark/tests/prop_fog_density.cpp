/*
 * HeavenBench — Property test for overcast fog-density monotonicity
 *
 * Feature: weather-and-rendering-features,: Fog density is monotonic non-decreasing with fixed endpoint
 *
 *
 * For any two rain intensities a <= b in [0, 1], the overcast fog density
 * WeatherState::fog_density() must be non-decreasing: fog_density(a) <=
 * fog_density(b). The endpoints are fixed: at intensity 0 the added overcast
 * fog density is exactly 0 (the Shipping_Look uses no distance fog), and at
 * intensity 1 it equals the configured maximum overcast density
 * (WeatherConfig::max_fog_density_at_1).
 *
 * The mapping is pure (WeatherState is header-only, GLM-only, no GPU deps), so
 * it is exercised on the host. This translation unit self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point.
 */
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

// Smart generator for a rain intensity in [0, 1]. Maps an integer grid onto the
// unit interval so the closed-interval endpoints 0.0 and 1.0 are exactly
// representable and reachable (the fixed-endpoint cases), while still covering
// the interior densely.
rc::Gen<float> intensity_gen() {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001),
                        [](int g) { return static_cast<float>(g) / 1'000'000.0f; });
}

bool run_fog_density_property() {
    // RapidCheck's default is 100 iterations (maxSuccess = 100), which meets the
    // "≥ 100 iterations" requirement for this property; it can be raised further
    // at runtime via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: weather-and-rendering-features, Property 13: Fog density is "
        "monotonic non-decreasing with fixed endpoint "
        "(Validates: Requirements 5.4)",
        [] {
            // Configured maximum overcast fog density at full storm. It is a
            // density, so non-negative; generated from an integer grid spanning
            // [0, 0.1], which comfortably includes the default.
            const float max_fog =
                *rc::gen::map(rc::gen::inRange<int>(0, 100'001),
                              [](int g) { return static_cast<float>(g) / 1'000'000.0f; });

            heaven::WeatherConfig cfg{};
            cfg.max_fog_density_at_1 = max_fog;

            // Two intensities, ordered a <= b, drawn from the closed unit
            // interval (Requirement 5.4 is stated over [0, 1]).
            float a = *intensity_gen();
            float b = *intensity_gen();
            if (a > b) std::swap(a, b);

            // Construct DRY (rain_intensity 0) states so set_intensity(x) snaps
            // effective_intensity() to exactly x — including the 0 endpoint
            // below. The renderer's default rain_intensity is now medium (0.5);
            // constructing from that and commanding 0 would START a dry-down
            // (effective stays 0.5) rather than reading the pure mapping.
            heaven::WeatherConfig dry = cfg;
            dry.rain_intensity = 0.0f;

            heaven::WeatherState lo(dry);
            heaven::WeatherState hi(dry);
            lo.set_intensity(a);
            hi.set_intensity(b);

            // Monotonic non-decreasing in intensity.
            RC_ASSERT(lo.fog_density() <= hi.fog_density());

            // Fixed endpoint at intensity 0: no added overcast fog density.
            heaven::WeatherState clear(dry);
            clear.set_intensity(0.0f);
            RC_ASSERT(clear.fog_density() == 0.0f);

            // Fixed endpoint at intensity 1: the configured maximum overcast
            // density.
            heaven::WeatherState storm(dry);
            storm.set_intensity(1.0f);
            RC_ASSERT(storm.fog_density() == cfg.max_fog_density_at_1);
        });
}

const heaven::testing::Register reg_fog_density{&run_fog_density_property};

} // namespace
