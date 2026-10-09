/*
 * Path evaluation is non-looping and holds its endpoints.
 *
 *
 * The implementation guarantees that the camera path is evaluated WITHOUT looping and HOLDS
 * the final keyframe once playback time reaches or exceeds the path duration;
 * symmetrically, for playback time at or before the start the first keyframe is
 * returned.  says the CameraPathPlayer drives the camera from
 * the authored path evaluated at the current playback time — i.e. the player's
 * apply() must match a direct evaluate_clamped()/apply_clamped() at time().
 *
 * For any random path with >= 2 strictly-increasing keyframes starting at t=0
 * (duration = last keyframe time), this property asserts:
 *
 *   - t >= duration   -> evaluate_clamped returns the FINAL keyframe's
 *                        position/fov (held endpoint), and is idempotent: any
 *                        t2 > t >= duration yields the same state (no looping,
 *                        the endpoint just holds).
 *   - t <= 0          -> evaluate_clamped returns the FIRST keyframe's
 *                        position/fov.
 *   - CameraPathPlayer::apply(cam) drives the camera identically to
 *     path.apply_clamped(player.time(), cam) for the advanced time.
 *
 * Keyframe rotations are kept as identity quaternions to sidestep slerp edge
 * cases (acceptable per the implementation): the endpoint/hold behaviour under test is
 * about position/fov clamping and the player↔path delegation, not the rotation
 * interpolation math. Positions and fovs are random but bounded and finite, and
 * keyframe times are a strictly-increasing sum of positive gaps so each segment
 * is well-formed and duration > 0.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "engine/camera.h"
#include "engine/camera_path.h"

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

// Endpoint values come straight out of glm::mix(a, b, 0) (== a) or back()
// (exact), so the differences are typically zero; a small absolute tolerance
// only guards residual float rounding while staying far tighter than any
// looping/wrong-endpoint result would produce.
bool approx(float a, float b, float tol = 1e-3f) {
    return std::abs(a - b) <= tol;
}

bool approx_vec(const glm::vec3& a, const glm::vec3& b, float tol = 1e-3f) {
    return approx(a.x, b.x, tol) && approx(a.y, b.y, tol) &&
           approx(a.z, b.z, tol);
}

bool run_path_clamped_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 3: path evaluation is non-looping and holds its endpoints "
        "(Validates: Requirements 2.1, 2.6)",
        [] {
            // At least two keyframes so the interpolator has a segment.
            const int n = *rc::gen::inRange<int>(2, 13);

            // Bounded, finite component / fov generators (no NaN/Inf, so the
            // segment search and clamps stay well-defined).
            const auto coord_gen =
                rc::gen::map(rc::gen::inRange<int>(-100000, 100001),
                             [](int v) { return static_cast<float>(v) * 0.01f; });
            const auto fov_gen =
                rc::gen::map(rc::gen::inRange<int>(100, 1701),
                             [](int v) { return static_cast<float>(v) * 0.1f; });

            // Strictly-positive gaps (seconds) so keyframe times are strictly
            // increasing and every segment has positive length.
            const auto gap_gen =
                rc::gen::map(rc::gen::inRange<int>(1, 5001),
                             [](int v) { return static_cast<float>(v) * 0.001f; });

            // Build the path: t[0] = 0, t[i] = t[i-1] + gap; identity rotations.
            heaven::CameraPath path;
            path.keyframes.reserve(static_cast<size_t>(n));

            float t_acc = 0.0f;
            for (int i = 0; i < n; ++i) {
                heaven::CameraKeyframe kf;
                if (i == 0) {
                    kf.time = 0.0f;
                } else {
                    t_acc += *gap_gen;
                    kf.time = t_acc;
                }
                kf.position = glm::vec3(*coord_gen, *coord_gen, *coord_gen);
                kf.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity (w,x,y,z)
                kf.fov = *fov_gen;
                path.keyframes.push_back(kf);
            }
            path.duration = path.keyframes.back().time;

            // Strictly-increasing positive gaps guarantee a positive duration.
            RC_ASSERT(path.duration > 0.0f);

            const heaven::CameraKeyframe& first = path.keyframes.front();
            const heaven::CameraKeyframe& last = path.keyframes.back();

            // ── t >= duration holds the FINAL keyframe  ──
            const float over = *gap_gen; // positive overshoot past the end
            for (float t : {path.duration, path.duration + over}) {
                glm::vec3 pos;
                glm::quat rot;
                float fov = 0.0f;
                path.evaluate_clamped(t, pos, rot, fov);
                RC_ASSERT(approx_vec(pos, last.position));
                RC_ASSERT(approx(fov, last.fov));
            }

            // Holding / idempotent: any t2 > t >= duration yields the same
            // state (a looping evaluator would wrap and diverge here).
            {
                const float t1 = path.duration + over;
                const float t2 = t1 + *gap_gen;
                glm::vec3 p1, p2;
                glm::quat r1, r2;
                float f1 = 0.0f, f2 = 0.0f;
                path.evaluate_clamped(t1, p1, r1, f1);
                path.evaluate_clamped(t2, p2, r2, f2);
                RC_ASSERT(approx_vec(p1, p2, 1e-4f));
                RC_ASSERT(approx(f1, f2, 1e-4f));
                // Both equal the final keyframe (the held endpoint).
                RC_ASSERT(approx_vec(p1, last.position));
                RC_ASSERT(approx(f1, last.fov));
            }

            // ── t <= 0 yields the FIRST keyframe  ──
            const float under = *gap_gen; // positive magnitude for a negative t
            for (float t : {0.0f, -under}) {
                glm::vec3 pos;
                glm::quat rot;
                float fov = 0.0f;
                path.evaluate_clamped(t, pos, rot, fov);
                RC_ASSERT(approx_vec(pos, first.position));
                RC_ASSERT(approx(fov, first.fov));
            }

            // ── CameraPathPlayer::apply matches apply_clamped(time()) ──
            // Advance to T in [0, 1.2*duration] so the
            // mid-path and beyond-end (held) cases are both exercised; advance()
            // is 1:1 with real time so time() == T exactly.
            const float frac =
                static_cast<float>(*rc::gen::inRange<int>(0, 1201)) / 1000.0f;
            const float T = path.duration * frac;

            heaven::CameraPathPlayer player(&path);
            player.reset();
            RC_ASSERT(player.time() == 0.0f);
            player.advance(T);
            RC_ASSERT(player.time() == T);

            heaven::Camera cam_a;
            heaven::Camera cam_b;
            path.apply_clamped(player.time(), cam_a);
            player.apply(cam_b);

            RC_ASSERT(approx_vec(cam_a.position, cam_b.position, 1e-4f));
            RC_ASSERT(approx(cam_a.fov, cam_b.fov, 1e-4f));
            RC_ASSERT(approx(cam_a.yaw, cam_b.yaw, 1e-4f));
            RC_ASSERT(approx(cam_a.pitch, cam_b.pitch, 1e-4f));
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_path_clamped{&run_path_clamped_property};

} // namespace
