/*
 * HeavenBench — uncapped benchmark playback versus capped simulation time
 */
#include "benchmark/benchmark_system.h"
#include "benchmark/scene_markers.h"
#include "benchmark/stats.h"
#include "engine/camera.h"
#include "engine/camera_path.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

heaven::CameraPath fixture_path() {
    heaven::CameraPath path;
    heaven::CameraKeyframe first{};
    first.position = glm::vec3(0.0f);
    first.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    first.fov = 60.0f;
    heaven::CameraKeyframe last = first;
    last.time = 8.0f;
    last.position.x = 8.0f;
    path.keyframes = {first, last};
    path.duration = last.time;
    return path;
}

bool require(bool condition, const char* message) {
    if (!condition) {
        std::printf("[unit_benchmark_wall_clock] FAILED: %s\n", message);
    }
    return condition;
}

bool sustained_slow_frames() {
    const auto path = fixture_path();
    heaven::SceneMarkers markers;
    markers.times = {0.0f, 2.0f, 4.0f, 6.0f};
    heaven::BenchmarkSystem bench(&markers, &path, [] { return true; });
    heaven::Camera camera;
    heaven::BenchmarkStats frame_clock;
    auto now = heaven::BenchmarkStats::Clock::time_point{};
    frame_clock.reset_frame_clock(now);
    if (!bench.request_start()) return false;

    for (int frame = 1; frame <= 16; ++frame) {
        now += std::chrono::milliseconds(500);
        const float simulation_dt = frame_clock.begin_frame(now);
        if (!require(simulation_dt == 0.1f,
                     "slow-frame simulation lost its 100 ms cap") ||
            !require(frame_clock.raw_frame_delta() == 0.5f,
                     "the raw frame interval was capped")) {
            return false;
        }
        bench.tick(frame_clock.raw_frame_delta(), camera, false);
        const float elapsed = static_cast<float>(frame) * 0.5f;
        if (!require(std::abs(camera.position.x - elapsed) < 1e-5f,
                     "camera playback did not follow elapsed wall time") ||
            !require(bench.will_complete_on_present() == (frame == 16),
                     "playback did not reach the endpoint at eight seconds")) {
            return false;
        }
        bench.record_presented_frame(frame_clock.raw_frame_delta());
        if (!require(bench.status().scene_index ==
                         markers.scene_index_at(elapsed),
                     "scene markers did not follow the raw playback clock")) {
            return false;
        }
    }
    const auto result = bench.status();
    return require(result.has_result && result.result.total_time == 8.0 &&
                       result.result.frames == 16 &&
                       result.result.average_fps == 2.0,
                   "slow playback did not complete with uncapped statistics");
}

bool startup_and_cancelled_pause() {
    const auto path = fixture_path();
    heaven::SceneMarkers markers;
    markers.times = {0.0f};
    heaven::BenchmarkSystem bench(&markers, &path, [] { return true; });
    heaven::Camera camera;
    heaven::BenchmarkStats frame_clock;
    auto now = heaven::BenchmarkStats::Clock::time_point{};
    frame_clock.reset_frame_clock(now);

    // Loading and the synchronous CPU phase precede a successful run request.
    now += std::chrono::seconds(120);
    if (!bench.request_start()) return false;
    frame_clock.reset_frame_clock(now);
    if (!require(frame_clock.raw_frame_delta() == 0.0f,
                 "new run retained its pre-start delta")) return false;
    bench.tick(frame_clock.raw_frame_delta(), camera, false);
    if (!require(camera.position.x == 0.0f,
                 "startup elapsed time advanced the authored path")) return false;

    now += std::chrono::milliseconds(500);
    static_cast<void>(frame_clock.begin_frame(now));
    bench.tick(frame_clock.raw_frame_delta(), camera, false);
    bench.record_presented_frame(0.5f);
    bench.cancel();

    // Android lifecycle cancellation discards the old run. No new benchmark
    // clock may inherit the subsequent pause or pre-start CPU measurement.
    now += std::chrono::seconds(120);
    static_cast<void>(frame_clock.begin_frame(now));
    bench.tick(frame_clock.raw_frame_delta(), camera, false);
    if (!require(bench.state() == heaven::BenchmarkState::Idle &&
                     !bench.status().has_result && camera.position.x == 0.5f,
                 "cancelled playback advanced through the paused interval")) {
        return false;
    }
    if (!bench.request_start()) return false;
    frame_clock.reset_frame_clock(now);
    bench.tick(frame_clock.raw_frame_delta(), camera, false);
    now += std::chrono::milliseconds(250);
    static_cast<void>(frame_clock.begin_frame(now));
    bench.tick(frame_clock.raw_frame_delta(), camera, false);
    bench.record_presented_frame(0.25f);
    return require(camera.position.x == 0.25f &&
                       bench.status().elapsed == 0.25 &&
                       bench.status().frames == 1,
                   "restart inherited elapsed time from a cancelled pause");
}

bool rejected_render_attempts() {
    const auto path = fixture_path();
    heaven::SceneMarkers markers;
    markers.times = {0.0f};
    heaven::BenchmarkSystem bench(&markers, &path, [] { return true; });
    heaven::Camera camera;
    heaven::BenchmarkStats frame_clock;
    auto now = heaven::BenchmarkStats::Clock::time_point{};
    frame_clock.reset_frame_clock(now);
    if (!bench.request_start()) return false;

    for (int attempt = 0; attempt < 4; ++attempt) {
        now += std::chrono::milliseconds(250);
        static_cast<void>(frame_clock.begin_frame(now));
        bench.tick(frame_clock.raw_frame_delta(), camera, false);
    }
    if (!require(camera.position.x == 1.0f && bench.status().frames == 0,
                 "rejected draws changed playback rate or counted as frames")) {
        return false;
    }
    bench.record_presented_frame(1.0f);
    return require(bench.status().frames == 1 && bench.status().elapsed == 1.0,
                   "the next accepted frame lost rejected-attempt wall time");
}

bool unit_benchmark_wall_clock() {
    return sustained_slow_frames() && startup_and_cancelled_pause() &&
           rejected_render_attempts();
}

const heaven::testing::Register kRegister(unit_benchmark_wall_clock);

} // namespace
