/*
 * HeavenBench — unit test: empty / missing / zero-marker handling
 *
 *
 *
 * Two defensive behaviours of the scene-marker code are pinned here:
 *
 *   - An empty SceneMarkers (default-constructed, no times) answers
 *     scene_index_at(t) with 0 for every query time, and reports count() == 0.
 *     The selection logic is defined so that a run started before any valid
 *     markers are loaded never indexes out of range.
 *
 *   - load_scene_markers() returns std::nullopt when clip.track cannot be found
 *     (the file is missing, unparseable, or has zero markers).
 *     This test uses a directory path guaranteed not to contain a clip.track so
 *     it exercises the missing-file branch WITHOUT depending on the extracted
 *     Heaven assets being present (the real-asset case is). That keeps
 *     the check valid in CI without assets.
 *
 * The test registers itself with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, following
 * the same self-registration pattern as the other test translation units. The
 * registered callable returns true on success and false (after printing a
 * diagnostic) on failure. No second main() is defined.
 */
#include <cstdio>
#include <functional>
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

bool unit_markers_edge() {
    bool ok = true;

    // ── Empty SceneMarkers: scene_index_at always 0, count() == 0 ──

    // A default-constructed SceneMarkers has no times. Whether the query time
    // precedes, sits inside, or follows where markers would be, the selection
    // must clamp to index 0 so callers never index an empty list.
    const heaven::SceneMarkers empty{};

    if (empty.count() != 0) {
        std::printf(
            "[unit_markers_edge] FAILED: empty SceneMarkers count() == %zu, "
            "expected 0\n",
            empty.count());
        ok = false;
    }

    const float query_times[] = {-1.0f, 0.0f, 5.0f, 100.0f};
    for (const float t : query_times) {
        const size_t index = empty.scene_index_at(t);
        if (index != 0) {
            std::printf(
                "[unit_markers_edge] FAILED: empty SceneMarkers "
                "scene_index_at(%.1f) == %zu, expected 0\n",
                static_cast<double>(t), index);
            ok = false;
        }
    }

    // ── load_scene_markers on a guaranteed-missing path → nullopt ──

    // This directory is chosen so clip.track cannot exist there, exercising the
    // missing-file branch  without relying on the extracted
    // assets being present in the working tree or CI.
    const std::string missing_dir =
        "/nonexistent/path/that/does/not/exist__benchmark_test";

    const std::optional<heaven::SceneMarkers> result =
        heaven::load_scene_markers(missing_dir);

    if (result.has_value()) {
        std::printf(
            "[unit_markers_edge] FAILED: load_scene_markers(\"%s\") returned a "
            "value, expected std::nullopt for a missing clip.track\n",
            missing_dir.c_str());
        ok = false;
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_markers_edge{unit_markers_edge};

} // namespace
