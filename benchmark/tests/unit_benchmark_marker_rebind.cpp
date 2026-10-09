/* HeavenBench — borrowed scene-marker replacement during scene reload. */
#include "benchmark/benchmark_system.h"
#include "engine/camera.h"

#include <cstdio>
#include <functional>
#include <optional>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool marker_rebind_check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[unit_benchmark_marker_rebind] FAILED: %s\n", message);
    }
    return condition;
}

bool unit_benchmark_marker_rebind() {
    heaven::CameraPath path;
    heaven::CameraKeyframe first;
    first.time = 0.0f;
    first.position = glm::vec3(0.0f);
    first.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    first.fov = 60.0f;
    auto last = first;
    last.time = 1.0f;
    last.position = glm::vec3(1.0f, 0.0f, 0.0f);
    path.keyframes = {first, last};
    path.duration = 1.0f;

    heaven::Camera camera;
    heaven::BenchmarkSystem benchmark(nullptr, &path, [] { return true; });
    if (!marker_rebind_check(!benchmark.request_start() &&
            benchmark.state() == heaven::BenchmarkState::Idle &&
            benchmark.status().scene_count == 0 &&
            !benchmark.status().has_result,
            "startup without markers must refuse a run")) return false;

    std::optional<heaven::SceneMarkers> markers(std::in_place);
    markers->times = {0.0f, 0.25f, 0.5f};
    benchmark.set_scene_markers(&*markers);
    if (!marker_rebind_check(benchmark.request_start(),
            "rebinding valid markers must permit the same camera path")) return false;
    benchmark.tick(0.3f, camera);
    const auto running = benchmark.status();
    if (!marker_rebind_check(benchmark.is_running() &&
            running.scene_count == 3 && running.scene_index == 1 &&
            running.frames == 1,
            "run must use the newly borrowed marker timeline")) return false;

    benchmark.set_scene_markers(nullptr);
    if (!marker_rebind_check(benchmark.state() == heaven::BenchmarkState::Cancelled &&
            benchmark.status().scene_count == 0 &&
            benchmark.status().frames == 0 && !benchmark.status().has_result,
            "detach must synchronously cancel and clear the active run")) return false;
    markers.reset(); // The borrowed object is destroyed only after detaching.
    benchmark.tick(0.1f, camera);
    if (!marker_rebind_check(benchmark.state() == heaven::BenchmarkState::Idle &&
            benchmark.status().scene_count == 0 && !benchmark.request_start(),
            "detached markers must remain safe through tick/status/start")) return false;

    markers.emplace();
    markers->times = {0.0f, 0.5f};
    benchmark.set_scene_markers(&*markers);
    if (!marker_rebind_check(benchmark.request_start(),
            "replacement markers must allow another run")) return false;
    benchmark.tick(0.5f, camera);
    const auto restarted = benchmark.status();
    if (!marker_rebind_check(restarted.scene_count == 2 &&
            restarted.scene_index == 1 && restarted.frames == 1 &&
            restarted.elapsed == 0.5,
            "replacement must reset stats and use its own marker count")) return false;
    benchmark.tick(0.5f, camera);
    const auto complete = benchmark.status();
    if (!marker_rebind_check(benchmark.state() == heaven::BenchmarkState::Complete &&
            complete.has_result && complete.scene_count == 2 &&
            complete.result.frames == 2 && complete.result.total_time == 1.0,
            "restarted run must complete without cancelled-run measurements")) return false;

    benchmark.set_scene_markers(nullptr);
    markers.reset();
    return marker_rebind_check(benchmark.state() == heaven::BenchmarkState::Idle &&
            benchmark.status().scene_count == 0 && !benchmark.status().has_result,
            "rebinding after completion must retire the borrowed result safely");
}

const heaven::testing::Register register_marker_rebind{unit_benchmark_marker_rebind};
} // namespace
