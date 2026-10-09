/*
 * HeavenBench — Property test for a successful start reset
 *
 * A successful start resets the clock, stats, and scene index and
 *             enters Running.
 *
 *
 * say that when a Benchmark_Run starts, the system resets
 * the playback time to zero (the beginning of the authored path), resets the
 * Stats_Collector, and sets the Scene_Index to zero, entering Running. This
 * must hold REGARDLESS of any state accumulated by a prior run.
 *
 * BenchmarkSystem::request_start() refuses only while a run is already active
 * (state == Running); from every non-Running state (Idle, Complete, Cancelled)
 * a start succeeds when the assets are usable (non-empty markers, >= 2 camera
 * keyframes) and the frame-pacing probe reports pacing is disabled. So to test
 * "regardless of prior accumulated time/stats" this property drives the system
 * through one of three prior histories before the start under test:
 *
 *   0. Fresh:     no prior run — the system is Idle.
 *   1. Cancelled: start a run, advance it a random number of frames so its
 *                 clock/stats/scene index move off their initial values, then
 *                 cancel() (-> Cancelled) and tick once so it settles to Idle.
 *   2. Completed: start a run and tick it past the path duration so it enters
 *                 Complete with NON-ZERO frozen frames/elapsed/scene index and
 *                 a result report still present.
 *
 * From that prior (non-Running) state, request_start() under test must:
 *   - return true and leave the system in Running
 *   - reset the playback clock           -> status().elapsed == 0
 *   - reset the Stats_Collector          -> frames == 0, average/min/max == 0
 *   - reset the Scene_Index              -> scene_index == 0
 *
 * The reset snapshot is read immediately after the successful start, BEFORE any
 * further tick, so the asserted values are the freshly-reset initial values.
 *
 * Test fixture: a usable CameraPath (n >= 2 strictly-increasing keyframes from
 * t = 0, so duration > 0), a non-empty SceneMarkers list spanning [0, duration]
 * (normalized non-decreasing), a pacing probe reporting true (pacing disabled),
 * and a heaven::Camera. Keyframe rotations are identity to sidestep slerp edge
 * cases; only the lifecycle/reset behaviour is under test here.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/benchmark_system.h"
#include "benchmark/scene_markers.h"
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

bool run_start_reset_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 6: a successful start resets the clock, stats, and scene "
        "index and enters Running (Validates: Requirements 3.2, 3.3)",
        [] {
            // ── Build a usable camera path: n >= 2 keyframes, t[0] = 0, then
            // strictly-increasing positive gaps, so duration > 0. ──────────
            const int n = *rc::gen::inRange<int>(2, 10);

            const auto coord_gen =
                rc::gen::map(rc::gen::inRange<int>(-100000, 100001),
                             [](int v) { return static_cast<float>(v) * 0.01f; });
            const auto fov_gen =
                rc::gen::map(rc::gen::inRange<int>(100, 1701),
                             [](int v) { return static_cast<float>(v) * 0.1f; });
            // Per-keyframe gaps in [0.1, 2.0] s so duration is comfortably > 0.
            const auto gap_gen =
                rc::gen::map(rc::gen::inRange<int>(100, 2001),
                             [](int v) { return static_cast<float>(v) * 0.001f; });

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
                kf.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity
                kf.fov = *fov_gen;
                path.keyframes.push_back(kf);
            }
            path.duration = path.keyframes.back().time;
            RC_ASSERT(path.duration > 0.0f);

            // ── Build a non-empty, non-decreasing marker list in
            // [0, duration] so the scene index can advance off zero during a
            // prior run. ──────────────────────────────────────────────────
            const int m = *rc::gen::inRange<int>(1, 9);
            std::vector<float> raw;
            raw.reserve(static_cast<size_t>(m));
            for (int i = 0; i < m; ++i) {
                const float frac =
                    static_cast<float>(*rc::gen::inRange<int>(0, 1001)) / 1000.0f;
                raw.push_back(path.duration * frac);
            }
            heaven::SceneMarkers markers;
            markers.times = heaven::normalize_marker_times(std::move(raw));
            RC_ASSERT(markers.count() >= size_t{1});

            // Pacing probe reports pacing disabled, so a start is allowed.
            heaven::BenchmarkSystem bench(
                &markers, &path, [] { return true; });

            heaven::Camera camera;

            // ── Drive one of three prior histories before the start under
            // test, so the property holds "regardless of prior accumulated
            // time/stats". ────────────────────────────────────────────────
            const int scenario = *rc::gen::inRange<int>(0, 3);

            // Small per-frame deltas that keep a run comfortably below the path
            // duration: k <= 20 frames of <= duration/50 each sums to <= 0.4 *
            // duration, so a partial run stays Running.
            const auto small_dt_gen = rc::gen::map(
                rc::gen::inRange<int>(1, 1001), [&](int v) {
                    return (static_cast<float>(v) / 1000.0f) *
                           (path.duration / 50.0f);
                });

            if (scenario == 1) {
                // Cancelled path: accumulate some clock/stats/scene-index
                // movement, then cancel and settle to Idle.
                RC_ASSERT(bench.request_start());
                const int k = *rc::gen::inRange<int>(1, 21);
                for (int i = 0; i < k; ++i) {
                    bench.tick(*small_dt_gen, camera);
                }
                bench.cancel();
                bench.tick(0.001f, camera); // Cancelled -> Idle
            } else if (scenario == 2) {
                // Completed path: accumulate frames, then force completion with
                // a final delta that pushes playback time past the duration.
                RC_ASSERT(bench.request_start());
                const int k = *rc::gen::inRange<int>(1, 21);
                for (int i = 0; i < k; ++i) {
                    bench.tick(*small_dt_gen, camera);
                }
                bench.tick(path.duration, camera); // playback time >= duration
                RC_ASSERT(bench.state() == heaven::BenchmarkState::Complete);
                // Sanity: the prior run really did accumulate non-initial state,
                // so the reset below is meaningful (not a no-op on zeros).
                const heaven::BenchmarkStatus prior = bench.status();
                RC_ASSERT(prior.frames > uint64_t{0});
                RC_ASSERT(prior.elapsed > 0.0);
            }
            // scenario == 0: no prior run; the system is Idle.

            // The prior history must leave the system in a non-Running state
            // from which a start can succeed.
            RC_ASSERT(bench.state() != heaven::BenchmarkState::Running);

            // ── The start under test: it must succeed and reset everything to
            // initial, entering Running. ────────────
            const bool started = bench.request_start();
            RC_ASSERT(started);
            RC_ASSERT(bench.state() == heaven::BenchmarkState::Running);
            RC_ASSERT(bench.is_running());

            // Read the snapshot BEFORE any further tick so the values are the
            // freshly-reset initial values.
            const heaven::BenchmarkStatus s = bench.status();
            RC_ASSERT(s.state == heaven::BenchmarkState::Running);
            RC_ASSERT(s.elapsed == 0.0);          // clock reset
            RC_ASSERT(s.frames == uint64_t{0});   // stats reset
            RC_ASSERT(s.scene_index == size_t{0}); // scene index reset
            RC_ASSERT(s.average_fps == 0.0);       // no measured time yet
            RC_ASSERT(s.min_fps == 0.0f);          // no valid frame yet
            RC_ASSERT(s.max_fps == 0.0f);
            RC_ASSERT(!s.has_result);              // any prior result cleared
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_start_reset{&run_start_reset_property};

} // namespace
