/*
 * HeavenBench — asset-integration test: the real clip.track
 *
 *
 *
 * has the Benchmark_System load the Benchmark_Track from
 * clip.track in the configured tracks directory;  extracts the
 * ordered Scene_Markers (one time value per scene) and sets the Scene_Count to
 * the number of extracted markers. The shipped Heaven asset has exactly 26
 * scenes, so loading the real clip.track must yield exactly 26 markers stored
 * in non-decreasing time order (the invariant load_scene_markers
 * guarantees).
 *
 * This test is GATED on the extracted Heaven assets being present: when
 * extracted/heaven/heaven/tracks/clip.track cannot be found from any of the
 * candidate relative locations, the test SKIPS (prints a notice and returns
 * true) so CI without assets still passes. When the asset IS present it loads
 * it for real and asserts the marker count and ordering, returning false with a
 * diagnostic on a genuine mismatch.
 *
 * The test registers itself with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, following
 * the same self-registration pattern as the other test translation units.
 */
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/scene_markers.h"

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

// The Heaven scene sequence has exactly 26 scenes (the spec's
// Scene_Count for the shipped asset).
constexpr size_t kExpectedSceneCount = 26;

// Resolve the tracks DIRECTORY containing clip.track. The test binary runs from
// the build dir (build/renderer), but the orchestrator/CI may launch it from
// various working directories, so probe a handful of candidate relative paths
// (repo root is two levels above build/renderer) and return the first whose
// clip.track exists. Returns std::nullopt when the asset is absent everywhere.
std::optional<std::string> resolve_tracks_dir() {
    namespace fs = std::filesystem;

    const char* const candidates[] = {
        "extracted/heaven/heaven/tracks",
        "../extracted/heaven/heaven/tracks",
        "../../extracted/heaven/heaven/tracks",
        "../../../extracted/heaven/heaven/tracks",
    };

    for (const char* dir : candidates) {
        std::error_code ec;
        const fs::path clip = fs::path(dir) / "clip.track";
        if (fs::exists(clip, ec) && !ec) {
            return std::string(dir);
        }
    }
    return std::nullopt;
}

bool integ_clip_track() {
    const std::optional<std::string> tracks_dir = resolve_tracks_dir();

    // Gracefully skip when the extracted assets are not present so CI without
    // assets still passes.
    if (!tracks_dir.has_value()) {
        std::printf(
            "[integ_clip_track] SKIPPED: assets absent "
            "(extracted/heaven/heaven/tracks/clip.track not found)\n");
        return true;
    }

    // Assets ARE present: load the real clip.track and assert the markers.
    const std::optional<heaven::SceneMarkers> markers =
        heaven::load_scene_markers(*tracks_dir);

    if (!markers.has_value()) {
        std::printf(
            "[integ_clip_track] FAILED: load_scene_markers returned nullopt "
            "for present asset in %s\n",
            tracks_dir->c_str());
        return false;
    }

    bool ok = true;

    // Exactly 26 markers.
    if (markers->count() != kExpectedSceneCount) {
        std::printf(
            "[integ_clip_track] FAILED: expected %zu scene markers, got %zu\n",
            kExpectedSceneCount, markers->count());
        ok = false;
    }

    // Markers are stored in non-decreasing time order.
    for (size_t i = 1; i < markers->times.size(); ++i) {
        if (markers->times[i] < markers->times[i - 1]) {
            std::printf(
                "[integ_clip_track] FAILED: markers not non-decreasing at "
                "index %zu (%.6f < %.6f)\n",
                i, markers->times[i], markers->times[i - 1]);
            ok = false;
            break;
        }
    }

    if (ok) {
        std::printf(
            "[integ_clip_track] PASSED: loaded %zu markers from %s in "
            "non-decreasing order\n",
            markers->count(), tracks_dir->c_str());
    }
    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_integ_clip_track{integ_clip_track};

} // namespace
