/*
 * HeavenBench — unit tests: same-frame System/Settings gathering + logging
 *
 *
 *
 * These example tests pin down the completion-time behaviour of
 * BenchmarkSystem::enter_complete(): the injected SystemInfoGatherer /
 * SettingsInfoGatherer are invoked synchronously inside the completing tick —
 * within the frame the run completes and BEFORE the overlay first builds the
 * Result_Report — so that status().result.system /.settings carry the gathered
 * descriptions the moment tick() returns.
 *
 *   1. Same-frame population — a run driven to Complete with stub gatherers
 *      returning canned SystemInfo / SettingsInfo has all three System fields
 *      (Platform / CPU / GPU) and all three Settings fields (Render API /
 *      Display / Quality) fully populated right after the completing tick, with
 *      no overlay build in between.
 *   2. Undetermined datum -> placeholder, report not skipped — a gatherer that
 *      returns the Unknown_Value_Placeholder for a datum (it returns formatted
 *      strings, so an undetermined datum already carries the placeholder) still
 *      reaches Complete with has_result true; the placeholder is surfaced in the
 *      result rather than the report being skipped (.x).
 *   3. Application log — the completing tick writes the run statistics together
 *      with the System_Info and Settings_Info lines to the application log
 *      (stderr). stderr is redirected to a temp file across the completing tick
 *      via dup/dup2, then restored, and the captured text is asserted to contain
 *      the run-stats line and both info lines.
 *
 * The translation unit self-registers with the shared `heaven_benchmark_tests`
 * harness (test_main.cpp) at static-init time via heaven::testing::Register,
 * matching the other test TUs; the file(GLOB) in benchmark/CMakeLists.txt picks
 * it up without editing the entry point. No second main() is defined.
 */
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "stdio_compat.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "benchmark/benchmark_system.h"
#include "benchmark/run_info.h"
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
// duration > 0). A single tick of dt == duration drives the run to Complete.
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

// Canned, fully-determined System_Info. The gatherer returns formatted strings,
// so these stand in for what the real LinuxHostInfoSource / backend composition
// would produce.
heaven::SystemInfo canned_system() {
    heaven::SystemInfo s;
    s.platform = "TestOS 1.2.3-test x86_64";
    s.cpu      = "Test CPU Model (2295MHz) x32";
    s.gpu      = "Test GPU Model (8192MB)";
    return s;
}

heaven::SettingsInfo canned_settings() {
    heaven::SettingsInfo s;
    s.render_api     = "Vulkan";
    s.display_mode   = "1600x900 8xAA windowed";
    s.quality_preset = "Extreme";
    return s;
}

// Drive a freshly-started run to Complete with a single tick of dt == duration.
// Returns false (after printing a diagnostic) if the run did not start or
// complete as expected.
bool drive_to_complete(heaven::BenchmarkSystem& bench, heaven::Camera& camera,
                       const char* case_name) {
    if (!bench.request_start() ||
        bench.state() != heaven::BenchmarkState::Running) {
        std::printf("[unit_complete_info] FAILED: could not start a run (%s)\n",
                    case_name);
        return false;
    }
    // dt == duration: at_end() uses >=, so 1.0 >= 1.0 completes the run on this
    // single tick — the same tick whose enter_complete() does the gathering.
    bench.tick(1.0f, camera);
    if (bench.state() != heaven::BenchmarkState::Complete) {
        std::printf("[unit_complete_info] FAILED: tick reaching the duration "
                    "did not complete the run (%s)\n", case_name);
        return false;
    }
    return true;
}

// ── Case 1: gathered info is populated same-frame, before any overlay build ──
bool complete_info_same_frame_population() {
    const heaven::CameraPath path = make_fixture_path();
    heaven::SceneMarkers markers;
    markers.times = {0.0f};

    const heaven::SystemInfo   sys = canned_system();
    const heaven::SettingsInfo set = canned_settings();

    heaven::BenchmarkSystem bench(
        &markers, &path, [] { return true; },
        canned_system, canned_settings);
    heaven::Camera camera;

    if (!drive_to_complete(bench, camera, "same-frame population")) {
        return false;
    }

    // Read the snapshot directly after the completing tick — this is the first
    // read, standing in for "before any overlay build". The gatherers must have
    // already run inside enter_complete().
    const heaven::BenchmarkStatus s = bench.status();
    if (!s.has_result) {
        std::printf("[unit_complete_info] FAILED: completed run exposes no "
                    "result (has_result == false)\n");
        return false;
    }

    // All three System fields and all three Settings fields are fully populated
    // (non-empty) and carry the canned descriptions.
    const heaven::SystemInfo&   rs = s.result.system;
    const heaven::SettingsInfo& rt = s.result.settings;
    if (rs.platform != sys.platform || rs.cpu != sys.cpu || rs.gpu != sys.gpu) {
        std::printf("[unit_complete_info] FAILED: System_Info not populated "
                    "from the gatherer: platform='%s' cpu='%s' gpu='%s'\n",
                    rs.platform.c_str(), rs.cpu.c_str(), rs.gpu.c_str());
        return false;
    }
    if (rt.render_api != set.render_api ||
        rt.display_mode != set.display_mode ||
        rt.quality_preset != set.quality_preset) {
        std::printf("[unit_complete_info] FAILED: Settings_Info not populated "
                    "from the gatherer: api='%s' display='%s' quality='%s'\n",
                    rt.render_api.c_str(), rt.display_mode.c_str(),
                    rt.quality_preset.c_str());
        return false;
    }
    if (rs.platform.empty() || rs.cpu.empty() || rs.gpu.empty() ||
        rt.render_api.empty() || rt.display_mode.empty() ||
        rt.quality_preset.empty()) {
        std::printf("[unit_complete_info] FAILED: a gathered field is blank "
                    "after completion\n");
        return false;
    }

    return true;
}

// ── Case 2: an undetermined datum yields the placeholder, report not skipped ─
bool complete_info_placeholder_not_skipped() {
    const heaven::CameraPath path = make_fixture_path();
    heaven::SceneMarkers markers;
    markers.times = {0.0f};

    // The gatherers return formatted strings; an undetermined datum already
    // carries the per-datum Unknown_Value_Placeholder. Here the GPU and the
    // Render API are undetermined, while the remaining datums are determined.
    heaven::SystemInfo sys = canned_system();
    sys.gpu = heaven::kUnknownGpu;
    heaven::SettingsInfo set = canned_settings();
    set.render_api = heaven::kUnknownRenderApi;

    heaven::BenchmarkSystem bench(
        &markers, &path, [] { return true; },
        [&sys] { return sys; },
        [&set] { return set; });
    heaven::Camera camera;

    if (!drive_to_complete(bench, camera, "placeholder not skipped")) {
        return false;
    }

    const heaven::BenchmarkStatus s = bench.status();

    // The report is NOT skipped: the run still reaches Complete with a result
    // — gathering an undetermined datum never blocks
    // or aborts the report.
    if (s.state != heaven::BenchmarkState::Complete || !s.has_result) {
        std::printf("[unit_complete_info] FAILED: undetermined datum skipped "
                    "the report (state=%d has_result=%d)\n",
                    static_cast<int>(s.state),
                    static_cast<int>(s.has_result));
        return false;
    }

    // The placeholder is surfaced verbatim for the undetermined datums, while
    // the determined datums still carry their real values.
    if (s.result.system.gpu != heaven::kUnknownGpu) {
        std::printf("[unit_complete_info] FAILED: undetermined GPU did not "
                    "yield the placeholder (gpu='%s', expected '%s')\n",
                    s.result.system.gpu.c_str(), heaven::kUnknownGpu);
        return false;
    }
    if (s.result.settings.render_api != heaven::kUnknownRenderApi) {
        std::printf("[unit_complete_info] FAILED: undetermined render API did "
                    "not yield the placeholder (api='%s', expected '%s')\n",
                    s.result.settings.render_api.c_str(),
                    heaven::kUnknownRenderApi);
        return false;
    }
    if (s.result.system.platform != sys.platform ||
        s.result.system.cpu != sys.cpu ||
        s.result.settings.display_mode != set.display_mode ||
        s.result.settings.quality_preset != set.quality_preset) {
        std::printf("[unit_complete_info] FAILED: a determined datum was lost "
                    "alongside the placeholder substitution\n");
        return false;
    }

    return true;
}

// ── stderr capture helper ───────────────────────────────────────────────────
// Redirect the process stderr to a temp file across fn(), restore it, and
// return the captured text. Redirect the underlying CRT descriptor so writes
// from the benchmark's C FILE stream are captured on Windows and POSIX hosts.
std::string capture_stderr(const std::function<void()>& fn) {
    std::fflush(stderr);
    std::FILE* temporary = heaven::test::temporary_file();
    if (!temporary) {
        fn();
        return {};
    }
    const int stderr_fd = heaven::test::file_descriptor(stderr);
    const int saved_fd = heaven::test::duplicate_fd(stderr_fd);
    if (saved_fd < 0 || heaven::test::duplicate_to_fd(
            heaven::test::file_descriptor(temporary), stderr_fd) < 0) {
        if (saved_fd >= 0) heaven::test::close_fd(saved_fd);
        std::fclose(temporary);
        fn();
        return {};
    }
    fn();
    std::fflush(stderr);
    heaven::test::duplicate_to_fd(saved_fd, stderr_fd);
    heaven::test::close_fd(saved_fd);
    std::rewind(temporary);
    std::string out;
    char buffer[4096];
    for (;;) {
        const size_t count = std::fread(buffer, 1, sizeof(buffer), temporary);
        out.append(buffer, count);
        if (count != sizeof(buffer)) break;
    }
    const bool failed = std::ferror(temporary) != 0;
    std::fclose(temporary);
    if (failed) return {};
    return out;
}

// ── Case 3: the completing tick logs run stats + System/Settings lines ──────
bool complete_info_logging() {
    const heaven::CameraPath path = make_fixture_path();
    heaven::SceneMarkers markers;
    markers.times = {0.0f};

    const heaven::SystemInfo   sys = canned_system();
    const heaven::SettingsInfo set = canned_settings();

    heaven::BenchmarkSystem bench(
        &markers, &path, [] { return true; },
        canned_system, canned_settings);
    heaven::Camera camera;

    // Start the run outside the capture (request_start logs nothing on success).
    if (!bench.request_start() ||
        bench.state() != heaven::BenchmarkState::Running) {
        std::printf("[unit_complete_info] FAILED: could not start a run "
                    "(logging case)\n");
        return false;
    }

    // Capture stderr only across the completing tick, whose enter_complete()
    // writes the application-log lines.
    const std::string log = capture_stderr([&] {
        bench.tick(1.0f, camera);
    });

    if (bench.state() != heaven::BenchmarkState::Complete) {
        std::printf("[unit_complete_info] FAILED: run did not complete during "
                    "the captured tick (logging case)\n");
        return false;
    }

    // The run statistics line  is present.
    if (log.find("Run complete:") == std::string::npos) {
        std::printf("[unit_complete_info] FAILED: application log is missing "
                    "the run-statistics line. Captured:\n%s\n", log.c_str());
        return false;
    }

    // The System_Info and Settings_Info lines are written together with the run
    // statistics.
    if (log.find("System_Info") == std::string::npos) {
        std::printf("[unit_complete_info] FAILED: application log is missing "
                    "the System_Info line. Captured:\n%s\n", log.c_str());
        return false;
    }
    if (log.find("Settings_Info") == std::string::npos) {
        std::printf("[unit_complete_info] FAILED: application log is missing "
                    "the Settings_Info line. Captured:\n%s\n", log.c_str());
        return false;
    }

    // The gathered descriptions themselves appear in the log lines.
    if (log.find(sys.gpu) == std::string::npos ||
        log.find(set.render_api) == std::string::npos) {
        std::printf("[unit_complete_info] FAILED: gathered descriptions did "
                    "not appear in the application log. Captured:\n%s\n",
                    log.c_str());
        return false;
    }

    return true;
}

bool unit_complete_info() {
    bool ok = true;
    ok = complete_info_same_frame_population() && ok;
    ok = complete_info_placeholder_not_skipped() && ok;
    ok = complete_info_logging() && ok;
    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_complete_info{unit_complete_info};

} // namespace
