/*
 * HeavenBench — unit tests: run lifecycle examples
 *
 *
 *
 * These example tests pin down three lifecycle behaviours of BenchmarkSystem
 * that the property tests cover only in aggregate:
 *
 *   1. Cancel at t=0  — starting a run and cancelling it
 *      before any frame is ticked takes effect synchronously (state() reports
 *      Cancelled the instant cancel() returns, i.e. in the same frame the key
 *      press is detected, before the next frame is rendered) and the snapshot
 *      exposes zero time / zero score and no result.
 *   2. ESC-style mid-run cancel  — cancelling after a few
 *      ticks is equally synchronous and still exposes no result or score.
 *   3. dismiss_result() Complete -> Idle  — completing a run
 *      shows a result; dismissing it returns to Idle and clears the result,
 *      while dismiss_result() is a no-op in any non-Complete state.
 *
 * The translation unit self-registers with the shared `heaven_benchmark_tests`
 * harness (test_main.cpp) at static-init time via heaven::testing::Register,
 * matching the other test TUs; the file(GLOB) in benchmark/CMakeLists.txt picks
 * it up without editing the entry point. No second main() is defined. The
 * registered callable returns true on success and false (after printing a
 * diagnostic) on the first failing check.
 */
#include <cstdio>
#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "benchmark/benchmark_system.h"
#include "benchmark/scene_markers.h"
#include "engine/camera.h"
#include "engine/camera_path.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is identical across test TUs (ODR-safe).
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// A minimal but usable CameraPath: two keyframes starting at t=0 with a
// positive duration, so request_start() accepts the assets (>= 2 keyframes,
// duration > 0). The camera transform values are arbitrary; the lifecycle
// behaviour under test does not depend on them.
heaven::CameraPath make_fixture_path() {
    heaven::CameraPath path;

    heaven::CameraKeyframe k0;
    k0.time     = 0.0f;
    k0.position = glm::vec3(0.0f, 0.0f, 0.0f);
    k0.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity (w,x,y,z)
    k0.fov      = 60.0f;

    heaven::CameraKeyframe k1;
    k1.time     = 1.0f;
    k1.position = glm::vec3(1.0f, 0.0f, 0.0f);
    k1.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    k1.fov      = 60.0f;

    path.keyframes = {k0, k1};
    path.duration  = 1.0f;
    return path;
}

// ── Case 1: cancel at t=0 yields zero-time / zero-score, no result ──────────
bool lifecycle_cancel_at_zero() {
    const heaven::CameraPath path = make_fixture_path();

    // Non-empty markers + pacing-off probe so the start is accepted.
    heaven::SceneMarkers markers;
    markers.times = {0.0f};

    heaven::BenchmarkSystem bench(&markers, &path, [] { return true; });
    heaven::Camera camera;

    if (!bench.request_start()) {
        std::printf("[unit_lifecycle] FAILED: request_start() returned false "
                    "for a valid fixture (cancel-at-zero case)\n");
        return false;
    }
    if (bench.state() != heaven::BenchmarkState::Running) {
        std::printf("[unit_lifecycle] FAILED: state is not Running after a "
                    "successful start (cancel-at-zero case)\n");
        return false;
    }

    // Cancel immediately, WITHOUT ticking (playback time is still 0). The
    // cancel must take effect in the same call: state() reports Cancelled the
    // instant cancel() returns (— "in the same frame the key
    // press is detected, before the next frame is rendered").
    bench.cancel();
    if (bench.state() != heaven::BenchmarkState::Cancelled) {
        std::printf("[unit_lifecycle] FAILED: cancel() did not take effect "
                    "synchronously; state() != Cancelled right after the "
                    "call (cancel-at-zero case)\n");
        return false;
    }

    // The snapshot exposes zero time / zero score and no result.
    const heaven::BenchmarkStatus s = bench.status();
    if (s.has_result) {
        std::printf("[unit_lifecycle] FAILED: cancelled run exposes a result "
                    "(has_result == true) at t=0\n");
        return false;
    }
    if (s.average_fps != 0.0 || s.elapsed != 0.0 || s.frames != 0 ||
        s.result.performance.scores.overall != 0.0 ||
        s.result.performance.scores.cpu != 0.0 ||
        s.result.performance.scores.gpu != 0.0) {
        std::printf("[unit_lifecycle] FAILED: cancel at t=0 did not yield "
                    "zero time/score: average_fps=%.17g elapsed=%.17g "
                    "frames=%llu overall_score=%.17g\n",
                    s.average_fps, s.elapsed,
                    static_cast<unsigned long long>(s.frames),
                    s.result.performance.scores.overall);
        return false;
    }

    return true;
}

// ── Case 2: ESC-style mid-run cancel takes effect in the same call ──────────
bool lifecycle_cancel_mid_run() {
    const heaven::CameraPath path = make_fixture_path();

    heaven::SceneMarkers markers;
    markers.times = {0.0f};

    heaven::BenchmarkSystem bench(&markers, &path, [] { return true; });
    heaven::Camera camera;

    if (!bench.request_start() ||
        bench.state() != heaven::BenchmarkState::Running) {
        std::printf("[unit_lifecycle] FAILED: could not start a run "
                    "(mid-run cancel case)\n");
        return false;
    }

    // Tick a few frames with small deltas that stay well below the 1.0 s
    // duration, so the run remains Running rather than completing.
    for (int i = 0; i < 5; ++i) {
        bench.tick(0.01f, camera);
        if (bench.state() != heaven::BenchmarkState::Running) {
            std::printf("[unit_lifecycle] FAILED: run left Running after a "
                        "small tick (mid-run cancel case, frame %d)\n", i);
            return false;
        }
    }

    // ESC-style cancel mid-run: effect is synchronous, the same as at t=0.
    bench.cancel();
    if (bench.state() != heaven::BenchmarkState::Cancelled) {
        std::printf("[unit_lifecycle] FAILED: mid-run cancel() did not take "
                    "effect synchronously; state() != Cancelled right after "
                    "the call\n");
        return false;
    }

    // No result and no score are exposed for the cancelled run.
    const heaven::BenchmarkStatus s = bench.status();
    if (s.has_result ||
        s.result.performance.scores.overall != 0.0 ||
        s.result.performance.scores.cpu != 0.0 ||
        s.result.performance.scores.gpu != 0.0 ||
        s.average_fps != 0.0) {
        std::printf("[unit_lifecycle] FAILED: mid-run cancel exposed a "
                    "result/score: has_result=%d overall_score=%.17g "
                    "average_fps=%.17g\n",
                    static_cast<int>(s.has_result),
                    s.result.performance.scores.overall,
                    s.average_fps);
        return false;
    }

    return true;
}

// ── Case 3: dismiss_result() returns Complete -> Idle  ─────
bool lifecycle_dismiss_result() {
    const heaven::CameraPath path = make_fixture_path();

    heaven::SceneMarkers markers;
    markers.times = {0.0f};

    heaven::BenchmarkSystem bench(&markers, &path, [] { return true; });
    heaven::Camera camera;

    // dismiss_result() is a no-op outside the Complete state: from Idle it must
    // leave the system Idle.
    bench.dismiss_result();
    if (bench.state() != heaven::BenchmarkState::Idle) {
        std::printf("[unit_lifecycle] FAILED: dismiss_result() from Idle did "
                    "not leave the system Idle\n");
        return false;
    }

    if (!bench.request_start() ||
        bench.state() != heaven::BenchmarkState::Running) {
        std::printf("[unit_lifecycle] FAILED: could not start a run "
                    "(dismiss-result case)\n");
        return false;
    }

    // One tick whose delta reaches the path duration completes the run
    // (at_end uses >=, and 1.0 >= 1.0).
    bench.tick(1.0f, camera);
    if (bench.state() != heaven::BenchmarkState::Complete) {
        std::printf("[unit_lifecycle] FAILED: a tick reaching the path "
                    "duration did not complete the run (state != Complete)\n");
        return false;
    }

    // A completed run presents a result report.
    if (!bench.status().has_result) {
        std::printf("[unit_lifecycle] FAILED: completed run does not expose a "
                    "result (has_result == false)\n");
        return false;
    }

    // Dismissing the result returns to Idle and clears the result.
    bench.dismiss_result();
    if (bench.state() != heaven::BenchmarkState::Idle) {
        std::printf("[unit_lifecycle] FAILED: dismiss_result() did not return "
                    "the system to Idle from Complete\n");
        return false;
    }
    if (bench.status().has_result) {
        std::printf("[unit_lifecycle] FAILED: result still exposed after "
                    "dismiss_result() (has_result == true)\n");
        return false;
    }

    return true;
}

bool lifecycle_hitch_measurement() {
    const heaven::CameraPath path = make_fixture_path();
    heaven::SceneMarkers markers;
    markers.times = {0.0f};
    heaven::BenchmarkSystem bench(&markers, &path, [] { return true; });
    heaven::Camera camera;
    if (!bench.request_start()) return false;

    bench.tick(0.5f, camera);
    const auto running = bench.status();
    bench.tick(0.5f, camera);
    const auto complete = bench.status();
    if (running.elapsed != 0.5 || running.average_fps != 2.0 ||
        running.min_fps != 2.0f || running.frames != 1 ||
        !complete.has_result || complete.result.total_time != 1.0 ||
        complete.result.average_fps != 2.0 ||
        complete.result.min_fps != 2.0f || complete.result.frames != 2) {
        std::printf("[unit_lifecycle] FAILED: live and completed statistics "
                    "must include the entire accepted-frame interval\n");
        return false;
    }
    return true;
}

bool unit_lifecycle() {
    bool ok = true;
    ok = lifecycle_cancel_at_zero() && ok;
    ok = lifecycle_cancel_mid_run() && ok;
    ok = lifecycle_dismiss_result() && ok;
    ok = lifecycle_hitch_measurement() && ok;
    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_lifecycle{unit_lifecycle};

} // namespace
