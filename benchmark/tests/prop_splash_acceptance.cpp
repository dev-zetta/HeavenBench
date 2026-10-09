// Feature: weather-and-rendering-features,: Splash acceptance matches the 30°-of-up filter
/*
 * Splash acceptance matches the 30°-of-up filter.
 *
 *
 * The splash placement test in heaven_splash.mat only spawns a rain-impact
 * effect on a surface whose normal is within kSplashConeDegrees (30°) of world
 * up (+Z, Heaven is Z-up). The CPU mirror in engine/weather_shader_ref.h is the
 * source of truth:
 *
 *     bool splash_accepted(N):
 *         len = length(N)
 *         if len <= 1e-8: return false            / degenerate normal rejected
 *         return dot(N / len, vec3(0,0,1)) >= cos(radians(30))
 *
 * For a unit-length normal at angle theta from +Z, dot(N, up) == cos(theta), so
 * the predicate is exactly `theta <= 30°`. This property pins that equivalence
 * across the whole input space and covers the four cases the requirement cares
 * about:
 *
 *   1. Constructed-angle normals (directly). Build a normal that makes a
 *      KNOWN angle theta with +Z from a generated (theta, azimuth) pair and an
 *      arbitrary positive magnitude (so the test also exercises normalization of
 *      non-unit normals). splash_accepted(N) MUST equal (theta <= 30°).
 *   2. Both sides of the 30° boundary. Explicit normals at 29° (just inside →
 *      accepted) and 31° (just outside → rejected) for every generated azimuth
 *      and magnitude.
 *   3. Degenerate normals. A normal whose length is <= 1e-8 (and the exact zero
 *      vector) cannot face up and MUST be rejected, regardless of direction.
 *   4. Arbitrary normals. Independently recompute the angle from +Z as
 *      acos(clamp(N.z / |N|, -1, 1)) and assert splash_accepted agrees with
 *      (angle <= 30°).
 *
 * Float robustness: comparisons within a small band around the exact 30°
 * boundary are skipped (the predicate's result there is at the mercy of float
 * rounding in cos / normalize and is not a meaningful correctness signal). The
 * explicit boundary checks sit a full degree off the edge, well outside that
 * band. All generators are finite and bounded (integer grids mapped onto closed
 * float intervals; no NaN/Inf), so every angle and dot product is well defined.
 *
 * The function under test is header-only and GLM-only (no GPU/Filament/SDL), so
 * this runs on the host. This translation unit self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point; no second main() is defined.
 */
#include <cmath>
#include <functional>
#include <numbers>
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

constexpr float kPi = std::numbers::pi_v<float>;

// The 30° cone half-angle the splash filter accepts (mirrors the reference).
constexpr float kConeDeg = wr::kSplashConeDegrees;

// Skip the strict accept/reject assertion within this many degrees of the exact
// 30° boundary: there the predicate flips on sub-float-epsilon differences in
// cos()/normalize() and the result is not a meaningful correctness signal.
constexpr float kBoundaryBandDeg = 0.1f;

// Map an integer grid onto a closed [lo, hi] float interval so the endpoints
// are exactly reachable while the interior is covered densely.
rc::Gen<float> ranged(float lo, float hi) {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001),
                        [lo, hi](int g) {
                            return lo + (static_cast<float>(g) / 1'000'000.0f) * (hi - lo);
                        });
}

// Unit-length direction that makes angle `thetaDeg` with +Z at azimuth `phi`.
// At thetaDeg == 0 this is exactly +Z; the Z component is cos(theta), so the
// angle the vector makes with world-up is `thetaDeg` by construction.
glm::vec3 dir_at_angle(float thetaDeg, float phi) {
    const float theta = glm::radians(thetaDeg);
    return {std::sin(theta) * std::cos(phi),
            std::sin(theta) * std::sin(phi),
            std::cos(theta)};
}

bool run_splash_acceptance_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: weather-and-rendering-features, Property 9: Splash acceptance "
        "matches the 30°-of-up filter (Validates: Requirements 4.1)",
        [] {
            // Angle from +Z over the full hemisphere-and-beyond, an arbitrary
            // azimuth, and an arbitrary strictly-positive magnitude (so the
            // normal is non-unit and normalization is exercised).
            const float thetaDeg  = *ranged(0.0f, 90.0f);
            const float phi       = *ranged(0.0f, 2.0f * kPi);
            const float magnitude = *ranged(1.0e-3f, 100.0f);

            // ── Case 1: constructed known-angle normal  ──────────────
            const glm::vec3 N = magnitude * dir_at_angle(thetaDeg, phi);
            const bool accepted = wr::splash_accepted(N);

            // Skip the strict comparison only inside the narrow boundary band.
            if (std::fabs(thetaDeg - kConeDeg) > kBoundaryBandDeg) {
                const bool expected = thetaDeg <= kConeDeg;
                RC_ASSERT(accepted == expected);
            }

            // ── Case 2: both sides of the 30° boundary ─────────────────────
            // A full degree off the edge, well outside the float-flakiness band.
            const glm::vec3 inside  = magnitude * dir_at_angle(kConeDeg - 1.0f, phi);
            const glm::vec3 outside = magnitude * dir_at_angle(kConeDeg + 1.0f, phi);
            RC_ASSERT(wr::splash_accepted(inside));
            RC_ASSERT(!wr::splash_accepted(outside));

            // ── Case 3: degenerate normals are rejected ────────────────────
            // Length <= 1e-8 cannot face up no matter the direction.
            const float tinyMag = *ranged(0.0f, 5.0e-9f);
            const glm::vec3 tinyN = tinyMag * dir_at_angle(thetaDeg, phi);
            RC_ASSERT(!wr::splash_accepted(tinyN));
            RC_ASSERT(!wr::splash_accepted(glm::vec3(0.0f)));

            // ── Case 4: arbitrary normal vs independent angle computation ──
            const glm::vec3 arb(*ranged(-1.0f, 1.0f),
                                *ranged(-1.0f, 1.0f),
                                *ranged(-1.0f, 1.0f));
            const float arbLen = glm::length(arb);
            if (arbLen > 1.0e-3f) {
                // Independent recomputation of the angle this normal makes with
                // world-up, in degrees, from a normalized Z component.
                const float cosToUp = glm::clamp(arb.z / arbLen, -1.0f, 1.0f);
                const float angleDeg = glm::degrees(std::acos(cosToUp));
                if (std::fabs(angleDeg - kConeDeg) > kBoundaryBandDeg) {
                    RC_ASSERT(wr::splash_accepted(arb) == (angleDeg <= kConeDeg));
                }
            }
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_splash_acceptance{&run_splash_acceptance_property};

} // namespace
