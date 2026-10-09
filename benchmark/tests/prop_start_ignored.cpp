/*
 * Start requests are ignored while a run is active.
 *
 *
 * The implementation guarantees that that WHILE a Benchmark_Run is active, a request to start
 * a new run (via the benchmark CLI option or the Benchmark control) SHALL be
 * ignored and SHALL leave the active run's playback time, Stats_Collector, and
 * Scene_Index unchanged. BenchmarkSystem::request_start() surfaces this by
 * returning false while Running and mutating nothing.
 *
 * The fixture is a usable, mid-flight run: a CameraPath with >= 2 strictly
 * increasing keyframes starting at t = 0 and a deliberately LARGE duration
 * (1000 s) so the random ticks below never reach the end and the run stays
 * Running; a non-empty (normalized, non-decreasing) SceneMarkers list; a frame
 * pacing probe that reports pacing disabled; and a free-fly Camera to steer.
 *
 * For each case this property:
 *   - starts a run from Idle and asserts request_start() returns true and the
 *     system is Running,
 *   - ticks a random number of frames with random strictly-positive deltas so
 *     the run is mid-flight (cumulative time kept far below the 1000 s
 *     duration) and still Running,
 *   - captures the status() snapshot (state, elapsed, frames, scene index),
 *   - calls request_start() AGAIN one or more times: each call must return
 *     false and leave the snapshot (state still Running, same elapsed, same
 *     frames, same scene index) completely unchanged.
 *
 * Keyframe rotations are identity quaternions: this property is about the
 * start-guard's no-op semantics, not the rotation interpolation math. The
 * snapshot fields compared (elapsed/frames/scene_index) are integers or a
 * double that is only ever written by tick(); since the repeated
 * request_start() calls perform no tick, exact equality is the correct check.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
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

bool run_start_ignored_property() {
    // RapidCheck's default is 100 successful iterations (maxSuccess == 100),
    // meeting the ">= 100 iterations" requirement; raise via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Property 7: start requests are ignored while a run is active "
        "(Validates: Requirements 3.5)",
        [] {
            // ── CameraPath fixture: >= 2 strictly-increasing keyframes from
            // t = 0, with a large duration so the run never completes during
            // the test ticks. ─────────────────────────────────────────────
            const int n = *rc::gen::inRange<int>(2, 14);
            constexpr float kDuration = 1000.0f;

            const auto coord_gen =
                rc::gen::map(rc::gen::inRange<int>(-100000, 100001),
                             [](int v) { return static_cast<float>(v) * 0.01f; });
            const auto fov_gen =
                rc::gen::map(rc::gen::inRange<int>(100, 1701),
                             [](int v) { return static_cast<float>(v) * 0.1f; });

            heaven::CameraPath path;
            path.keyframes.reserve(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                heaven::CameraKeyframe kf;
                // Evenly spaced, strictly increasing times in [0, kDuration].
                kf.time = kDuration * static_cast<float>(i) /
                          static_cast<float>(n - 1);
                kf.position = glm::vec3(*coord_gen, *coord_gen, *coord_gen);
                kf.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity (w,x,y,z)
                kf.fov = *fov_gen;
                path.keyframes.push_back(kf);
            }
            path.duration = kDuration;
            RC_ASSERT(path.keyframes.size() >= size_t{2});

            // ── SceneMarkers fixture: a non-empty list normalized to
            // non-decreasing order. Times are kept small (<= 2 s) so the few
            // seconds of ticks below can advance the scene index past zero,
            // exercising the "scene index unchanged" clause meaningfully. ──
            const int marker_count = *rc::gen::inRange<int>(1, 27); // 1..26
            std::vector<float> marker_times;
            marker_times.reserve(static_cast<size_t>(marker_count));
            for (int i = 0; i < marker_count; ++i) {
                marker_times.push_back(
                    static_cast<float>(*rc::gen::inRange<int>(0, 2001)) *
                    0.001f); // [0, 2] seconds
            }
            heaven::SceneMarkers markers;
            markers.times = heaven::normalize_marker_times(std::move(marker_times));
            RC_ASSERT(markers.count() >= size_t{1});

            // Pacing probe reports frame pacing is disabled, so the start
            // precondition for  is satisfied.
            heaven::BenchmarkSystem bench(&markers, &path,
                                          [] { return true; });

            heaven::Camera camera;

            // A run starts cleanly from Idle.
            RC_ASSERT(bench.request_start());
            RC_ASSERT(bench.state() == heaven::BenchmarkState::Running);

            // ── Tick the run mid-flight with strictly-positive deltas, keeping
            // the cumulative time far below kDuration so it stays Running. With
            // at most 50 ticks of <= 0.1 s each, the total is <= 5 s << 1000 s. ─
            const int num_ticks = *rc::gen::inRange<int>(1, 51);
            for (int i = 0; i < num_ticks; ++i) {
                const float dt =
                    static_cast<float>(*rc::gen::inRange<int>(1, 101)) *
                    0.001f; // [0.001, 0.1] seconds
                bench.tick(dt, camera);
            }
            RC_ASSERT(bench.state() == heaven::BenchmarkState::Running);

            // Snapshot the active run's state, playback time, frame count, and
            // scene index before any repeated start request.
            const heaven::BenchmarkStatus before = bench.status();
            RC_ASSERT(before.state == heaven::BenchmarkState::Running);

            // ── Repeated request_start() while Running: each returns false and
            // leaves the snapshot unchanged. ──────────────
            const int extra_starts = *rc::gen::inRange<int>(1, 6);
            for (int i = 0; i < extra_starts; ++i) {
                RC_ASSERT(!bench.request_start());

                const heaven::BenchmarkStatus after = bench.status();
                RC_ASSERT(after.state == heaven::BenchmarkState::Running);
                RC_ASSERT(after.elapsed == before.elapsed);
                RC_ASSERT(after.frames == before.frames);
                RC_ASSERT(after.scene_index == before.scene_index);
            }
        });
}

const heaven::testing::Register reg_start_ignored{&run_start_ignored_property};

} // namespace
