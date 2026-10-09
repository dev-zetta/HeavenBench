/*
 * A run completes exactly when playback time reaches the path duration.
 *
 *
 * The implementation guarantees that a Benchmark_Run is treated as complete WHEN the playback
 * time reaches or exceeds the authored camera path's total duration. The
 * BenchmarkSystem advances its playback clock 1:1 with the raw frame delta
 * (CameraPathPlayer::advance does time_ += dt) and, after each advance,
 * transitions Running -> Complete exactly when at_end() (time_ >= duration) is
 * true. So for any sequence of positive deltas fed one at a time via
 * tick(dt, camera):
 *
 *   - while the cumulative advanced time is STRICTLY BELOW duration, the system
 *     stays Running;
 *   - the first tick whose cumulative advanced time REACHES OR EXCEEDS duration
 *     transitions the system to Complete;
 *   - every subsequent tick leaves the system Complete (no looping / no
 *     re-running once terminal).
 *
 * Float-boundary handling: the test mirrors the player's clock by accumulating
 * the SAME float deltas in the SAME order the implementation uses (a single
 * pre-computed `deltas` vector is fed to both tick() and the local sum). The
 * local `accumulated` is therefore bit-identical to the player's `time_`, and
 * the expected predicate `accumulated >= duration` is the exact same boolean as
 * the player's `at_end()` (time_ >= duration) — so the at-boundary case is
 * unambiguous rather than relying on an epsilon. `at_end` uses `>=`, so an
 * exactly-equal cumulative counts as Complete.
 *
 * Fixture: a random non-looping CameraPath with >= 2 keyframes whose times start
 * at 0 and strictly increase (duration = last keyframe time, > 0); a non-empty
 * SceneMarkers so request_start() accepts the assets; a pacing probe that
 * reports frame pacing disabled; and a Camera for the player to drive. The
 * deltas are positive fractions of the duration so a moderate sequence usually
 * crosses the end (exercising the Running -> Complete transition and the
 * stays-Complete tail), while short/small-step sequences exercise the
 * stays-Running half.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <cstddef>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/benchmark_system.h"
#include "benchmark/scene_markers.h"
#include "engine/camera.h"
#include "engine/camera_path.h"

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

bool run_run_completes_property() {
    // RapidCheck's default is 100 successful cases (maxSuccess = 100), meeting
    // the ">= 100 iterations" requirement; it can be raised via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Property 5: a run completes exactly when playback time reaches the "
        "path duration (Validates: Requirements 2.5)",
        [] {
            // ── Build a non-looping path with >= 2 strictly-increasing keys ──
            const int n = *rc::gen::inRange<int>(2, 9);

            // Strictly-positive gaps (ms-quantized for clean floats) so keyframe
            // times strictly increase and the duration is positive.
            const auto gap_gen =
                rc::gen::map(rc::gen::inRange<int>(10, 1001),
                             [](int ms) { return static_cast<float>(ms) * 0.001f; });

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
                kf.position = glm::vec3(0.0f, 0.0f, 0.0f);
                kf.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity (w,x,y,z)
                kf.fov = 60.0f;
                path.keyframes.push_back(kf);
            }
            path.duration = path.keyframes.back().time;
            RC_ASSERT(path.duration > 0.0f);

            // ── Positive deltas, pre-computed once so tick() and the local sum
            //    consume bit-identical float values ──
            // Each delta is a positive fraction (0.001.. 0.5) of the duration,
            // so a handful of steps tends to cross the end while short/small
            // sequences stay below it.
            const auto frac_gen =
                rc::gen::map(rc::gen::inRange<int>(1, 501),
                             [](int v) { return static_cast<float>(v) * 0.001f; });

            const int steps = *rc::gen::inRange<int>(1, 160);
            std::vector<float> deltas;
            deltas.reserve(static_cast<size_t>(steps));
            for (int i = 0; i < steps; ++i) {
                deltas.push_back(path.duration * *frac_gen);
            }

            // ── Non-empty markers + pacing-off probe so the start is accepted ──
            heaven::SceneMarkers markers;
            markers.times = {0.0f};

            heaven::BenchmarkSystem bench(&markers, &path, [] { return true; });
            heaven::Camera camera;

            // A successful start enters Running with the clock at zero, and
            // duration > 0 means the system is strictly below the end before any
            // tick (precondition for this property).
            RC_ASSERT(bench.request_start());
            RC_ASSERT(bench.state() == heaven::BenchmarkState::Running);

            // ── Feed the deltas one at a time and check the state predicate ──
            // `accumulated` mirrors the player's clock exactly (same floats,
            // same order), so `accumulated >= duration` is the same boolean as
            // the player's at_end(). Once Complete is reached the run must stay
            // Complete on every further tick.
            float accumulated = 0.0f;
            bool  completed    = false;
            for (float dt : deltas) {
                bench.tick(dt, camera);

                if (completed) {
                    // Terminal: further ticks never revive the run.
                    RC_ASSERT(bench.state() == heaven::BenchmarkState::Complete);
                    continue;
                }

                // Mirror the in-tick advance (time_ += dt) after the fact; the
                // value matches the player's clock bit-for-bit.
                accumulated += dt;

                if (accumulated >= path.duration) {
                    // Reached/exceeded duration -> Complete this very tick.
                    completed = true;
                    RC_ASSERT(bench.state() == heaven::BenchmarkState::Complete);
                } else {
                    // Strictly below duration -> still Running.
                    RC_ASSERT(bench.state() == heaven::BenchmarkState::Running);
                }
            }
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_run_completes{&run_run_completes_property};

} // namespace
