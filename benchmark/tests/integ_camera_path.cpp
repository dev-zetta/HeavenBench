/*
 * HeavenBench — asset-integration test: the real camera path
 *
 *
 *
 * This test exercises load_camera_path() against the ACTUAL extracted Heaven
 * assets under extracted/heaven/heaven/tracks (the camera_*.track waypoint
 * files merged into a single CameraPath). It pins two run-critical properties
 * of the loaded path:
 *
 *   - keyframes.size() >= 2  — a usable path has at least two keyframes so the
 *     interpolator has a segment to evaluate (a path with
 *     fewer than two keyframes is unusable and a run must not start).
 *   - duration > 0           — the merged path spans a positive number of
 *     seconds, so playback time can advance toward a defined end
 *     (the authored camera path is loaded and merged).
 *
 * GATING: the extracted assets are gitignored and absent in CI. The test
 * probes a handful of candidate relative paths (the binary may run from
 * build/renderer, the repo root, or elsewhere) for the tracks directory and
 * only proceeds when one exists AND contains at least one camera_*.track file.
 * When no such directory is found the test SKIPS gracefully (prints a notice
 * and returns true) so CI without assets still passes.
 *
 * The test registers itself with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, following
 * the same self-registration pattern as the other test translation units. The
 * registered callable returns true on success/skip and false (after printing a
 * diagnostic) on a genuine failure. No second main() is defined.
 */
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "engine/asset_usage_recorder.h"
#include "benchmark/scene_markers.h"
#include "engine/camera_path.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// is a free function with external linkage defined there; the Register helper's
// definition is identical so it can be reused from this translation unit.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

namespace fs = std::filesystem;

// True when dir exists and holds at least one camera_*.track waypoint file
// (excluding clip.track, which is the scene-marker track, not a camera path).
// This is the same selection load_camera_path() applies, so a positive result
// here means the loader has real input to merge.
bool has_camera_tracks(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) return false;
        const std::string name = entry.path().filename().string();
        if (name.starts_with("camera") && name.ends_with(".track") &&
            name != "clip.track") {
            return true;
        }
    }
    return false;
}

bool integ_camera_path() {
    // The asset tracks dir is <repo_root>/extracted/heaven/heaven/tracks. The
    // test binary's CWD depends on how it is launched (build/renderer under
    // ctest, the repo root, a nested build tree,...), so probe a few relative
    // candidates and use the first that actually contains camera tracks.
    const char* const candidates[] = {
        "extracted/heaven/heaven/tracks",
        "../extracted/heaven/heaven/tracks",
        "../../extracted/heaven/heaven/tracks",
        "../../../extracted/heaven/heaven/tracks",
    };

    fs::path tracks_dir;
    for (const char* candidate : candidates) {
        if (has_camera_tracks(candidate)) {
            tracks_dir = candidate;
            break;
        }
    }

    if (tracks_dir.empty()) {
        std::printf("[integ_camera_path] SKIPPED: assets absent\n");
        return true;
    }

    // Assets are present: load the real camera path and assert it is usable.
    // The source root is extracted/heaven, two levels above heaven/tracks.
    // Supplying the recorder also verifies that nested DOF-track dependencies
    // are included by the used-asset packer.
    const fs::path source_root =
        fs::weakly_canonical(tracks_dir.parent_path().parent_path());
    heaven::AssetUsageRecorder recorder(
        source_root.string());
    const heaven::CameraPath path =
        heaven::load_camera_path(tracks_dir.string(), &recorder);

    bool ok = true;

    const auto markers = heaven::load_scene_markers(tracks_dir.string());
    if (!markers) {
        std::printf(
            "[integ_camera_path] FAILED: could not reload scene markers\n");
        ok = false;
    } else {
        // Every discontinuity in clip.track must survive camera-path baking as
        // an exact key for the new scene. A nearby 20 Hz grid key is not
        // equivalent: retaining the old scene there delays the camera/DOF
        // switch until the following sample and produces a visible pop.
        for (size_t scene = 0; scene < markers->times.size(); ++scene) {
            const float boundary = markers->times[scene];
            const auto key = std::find_if(
                path.keyframes.begin(), path.keyframes.end(),
                [&](const heaven::CameraKeyframe& candidate) {
                    return candidate.time == boundary &&
                           std::cmp_equal(candidate.scene, scene);
                });
            if (key == path.keyframes.end()) {
                std::printf(
                    "[integ_camera_path] FAILED: exact boundary %.6f for "
                    "scene %zu was lost during path baking\n",
                    static_cast<double>(boundary), scene);
                ok = false;
            }
        }
    }

    const auto& used_assets = recorder.used();
    for (const char* expected : {
             "heaven/tracks/dof_control2.track",
             "heaven/tracks/dof_control16.track",
         }) {
        if (!used_assets.contains(expected)) {
            std::printf(
                "[integ_camera_path] FAILED: asset recorder omitted nested "
                "DOF dependency %s\n",
                expected);
            ok = false;
        }
    }

    // a usable path has at least two keyframes.
    if (path.keyframes.size() < 2) {
        std::printf(
            "[integ_camera_path] FAILED: loaded path has %zu keyframes, "
            "expected >= 2 (dir: %s)\n",
            path.keyframes.size(), tracks_dir.string().c_str());
        ok = false;
    }

    // the merged path spans a positive duration.
    if (!(path.duration > 0.0f)) {
        std::printf(
            "[integ_camera_path] FAILED: loaded path duration is %.6f, "
            "expected > 0 (dir: %s)\n",
            static_cast<double>(path.duration), tracks_dir.string().c_str());
        ok = false;
    }

    auto expect_hour = [&](float sample_time, float expected) {
        const float actual = path.time_of_day_clamped(sample_time, -1.0f);
        if (std::abs(actual - expected) > 0.05f) {
            std::printf(
                "[integ_camera_path] FAILED: time_of_day_clamped(%.2f) = %.3f, "
                "expected %.3f from nested day_cycle.track binding\n",
                static_cast<double>(sample_time),
                static_cast<double>(actual),
                static_cast<double>(expected));
            ok = false;
        }
    };

    auto expect_dof = [&](float sample_time, float expected_focus,
                          float expected_aperture) {
        const heaven::AuthoredDofState dof =
            path.depth_of_field_clamped(sample_time);
        if (!dof.enabled ||
            std::abs(dof.focus_distance - expected_focus) > 0.05f ||
            std::abs(dof.desired_aperture - expected_aperture) > 0.05f) {
            std::printf(
                "[integ_camera_path] FAILED: depth_of_field_clamped(%.2f) = "
                "{enabled=%d, focus=%.3f, aperture=%.3f}, expected "
                "{enabled=1, focus=%.3f, aperture=%.3f}\n",
                static_cast<double>(sample_time), dof.enabled ? 1 : 0,
                static_cast<double>(dof.focus_distance),
                static_cast<double>(dof.desired_aperture),
                static_cast<double>(expected_focus),
                static_cast<double>(expected_aperture));
            ok = false;
        }
    };

    // The original benchmark embeds a nested day_cycle.track binding inside
    // each camera track. Pin a few scene-boundary samples so the loader cannot
    // regress to one global CLI hour while still passing camera-pose checks.
    if (path.keyframes.size() >= 2 && path.duration > 181.1f) {
        expect_hour(0.0f, 8.0f);      // camera_09.track: 8 -> 20
        expect_hour(17.0f, 15.7f);    // camera2_04.track
        expect_hour(181.1f, 24.0f);   // camera2_19.track

        // The same camera tracks bind the original DOF control presets. Pin
        // both their sampled focus-control time and normalized f/2:f/16 blend.
        // Equivalent f-number is the harmonic blend because optical aperture
        // strength is proportional to 1/f-number.
        expect_dof(0.0f, 7.2f, 2.42424f);   // camera_09: weights.8/.2
        expect_dof(17.0f, 16.0f, 2.71186f); // camera2_04: weights.7/.3

        // camera_32 performs a continuous focus pull from control time.4 to
        // 1.0. Sample safely inside the shot instead of exactly on its
        // floating-point scene boundary.
        const heaven::AuthoredDofState pull_start =
            path.depth_of_field_clamped(68.2f);
        const heaven::AuthoredDofState pull_end =
            path.depth_of_field_clamped(73.7f);
        if (!pull_start.enabled || !pull_end.enabled ||
            !(pull_end.focus_distance > pull_start.focus_distance + 10.0f)) {
            std::printf(
                "[integ_camera_path] FAILED: camera_32 focus pull did not "
                "advance (%.3f -> %.3f)\n",
                static_cast<double>(pull_start.focus_distance),
                static_cast<double>(pull_end.focus_distance));
            ok = false;
        }
    }

    if (ok) {
        std::printf(
            "[integ_camera_path] OK: %zu keyframes, duration %.3f s (dir: %s)\n",
            path.keyframes.size(), static_cast<double>(path.duration),
            tracks_dir.string().c_str());
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_integ_camera_path{integ_camera_path};

} // namespace
