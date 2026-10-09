/* Recording taints only the tour it overlaps, including recording at start. */
#include "benchmark/benchmark_system.h"
#include "benchmark/result_report.h"
#include "engine/camera.h"
#include "json.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}
namespace {
bool check(bool value, const char* message) {
    if (!value) std::printf("[unit_microprofiler_scoring] %s\n", message);
    return value;
}
bool microprofiler_scoring() {
    heaven::CameraPath path;
    heaven::CameraKeyframe first;
    first.time = 0.0f;
    first.position = glm::vec3(0.0f);
    first.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    first.fov = 60.0f;
    auto last = first;
    last.time = 1.0f;
    path.keyframes = {first, last};
    path.duration = 1.0f;
    heaven::SceneMarkers markers;
    markers.times = {0.0f};
    heaven::BenchmarkSystem system(&markers, &path, [] { return true; });
    heaven::Camera camera;
    if (!system.request_start()) return false;
    system.tick(0.25f, camera);
    system.set_microprofile_recording(true);
    system.set_microprofile_recording(false);
    if (!check(system.status().diagnostic, "stopping trace cleared sticky diagnostic state")) return false;
    system.tick(0.75f, camera);
    const auto recorded = system.status();
    if (!check(recorded.has_result && recorded.result.diagnostic && recorded.result.frames == 2 &&
               recorded.result.total_time == 1.0 && recorded.result.average_fps == 2.0 &&
               recorded.result.performance.scores.overall == 0.0,
               "diagnostic tour must retain stats and suppress score")) return false;
    const auto doc = heaven::build_benchmark_report(recorded.result, heaven::BenchConfig{},
        {"test", "test"}, std::chrono::system_clock::time_point{});
    const auto json = nlohmann::json::parse(doc.json);
    if (!check(json.at("run").at("diagnostic") == true && json.at("run").at("scored") == false &&
               json.at("scores").at("overall").is_null() && json.at("scores").at("cpu").is_null() &&
               json.at("scores").at("gpu").is_null() && doc.html.find("Unscored (diagnostic)") != std::string::npos,
               "diagnostic JSON/HTML still awarded scores")) return false;
    system.dismiss_result();
    if (!system.request_start()) return false;
    system.tick(1.0f, camera);
    const auto clean = system.status();
    const auto clean_doc = heaven::build_benchmark_report(clean.result, heaven::BenchConfig{},
        {"test", "test"}, std::chrono::system_clock::time_point{});
    const auto clean_json = nlohmann::json::parse(clean_doc.json);
    if (!check(!clean.diagnostic && !clean.result.diagnostic && clean.result.performance.scores.overall > 0 &&
               !clean_json.at("run").contains("diagnostic") && !clean_json.at("run").contains("scored") &&
               clean_json.at("scores").at("version") == heaven::kScoreVersion &&
               clean_json.at("schema_version") == heaven::kResultReportSchemaVersion,
               "later clean tour/schema/score version changed")) return false;
    system.dismiss_result();
    system.set_microprofile_recording(true);
    if (!system.request_start()) return false;
    system.set_microprofile_recording(false);
    system.tick(1.0f, camera);
    if (!check(system.status().result.diagnostic, "recording at tour start was not sticky")) return false;
    system.dismiss_result();
    if (!system.request_start()) return false;
    system.set_microprofile_recording(true);
    system.cancel();
    system.set_microprofile_recording(false);
    system.tick(0.1f, camera);
    return check(!system.status().has_result && !system.status().diagnostic &&
                 system.request_start() && !system.status().diagnostic,
                 "cancelled diagnostic state leaked into next tour");
}
const heaven::testing::Register registered{microprofiler_scoring};
}
