// Feature: advanced-rendering-features,: SSGI depth reconstruction round-trips
/*
 * The SSGI pass's depth → view-space reconstruction round-trips,
 * and its normal reconstruction always yields a unit-length vector.
 *
 *
 * WHAT THIS PINS DOWN
 * -------------------
 * Screen-space global illumination reconstructs each pixel's VIEW-SPACE position
 * and surface normal from the half-res linear scene-depth pre-pass, then gathers
 * one-bounce indirect light within a bounded screen-space radius. The
 * reconstruction math (un-projecting a screen [uv, distance] back to a
 * view-space position, and deriving a unit normal from neighbour taps) is the
 * part most prone to silent error — a sign flip or a wrong view-ray scale only
 * subtly tints the gather. The CPU mirror in engine/ssgi_reconstruct_ref.h
 * reproduces the exact reconstruction the heaven_ssgi.mat shader is written to
 * match, so the round-trip can be PROVEN offline rather than judged by eye
 * (— the shader stays in lockstep with this host-testable reference).
 *
 * THE DEPTH SOURCE MODELLED (scene-depth-pipeline)
 * ------------------------------------------------
 * The depth pre-pass now renders LINEAR scene depth into an F COLOUR target:
 * each pixel stores the eye-space FORWARD distance in metres (-viewZ), NOT a
 * reversed-Z window depth. SSGI works in VIEW space (camera at the origin
 * looking down -Z), so the reconstruction is a plain view-ray un-projection
 * scaled by that linear distance — the only camera parameter needed is the
 * per-axis view-ray slope projScale = (tan(fovY/2)*aspect, tan(fovY/2)) built by
 * heaven::ssgi_ref::view_ray_scale.
 *
 * THE PROPERTIES (≥100 iterations via rc::check)
 * ----------------------------------------------
 *  (A) ROUND-TRIP: for any view-space point in front of the camera and
 *      inside the frustum, project_view_linear(projScale, p) yields {uv, dist},
 *      and view_position_from_dist(projScale, uv, dist) returns p within float
 *      tolerance. Points are generated with negative view-space Z strictly
 *      between the near and far planes, with X/Y bounded inside the frustum so
 *      project_view_linear always returns a value (uv ∈ [0,1]^2, dist > 0). The
 *      generated point is asserted to actually project (no nullopt) so the
 *      generator genuinely covers the in-frustum input space.
 *
 *  (B) UNIT NORMAL: reconstruct_view_normal(center, right, up) ALWAYS
 *      returns a unit-length vector — including the degenerate case (collinear /
 *      coincident taps, zero-length cross product) where it must fall back to the
 *      unit +Z camera axis. Taps are drawn over a wide range (covering general
 *      planes and explicit degenerate sets) so both the well-defined-plane branch
 *      and the degenerate fallback are exercised across the run.
 *
 * All generators are finite and bounded; the functions under test are
 * header-only and GLM-only (no GPU/Filament/SDL), so this runs on the host. This
 * translation unit self-registers with the RapidCheck harness in test_main.cpp
 * via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no second
 * main() is defined.
 */
#include <cmath>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include <rapidcheck.h>

#include "engine/ssgi_reconstruct_ref.h"

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

namespace sg = heaven::ssgi_ref;

// Map an integer grid onto a closed [lo, hi] float interval so the endpoints are
// reachable while the interior is covered densely.
rc::Gen<float> ranged(float lo, float hi) {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001),
                        [lo, hi](int g) {
                            return lo + (static_cast<float>(g) / 1'000'000.0f) * (hi - lo);
                        });
}

// The depth pre-pass projection (depth_prepass.cpp): 60° vertical FOV, near 0.5,
// far 2000. Aspect is generated per-case so the round-trip holds at any window
// shape. (The linear depth source stores eye-forward metres, so only the FOV /
// aspect view-ray slope matters; near/far bound the generated point range.)
constexpr float kFovDeg = 60.0f;
constexpr float kNear   = 0.5f;
constexpr float kFar    = 2000.0f;

bool run_ssgi_reconstruct_roundtrip_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess ==
    // 100), meeting the ">= 100 iterations" requirement; it can be raised via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: advanced-rendering-features, Property 11: SSGI depth "
        "reconstruction round-trips (Validates: Requirements 7.6, 12.6)",
        [] {
            // ── Generated viewport → per-axis view-ray slope ───────────────
            const int   vw     = *rc::gen::inRange<int>(640, 2561);
            const int   vh     = *rc::gen::inRange<int>(360, 1441);
            const float aspect = static_cast<float>(vw) / static_cast<float>(vh);

            const glm::vec2 projScale = sg::view_ray_scale(kFovDeg, aspect);

            // ── (A) ROUND-TRIP for an in-frustum view-space point ──────────
            // View space looks down -Z, so a point in front of the camera has
            // negative Z. Keep Z strictly inside (near, far) so the eye-forward
            // distance is positive and in range. X/Y are bounded inside the
            // frustum half-extents (with a 0.95 margin) so the projected UV stays
            // inside [0,1]^2.
            const float z = *ranged(-(kFar - 1.0f), -(kNear + 0.05f));
            const float tanHalf = std::tan(glm::radians(kFovDeg) * 0.5f);
            const float halfH = tanHalf * (-z);            // |y| frustum bound
            const float halfW = halfH * aspect;            // |x| frustum bound
            const float x = *ranged(-0.95f * halfW, 0.95f * halfW);
            const float y = *ranged(-0.95f * halfH, 0.95f * halfH);
            const glm::vec3 p(x, y, z);

            const std::optional<sg::UvDist> proj = sg::project_view_linear(projScale, p);
            // The generator constrains p to the frustum, so it MUST project.
            RC_ASSERT(proj.has_value());
            RC_ASSERT(proj->uv.x >= 0.0f && proj->uv.x <= 1.0f);
            RC_ASSERT(proj->uv.y >= 0.0f && proj->uv.y <= 1.0f);
            RC_ASSERT(proj->dist > 0.0f);

            const glm::vec3 reconstructed =
                sg::view_position_from_dist(projScale, proj->uv, proj->dist);

            // The reconstructed view-space point equals the original within a
            // tolerance that scales with distance to absorb float rounding in the
            // inverse-projection of far points.
            const float err  = glm::length(reconstructed - p);
            const float dist = glm::length(p);
            RC_ASSERT(err <= 1e-3f + 1e-4f * dist);

            // ── (B) reconstruct_view_normal is ALWAYS unit length ──────────
            // General (likely non-degenerate) taps over a wide range exercise the
            // well-defined-plane branch.
            const glm::vec3 center(*ranged(-50.0f, -1.0f),
                                   *ranged(-50.0f, 50.0f),
                                   *ranged(-50.0f, -1.0f));
            const glm::vec3 right(*ranged(-50.0f, 50.0f),
                                  *ranged(-50.0f, 50.0f),
                                  *ranged(-50.0f, 50.0f));
            const glm::vec3 up(*ranged(-50.0f, 50.0f),
                               *ranged(-50.0f, 50.0f),
                               *ranged(-50.0f, 50.0f));
            const glm::vec3 n = sg::reconstruct_view_normal(center, right, up);
            RC_ASSERT(std::abs(glm::length(n) - 1.0f) <= 1e-5f);

            // Degenerate tap set (all three coincident → zero-length cross
            // product): the function must still return a unit-length vector via
            // the +Z fallback, never a zero or NaN normal.
            const glm::vec3 nDegenerate =
                sg::reconstruct_view_normal(center, center, center);
            RC_ASSERT(std::abs(glm::length(nDegenerate) - 1.0f) <= 1e-5f);

            // Collinear taps (right and up offset along the same direction) are
            // also degenerate (cross product zero) and must yield a unit normal.
            const glm::vec3 dir(*ranged(-10.0f, 10.0f),
                                *ranged(-10.0f, 10.0f),
                                *ranged(-10.0f, 10.0f));
            const glm::vec3 nCollinear = sg::reconstruct_view_normal(
                center, center + dir, center + 2.0f * dir);
            RC_ASSERT(std::abs(glm::length(nCollinear) - 1.0f) <= 1e-5f);
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register g_register_ssgi_reconstruct_roundtrip{
    &run_ssgi_reconstruct_roundtrip_property};

} // namespace
