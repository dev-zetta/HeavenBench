/*
 * A cancelled run never exposes a result.
 *
 *
 * The implementation guarantees that cancelling a run returns the renderer to its pre-run
 * interactive state (free-fly + Menu_Bar) WITHOUT presenting a Result_Report.
 * The implementation guarantees that the statistics collected during the cancelled run are
 * discarded so the Overlay_UI presents NO Result_Report, Score, or run
 * statistics from that run.
 *
 * BenchmarkSystem encodes this with a transient Cancelled state: cancel()
 * discards the StatsCollector contents and the result and flips to Cancelled,
 * which status() reports to the overlay as a clean Idle snapshot (no stats, no
 * result). The next tick() settles Cancelled to Idle so the renderer is fully
 * interactive again.
 *
 * For any random cancel time (a random number of frames at random positive
 * deltas, kept strictly under the path duration so the run is genuinely still
 * Running when cancelled), this property asserts that immediately after
 * cancel():
 *
 *   - status().has_result == false
 *   - the snapshot presents as Idle, NOT Running, so the overlay shows the
 *     Menu_Bar rather than the running overlay
 *   - no run statistics or score leak through the snapshot:
 *       average_fps == 0, elapsed == 0, frames == 0,
 *       min_fps == 0, max_fps == 0,
 *       all result scores == 0, result.average_fps == 0
 *
 * and that after one further tick() (Cancelled -> Idle):
 *
 *   - state() is Idle, status() still exposes no result/score/stats.
 *
 * The run accumulates real frames/time before the cancel (each tick feeds the
 * collector), so a cancel that failed to discard the stats WOULD show nonzero
 * figures here — the property is non-vacuous.
 *
 * The fixture uses a CameraPath with several keyframes starting at t = 0 and a
 * large duration (1000 s) so the random ticks (bounded well below 1000 s) never
 * reach end-of-path and prematurely Complete the run; rotations are identity
 * (the camera math is not under test here). The SceneMarkers list is non-empty
 * so request_start() accepts the run, and the frame-pacing probe reports true
 * so pacing never blocks the start (is covered elsewhere).
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

// A camera path with a large duration so random ticks stay well short of the
// end and the run remains Running until we cancel it. Several keyframes with
// identity rotation (the camera interpolation is not the subject of this
// property) starting at t = 0; duration is the final keyframe time.
heaven::CameraPath make_long_path() {
    heaven::CameraPath path;
    const float times[] = {0.0f, 250.0f, 500.0f, 750.0f, 1000.0f};
    for (float t : times) {
        heaven::CameraKeyframe kf;
        kf.time     = t;
        kf.position = glm::vec3(t, -t, 0.5f * t);
        kf.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity (w,x,y,z)
        kf.fov      = 60.0f;
        path.keyframes.push_back(kf);
    }
    path.duration = path.keyframes.back().time;
    return path;
}

bool run_cancel_no_result_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 9: a cancelled run never exposes a result "
        "(Validates: Requirements 4.3, 4.4)",
        [] {
            const heaven::CameraPath path = make_long_path();

            // Non-empty scene markers so request_start() accepts the run. A
            // random, non-decreasing list within the path range exercises the
            // scene-index bookkeeping that cancel() must NOT surface afterwards.
            const auto marker_gen = rc::gen::map(
                rc::gen::inRange<int>(0, 1000),
                [](int v) { return static_cast<float>(v); });
            std::vector<float> raw =
                *rc::gen::container<std::vector<float>>(marker_gen);
            raw.push_back(*marker_gen); // force non-empty
            heaven::SceneMarkers markers;
            markers.times = heaven::normalize_marker_times(std::move(raw));
            RC_ASSERT(markers.count() >= size_t{1});

            // Frame pacing is "off", so the start precondition (Requirement
            // is satisfied and never blocks this run.
            heaven::BenchmarkSystem bench(
                &markers, &path, [] { return true; });

            heaven::Camera camera;

            // Start the run: with usable assets and pacing off this must succeed
            // and enter Running.
            RC_ASSERT(bench.request_start());
            RC_ASSERT(bench.is_running());

            // Cancel at an arbitrary playback time: run a random number of
            // frames with random positive deltas. Each delta is in [0.001, 1.0]
            // s and there are at most 100 of them, so the cumulative playback
            // time is at most 100 s — far below the 1000 s duration — and the
            // run never reaches end-of-path and Completes before we cancel.
            const auto dt_gen =
                rc::gen::map(rc::gen::inRange<int>(1, 1001),
                             [](int milli) {
                                 return static_cast<float>(milli) / 1000.0f;
                             });
            const int frames = *rc::gen::inRange<int>(0, 101);
            for (int i = 0; i < frames; ++i) {
                bench.tick(*dt_gen, camera);
            }

            // We kept the cumulative time well under the duration, so the run is
            // still genuinely Running (not already Complete) at the cancel point.
            RC_ASSERT(bench.is_running());

            // ── Cancel ────────────────────────────────────────────────
            bench.cancel();

            // Immediately after cancel the snapshot must expose NO result and
            // present as a clean Idle (Menu_Bar) snapshot rather than Running
            // with every run statistic and score discarded

            const heaven::BenchmarkStatus after_cancel = bench.status();
            RC_ASSERT(after_cancel.has_result == false);
            RC_ASSERT(after_cancel.state == heaven::BenchmarkState::Idle);
            RC_ASSERT(after_cancel.state != heaven::BenchmarkState::Running);
            RC_ASSERT(after_cancel.average_fps == 0.0);
            RC_ASSERT(after_cancel.elapsed == 0.0);
            RC_ASSERT(after_cancel.frames == uint64_t{0});
            RC_ASSERT(after_cancel.min_fps == 0.0f);
            RC_ASSERT(after_cancel.max_fps == 0.0f);
            RC_ASSERT(after_cancel.result.performance.scores.overall == 0.0);
            RC_ASSERT(after_cancel.result.performance.scores.cpu == 0.0);
            RC_ASSERT(after_cancel.result.performance.scores.gpu == 0.0);
            RC_ASSERT(after_cancel.result.average_fps == 0.0);

            // One further tick settles the transient Cancelled state to Idle so
            // the renderer is fully interactive again. The
            // snapshot must still expose no result, score, or run statistics.
            bench.tick(*dt_gen, camera);
            RC_ASSERT(bench.state() == heaven::BenchmarkState::Idle);

            const heaven::BenchmarkStatus after_settle = bench.status();
            RC_ASSERT(after_settle.has_result == false);
            RC_ASSERT(after_settle.state == heaven::BenchmarkState::Idle);
            RC_ASSERT(after_settle.average_fps == 0.0);
            RC_ASSERT(after_settle.elapsed == 0.0);
            RC_ASSERT(after_settle.frames == uint64_t{0});
            RC_ASSERT(after_settle.min_fps == 0.0f);
            RC_ASSERT(after_settle.max_fps == 0.0f);
            RC_ASSERT(after_settle.result.performance.scores.overall == 0.0);
            RC_ASSERT(after_settle.result.performance.scores.cpu == 0.0);
            RC_ASSERT(after_settle.result.performance.scores.gpu == 0.0);
            RC_ASSERT(after_settle.result.average_fps == 0.0);
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_cancel_no_result{
    &run_cancel_no_result_property};

} // namespace
