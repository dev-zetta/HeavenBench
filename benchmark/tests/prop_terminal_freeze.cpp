/*
 * Statistics and playback time freeze once a terminal state is
 *             entered.
 *
 *
 * The implementation guarantees that that when a Benchmark_Run COMPLETES the Stats_Collector
 * stops accumulating further frames and elapsed time.  says that
 * when a run is CANCELLED the system stops advancing the playback clock and
 * stops the Stats_Collector. Together: once the system leaves the Running state
 * (into Complete or Cancelled), additional per-frame ticks must change neither
 * the collected statistics nor the playback time exposed through the overlay
 * snapshot.
 *
 * Both terminal paths are exercised here, in one check so the iteration count
 * covers both (RapidCheck runs >= 100 successful cases by default):
 *
 *   COMPLETE — request_start(); tick with positive deltas until playback time
 *     reaches/exceeds the path duration so state() == Complete. Capture the
 *     BenchmarkStatus snapshot (elapsed, frames, min/max FPS, average FPS, and
 *     the RunResult). Apply MANY more ticks with arbitrary deltas and assert the
 *     snapshot is byte-for-byte unchanged and state() stays Complete. The
 *     completed result is read from the frozen RunResult, so it must not drift.
 *
 *   CANCELLED — request_start(); tick a few times (without completing); then
 *     cancel(). cancel() discards the run's stats (frames/time reset, no result)
 *     and Cancelled is transient — the next tick settles to Idle. Capture the
 *     snapshot right after cancel() and assert that post-cancel ticks never
 *     resurrect run statistics: has_result stays false and frames/elapsed/
 *     average/min/max stay at their discarded (zero) values, i.e. the cancelled
 *     run never resumes accumulating.
 *
 * The post-terminal ticks deliberately draw from a WIDE delta range (including
 * zero and negative values) because tick() is contractually a no-op for the
 * camera, stats, and clock in every non-Running state, so any delta must leave
 * the frozen snapshot untouched. Keyframe rotations are identity quaternions
 * (sufficient: this property is about the stats/time freeze, not rotation
 * interpolation). Keyframe times are a strictly-increasing sum of positive gaps
 * so duration > 0 and the path is usable for request_start().
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

// Exact equality of the immutable overlay snapshot. Nothing mutates between
// captures in a terminal state (tick() early-returns), so the recomputed/stored
// fields are bit-identical and an exact comparison is the correct, strongest
// "frozen" assertion.
bool status_equal(const heaven::BenchmarkStatus& a,
                  const heaven::BenchmarkStatus& b) {
    return a.state == b.state && a.scene_index == b.scene_index &&
           a.scene_count == b.scene_count && a.average_fps == b.average_fps &&
           a.elapsed == b.elapsed && a.frames == b.frames &&
           a.min_fps == b.min_fps && a.max_fps == b.max_fps &&
           a.has_result == b.has_result &&
           a.result.average_fps == b.result.average_fps &&
           a.result.performance.scores.overall ==
               b.result.performance.scores.overall &&
           a.result.performance.scores.cpu ==
               b.result.performance.scores.cpu &&
           a.result.performance.scores.gpu ==
               b.result.performance.scores.gpu &&
           a.result.min_fps == b.result.min_fps &&
           a.result.max_fps == b.result.max_fps &&
           a.result.frames == b.result.frames &&
           a.result.total_time == b.result.total_time;
}

// Build a usable random camera path: >= 2 keyframes, t[0] = 0, strictly
// increasing times (positive gaps), identity rotations, bounded finite
// position/fov. duration = last keyframe time > 0.
heaven::CameraPath make_random_path() {
    const int n = *rc::gen::inRange<int>(2, 13);

    const auto coord_gen =
        rc::gen::map(rc::gen::inRange<int>(-100000, 100001),
                     [](int v) { return static_cast<float>(v) * 0.01f; });
    const auto fov_gen =
        rc::gen::map(rc::gen::inRange<int>(100, 1701),
                     [](int v) { return static_cast<float>(v) * 0.1f; });
    // Strictly-positive gaps (seconds) so every segment has positive length.
    const auto gap_gen =
        rc::gen::map(rc::gen::inRange<int>(1, 5001),
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
        kf.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity (w,x,y,z)
        kf.fov = *fov_gen;
        path.keyframes.push_back(kf);
    }
    path.duration = path.keyframes.back().time;
    return path;
}

bool run_terminal_freeze_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 8: statistics and playback time freeze once a terminal state "
        "is entered (Validates: Requirements 3.4, 4.2)",
        [] {
            heaven::CameraPath path = make_random_path();
            const float duration = path.duration;
            RC_ASSERT(duration > 0.0f);

            // Non-empty, non-decreasing markers derived from the duration so the
            // start preconditions are satisfied; the exact values do not matter
            // to the freeze contract.
            heaven::SceneMarkers markers;
            markers.times = {0.0f, duration * 0.3f, duration * 0.6f,
                             duration * 0.9f};

            // Pacing is "off" so request_start() is allowed to proceed.
            auto pacing_off = [] { return true; };

            // Wide delta generator for post-terminal ticks: tick() must be a
            // complete no-op in every non-Running state, so zero and negative
            // deltas are valid (and stronger) inputs here.
            const auto any_dt_gen =
                rc::gen::map(rc::gen::inRange<int>(-5000, 5001),
                             [](int v) { return static_cast<float>(v) * 0.01f; });

            // ── COMPLETE: stats + time freeze after the run completes ──────
            {
                heaven::BenchmarkSystem bench(&markers, &path, pacing_off);
                RC_ASSERT(bench.request_start());
                RC_ASSERT(bench.state() == heaven::BenchmarkState::Running);

                heaven::Camera cam;

                // Advance with positive deltas (a fraction of the duration each
                // tick) until the run completes. Each tick advances at least
                // 0.05 * duration, so completion is reached within ~20 ticks.
                const auto step_frac_gen =
                    rc::gen::map(rc::gen::inRange<int>(50, 351),
                                 [](int v) { return static_cast<float>(v) * 0.001f; });

                int guard = 0;
                while (bench.state() == heaven::BenchmarkState::Running &&
                       guard < 100000) {
                    bench.tick(duration * (*step_frac_gen), cam);
                    ++guard;
                }
                RC_ASSERT(bench.state() == heaven::BenchmarkState::Complete);

                // Frozen snapshot captured the moment the run completes.
                const heaven::BenchmarkStatus frozen = bench.status();
                RC_ASSERT(frozen.state == heaven::BenchmarkState::Complete);
                RC_ASSERT(frozen.has_result);

                // MANY more ticks with arbitrary deltas must not change a thing.
                const int extra = *rc::gen::inRange<int>(5, 51);
                for (int i = 0; i < extra; ++i) {
                    bench.tick(*any_dt_gen, cam);
                    RC_ASSERT(bench.state() == heaven::BenchmarkState::Complete);
                    RC_ASSERT(status_equal(bench.status(), frozen));
                }
            }

            // ── CANCELLED: stats + time stay discarded after cancellation ──
            {
                heaven::BenchmarkSystem bench(&markers, &path, pacing_off);
                RC_ASSERT(bench.request_start());

                heaven::Camera cam;

                // A few Running ticks that do NOT complete the run: at most 5
                // ticks of <= 0.05 * duration each (cumulative <= 0.25 *
                // duration < duration).
                const int pre = *rc::gen::inRange<int>(0, 6);
                const auto small_frac_gen =
                    rc::gen::map(rc::gen::inRange<int>(10, 51),
                                 [](int v) { return static_cast<float>(v) * 0.001f; });
                for (int i = 0;
                     i < pre && bench.state() == heaven::BenchmarkState::Running;
                     ++i) {
                    bench.tick(duration * (*small_frac_gen), cam);
                }

                // Cancel discards the run's stats and exposes no result.
                bench.cancel();
                const heaven::BenchmarkStatus frozen = bench.status();
                RC_ASSERT(!frozen.has_result);
                RC_ASSERT(frozen.frames == 0u);
                RC_ASSERT(frozen.elapsed == 0.0);
                RC_ASSERT(frozen.average_fps == 0.0);

                // Post-cancel ticks must never resurrect run statistics or the
                // playback time: the cancelled run never resumes accumulating.
                const int extra = *rc::gen::inRange<int>(5, 51);
                for (int i = 0; i < extra; ++i) {
                    bench.tick(*any_dt_gen, cam);
                    const heaven::BenchmarkStatus now = bench.status();
                    RC_ASSERT(status_equal(now, frozen));
                    RC_ASSERT(!now.has_result);
                    RC_ASSERT(now.frames == 0u);
                    RC_ASSERT(now.elapsed == 0.0);
                }
            }
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_terminal_freeze{&run_terminal_freeze_property};

} // namespace
