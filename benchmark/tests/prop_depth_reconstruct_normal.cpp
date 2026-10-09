// Feature: weather-and-rendering-features,: Depth→world reconstruction yields a view-invariant world-space normal
/*
 * The splash pass's depth→world reconstruction yields a TRUE
 * world-space surface normal that is invariant to camera orientation.
 * (the splash ground test relies on a correct
 * world-space up-normal), and is the offline proof for the rotating-normal bug.
 *
 * THE BUG THIS PINS DOWN
 * ----------------------
 * The splash shader (heaven_splash.mat) reconstructs each pixel's WORLD position
 * from the half-res depth pre-pass, builds a normal `n = cross(hVec, vVec)` from
 * neighbour taps, and tests `abs(n.z)` for Z-up ground. The reconstructed normal
 * of FLAT GROUND was observed to CHANGE as the camera rotated (teal looking
 * down, magenta at level views) — a view-DEPENDENT normal, when a true
 * world-space flat-ground normal is constant +Z at every orientation.
 *
 * The CPU mirror in engine/depth_reconstruct_ref.h reproduces the EXACT
 * reconstruction offline, modelling the two things that matter:
 *
 *   1. FILAMENT'S REVERSED-Z PROJECTION. The depth pre-pass renders with
 *      `setProjection(60, aspect, 0.5, 2000, VERTICAL)`. Filament post-multiplies
 *      the GL frustum by a `(0,0,-0.5,0.5)` third row ("GL to inverted DX"),
 *      giving reversed-Z with z∈[0,1] (Vulkan) — NOT GL's z∈[-1,1] non-reversed
 *      (which the failed glm::perspective attempt assumed). The depth texture
 *      stores that reversed-z [0,1] value directly, so getWorldFromClipMatrix()
 *      = inverse(P_revZ·V) consumes the raw sample with no `1-2*depth` remap.
 *
 *   2. THE Y-FLIP. The depth texture is sampled Y-flipped
 *      (`depthUV = (uv.x, 1-uv.y)`). The CENTRE reconstruction is consistent and
 *      was always correct (volfog's distance worked). The BUG is the NEIGHBOUR
 *      taps: the shader reads the vertical neighbour's depth at
 *      `depthUV + (0,+texel.y)` but reconstructs it at screen `uv + (0,+texel.y)`
 *      — the SAME sign. Since depthUV.y runs OPPOSITE to screen uv.y, the
 *      neighbour's depth (pixel below) and its NDC position (above) are
 *      vertically mismatched, so the reconstructed neighbour falls OFF the plane
 *      by an amount that scales with the on-screen vertical depth gradient: ~0
 *      top-down (normal ≈ +Z) but large at level views (normal tilts) — exactly
 *      the teal/magenta symptom. The fix negates the Y offset of the depth tap
 *      (`depthUV + (ox, -oy)`) so each tap reads the depth of the SAME pixel it
 *      is reconstructed at; the X taps were already correct.
 *
 * THE PROPERTY (the assertion that currently fails for the buggy mapping and
 * PASSES for the fixed one)
 * --------------------------------------------------------------------------
 * For a grid of screen UVs and a set of cameras placed at the SAME world point
 * looking in different directions (varying yaw and pitch from near-straight-down
 * to level, using the engine's Z-up lookAt), reconstruct the normal of a known
 * WORLD flat-ground plane (z = const) and a known WORLD vertical wall (x = const)
 * with the CORRECT (Y-flipped tap) mapping and assert:
 *
 *   - FLAT GROUND normal is within a few degrees of world +Z at EVERY camera
 *     orientation (|n̂.z| ≥ cos(3°)) — view-invariant, the clause that fails for
 *     the buggy mapping.
 *   - WALL normal is perpendicular to +Z (|n̂.z| ≤ sin(3°)).
 *   - The reconstructed centre WORLD position round-trips back to the true world
 *     point the eye ray hits, within a tight tolerance.
 *
 * It ALSO asserts the bug is real: with the BUGGY (same-sign tap) mapping, there
 * EXISTS a non-top-down camera for which the flat-ground normal deviates well
 * beyond the 3° tolerance (so this test would catch a regression to the old
 * behaviour, and documents what was wrong).
 *
 * All generators are finite and bounded; the function under test is header-only
 * and GLM-only (no GPU/Filament/SDL), so this runs on the host. This translation
 * unit self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/CMakeLists.txt
 * picks it up without editing the entry point; no second main() is defined.
 */
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

#include <rapidcheck.h>

#include "engine/depth_reconstruct_ref.h"

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

namespace dr = heaven::depth_ref;

// Map an integer grid onto a closed [lo, hi] float interval so the endpoints
// are exactly reachable while the interior is covered densely.
rc::Gen<float> ranged(float lo, float hi) {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001),
                        [lo, hi](int g) {
                            return lo + (static_cast<float>(g) / 1'000'000.0f) * (hi - lo);
                        });
}

// The depth pre-pass projection (depth_prepass.cpp): 60° vertical FOV, near 0.5,
// far 2000. Aspect is generated per-case so the property holds at any window
// shape (the half-res texel size is derived from a generated viewport too).
constexpr float kFovDeg = 60.0f;
constexpr float kNear   = 0.5f;
constexpr float kFar    = 2000.0f;

// Forward direction for a (yaw, pitch) in the engine's convention
// (camera.h Camera::front): pitch clamped to ±89° so the lookAt up=+Z is never
// degenerate, exactly like the live camera.
glm::vec3 forward_from(float yawDeg, float pitchDeg) {
    const float ry = glm::radians(yawDeg);
    const float rp = glm::radians(pitchDeg);
    return glm::normalize(glm::vec3(std::cos(rp) * std::cos(ry),
                                    std::cos(rp) * std::sin(ry),
                                    std::sin(rp)));
}

// cos / sin of the angular tolerance the half-res reconstruction is allowed to
// wobble. The reconstruction of a PLANE is exact in the reference (no half-res
// quantisation in this host model), so the tolerance is tight; 3° leaves margin
// for float rounding in the inverse/cross while still being far tighter than the
// tens-of-degrees error the buggy mapping produces.
constexpr float kTolDeg   = 3.0f;
const float kCosTol = std::cos(glm::radians(kTolDeg));   // flat ground: |n.z| >= this
const float kSinTol = std::sin(glm::radians(kTolDeg));   // wall: |n.z| <= this

bool run_depth_reconstruct_normal_property() {
    return rc::check(
        "Feature: weather-and-rendering-features, Property 11: Depth->world "
        "reconstruction yields a view-invariant world-space normal (Validates: "
        "Requirements 4.1)",
        [] {
            // ── Generated viewport + camera placement ──────────────────────
            const int   vw     = *rc::gen::inRange<int>(640, 2561);
            const int   vh     = *rc::gen::inRange<int>(360, 1441);
            const float aspect = static_cast<float>(vw) / static_cast<float>(vh);
            // Half-res depth pre-pass texel (depth_prepass renders w/2 x h/2),
            // so one half-res texel in UV is 2/vw by 2/vh.
            const glm::vec2 texel(2.0f / static_cast<float>(vw),
                                  2.0f / static_cast<float>(vh));

            const glm::mat4 P =
                dr::filament_reversed_z_projection(kFovDeg, aspect, kNear, kFar);

            // Camera at an arbitrary world point, high enough above the plane to
            // see ground in front of it; ground plane at a generated height.
            const glm::vec3 eye(*ranged(-50.0f, 50.0f),
                                *ranged(-50.0f, 50.0f),
                                *ranged(15.0f, 40.0f));
            const float groundZ = *ranged(-5.0f, 5.0f);
            const dr::Plane ground{glm::vec3(0.0f, 0.0f, 1.0f),
                                   glm::vec3(0.0f, 0.0f, groundZ)};

            // A screen UV in the lower-centre band, where the eye ray reliably
            // strikes the ground in front of a downward/level-tilted camera.
            const glm::vec2 uv(*ranged(0.35f, 0.65f), *ranged(0.30f, 0.55f));

            // ── Orientation sweep: straight-down → 30° down → level ─────────
            // The yaw is arbitrary; the pitch covers the full range the symptom
            // spanned. The KEY clause: the flat-ground normal must be ≈ +Z at
            // EVERY one of these orientations (the buggy mapping fails this).
            const float yaw = *ranged(0.0f, 360.0f);
            const float pitches[] = {-89.0f, -75.0f, -60.0f, -45.0f, -30.0f, -15.0f, -2.0f};

            for (float pitch : pitches) {
                const glm::vec3 fwd = forward_from(yaw, pitch);
                const glm::mat4 clipFromWorld = P * dr::view_matrix_zup(eye, fwd);
                const glm::mat4 worldFromClip = glm::inverse(clipFromWorld);

                const std::optional<dr::NormalSample> s =
                    dr::reconstruct_normal(clipFromWorld, ground, uv, texel,
                                           /*flipTapY=*/true);
                // The eye ray must actually hit the ground for this UV/pitch; if
                // it grazes off (looking too flat for this UV), skip — there is
                // no ground pixel to reconstruct, which is a legitimate miss.
                if (!s) {
                    continue;
                }

                // (A) FLAT-GROUND NORMAL ≈ world +Z, view-invariant. This
                // is the clause that fails for the rotating-normal bug.
                const float nl = glm::length(s->normal);
                RC_ASSERT(nl > 1e-6f);
                const glm::vec3 nUnit = s->normal / nl;
                RC_ASSERT(std::abs(nUnit.z) >= kCosTol);

                // (B) WORLD ROUND-TRIP: the reconstructed centre position is the
                // point the eye ray through `uv` strikes on the ground plane.
                // Recompute that intersection independently and compare.
                const dr::Ray ray = dr::eye_ray(worldFromClip, uv);
                const float denom = glm::dot(ground.normal, ray.dir);
                if (std::abs(denom) > 1e-7f) {
                    const float t =
                        glm::dot(ground.normal, ground.point - ray.origin) / denom;
                    if (t > 0.0f) {
                        const glm::vec3 expected = ray.origin + t * ray.dir;
                        const float err = glm::length(s->worldPos - expected);
                        // Tolerance scales with distance to absorb float
                        // rounding in the inverse-projection of far points.
                        const float dist = glm::length(s->worldPos - eye);
                        RC_ASSERT(err <= 1e-2f + 1e-4f * dist);
                        // The reconstructed point lies ON the ground plane.
                        RC_ASSERT(std::abs(s->worldPos.z - groundZ) <= 1e-2f + 1e-4f * dist);
                    }
                }
            }

            // ── (C) WALL NORMAL ⟂ +Z ───────────────────────────────────────
            // A vertical wall (x = const) reconstructs a normal with ~zero Z at
            // any orientation that faces it. Look roughly toward +X at the wall.
            const float wallX = *ranged(8.0f, 20.0f);
            const dr::Plane wall{glm::vec3(1.0f, 0.0f, 0.0f),
                                 glm::vec3(wallX, 0.0f, 0.0f)};
            const glm::vec3 wallEye(0.0f, *ranged(-3.0f, 3.0f), *ranged(2.0f, 8.0f));
            const float wallYaw   = *ranged(-20.0f, 20.0f);   // ~toward +X
            const float wallPitch = *ranged(-10.0f, 10.0f);
            const glm::mat4 wallClip =
                P * dr::view_matrix_zup(wallEye, forward_from(wallYaw, wallPitch));
            const std::optional<dr::NormalSample> ws = dr::reconstruct_normal(
                wallClip, wall, glm::vec2(0.5f, 0.5f), texel, /*flipTapY=*/true);
            if (ws) {
                const float wnl = glm::length(ws->normal);
                RC_ASSERT(wnl > 1e-6f);
                RC_ASSERT(std::abs(ws->normal.z / wnl) <= kSinTol);
            }
        });
}

// Independent example asserting the BUG is real: with the buggy (same-sign tap)
// mapping there EXISTS a non-top-down camera whose flat-ground normal deviates
// well beyond the tolerance. This documents the defect and guards against a
// regression to the old reconstruction. Runs once (not property-randomised).
bool run_depth_reconstruct_bug_witness() {
    const float aspect = 16.0f / 9.0f;
    const glm::vec2 texel(2.0f / 1280.0f, 2.0f / 720.0f);
    const glm::mat4 P =
        dr::filament_reversed_z_projection(kFovDeg, aspect, kNear, kFar);
    const dr::Plane ground{glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f)};
    const glm::vec3 eye(0.0f, 0.0f, 20.0f);

    bool buggy_rotates = false;
    bool fixed_stable  = true;
    for (float pitch : {-60.0f, -45.0f, -30.0f}) {
        const glm::mat4 clip =
            P * dr::view_matrix_zup(eye, forward_from(0.0f, pitch));
        const auto fixed = dr::reconstruct_normal(clip, ground,
                               glm::vec2(0.5f, 0.45f), texel, true);
        const auto buggy = dr::reconstruct_normal(clip, ground,
                               glm::vec2(0.5f, 0.45f), texel, false);
        if (fixed) {
            const glm::vec3 n = glm::normalize(fixed->normal);
            if (std::abs(n.z) < kCosTol) fixed_stable = false;
        }
        if (buggy) {
            const glm::vec3 n = glm::normalize(buggy->normal);
            if (std::abs(n.z) < std::cos(glm::radians(20.0f))) buggy_rotates = true;
        }
    }

    const bool ok = fixed_stable && buggy_rotates;
    if (!ok) {
        std::printf("[depth_reconstruct_bug_witness] FAILED "
                    "(fixed_stable=%d buggy_rotates=%d)\n",
                    fixed_stable, buggy_rotates);
    }
    return ok;
}

// Register both with the harness at static-init time.
const heaven::testing::Register reg_depth_reconstruct_normal{
    &run_depth_reconstruct_normal_property};
const heaven::testing::Register reg_depth_reconstruct_bug_witness{
    &run_depth_reconstruct_bug_witness};

} // namespace
