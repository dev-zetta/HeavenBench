/*
 * HeavenBench — Property test for refusing start when pacing can't be off
 *
 * Start is refused when frame pacing cannot be disabled.
 *
 *
 * The implementation guarantees that that IF frame pacing cannot be disabled when starting a
 * Benchmark_Run, THEN the Benchmark_System SHALL refuse to start, SHALL leave
 * the system in its pre-start idle state with no run created, AND SHALL surface
 * an error indication identifying that frame pacing could not be disabled.
 *
 * BenchmarkSystem::request_start() surfaces this by returning false, staying
 * Idle, and logging the cause when its FramePacingProbe reports false. The
 * pacing check is the LAST precondition in request_start(), so to prove the
 * refusal is driven specifically by pacing (and "regardless of other valid
 * preconditions"), the fixture makes EVERY other precondition valid:
 *
 *   - a usable CameraPath: n >= 2 keyframes, t[0] = 0, strictly-increasing
 *     positive gaps, so duration > 0 and keyframes.size() >= 2 (OK);
 *   - a non-empty, non-decreasing SceneMarkers list spanning [0, duration]
 *     (OK);
 *   - the system starts in Idle (so the "run already active" guard, is
 *     not what trips it).
 *
 * The only thing wrong is the pacing probe returning false. Under that fixture,
 * across randomized otherwise-valid assets, this property asserts:
 *
 *   - request_start() returns false and the system stays Idle               (11.2)
 *   - this holds across repeated request_start() calls (still false, still Idle)
 *   - status() exposes no run: state Idle, has_result false, and the run
 *     statistics read as their clean idle values (no frames/elapsed/score).
 *
 * As a CONTRAST sub-check on the SAME randomized assets, a second system built
 * with a probe returning true DOES start (returns true, enters Running) — so
 * the pacing probe is demonstrably the only differing factor between refusal
 * and success. The main assertion remains the false-probe refusal.
 *
 * Keyframe rotations are identity quaternions: this property is about the
 * start-guard's pacing precondition, not rotation interpolation.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/benchmark_system.h"
#include "benchmark/scene_markers.h"
#include "engine/camera.h"
#include "engine/camera_path.h"

namespace heaven::testing {

// Mirrors the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

bool run_start_pacing_property() {
    // RapidCheck's default is 100 successful iterations (maxSuccess == 100),
    // meeting the ">= 100 iterations" requirement; raise via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Property 15: start is refused when frame pacing cannot be disabled "
        "(Validates: Requirements 11.2)",
        [] {
            // ── Build a fully-usable CameraPath: n >= 2 keyframes, t[0] = 0,
            // then strictly-increasing positive gaps, so duration > 0. ──────
            const int n = *rc::gen::inRange<int>(2, 10);

            const auto coord_gen =
                rc::gen::map(rc::gen::inRange<int>(-100000, 100001),
                             [](int v) { return static_cast<float>(v) * 0.01f; });
            const auto fov_gen =
                rc::gen::map(rc::gen::inRange<int>(100, 1701),
                             [](int v) { return static_cast<float>(v) * 0.1f; });
            // Per-keyframe gaps in [0.1, 2.0] s so duration is comfortably > 0.
            const auto gap_gen =
                rc::gen::map(rc::gen::inRange<int>(100, 2001),
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
                kf.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity
                kf.fov = *fov_gen;
                path.keyframes.push_back(kf);
            }
            path.duration = path.keyframes.back().time;
            RC_ASSERT(path.duration > 0.0f);
            RC_ASSERT(path.keyframes.size() >= size_t{2}); // satisfied

            // ── Build a non-empty, non-decreasing marker list in
            // [0, duration] so the scene-marker precondition is valid too. ──
            const int m = *rc::gen::inRange<int>(1, 9);
            std::vector<float> raw;
            raw.reserve(static_cast<size_t>(m));
            for (int i = 0; i < m; ++i) {
                const float frac =
                    static_cast<float>(*rc::gen::inRange<int>(0, 1001)) / 1000.0f;
                raw.push_back(path.duration * frac);
            }
            heaven::SceneMarkers markers;
            markers.times = heaven::normalize_marker_times(std::move(raw));
            RC_ASSERT(markers.count() >= size_t{1}); // satisfied

            heaven::Camera camera;

            // ── Main assertion: with EVERY other precondition valid but the
            // pacing probe reporting false, the start is refused and the system
            // stays in its pre-start idle state. ─────────
            heaven::BenchmarkSystem paced(
                &markers, &path, [] { return false; }); // pacing NOT disabled

            // The system begins Idle and exposes no run.
            RC_ASSERT(paced.state() == heaven::BenchmarkState::Idle);

            // request_start() must refuse, repeatedly, and never leave Idle.
            const int attempts = *rc::gen::inRange<int>(1, 6);
            for (int i = 0; i < attempts; ++i) {
                RC_ASSERT(!paced.request_start());            // refused (11.2)
                RC_ASSERT(paced.state() == heaven::BenchmarkState::Idle);
                RC_ASSERT(!paced.is_running());

                // status() exposes no run: clean idle snapshot, no result, and
                // no run statistics.
                const heaven::BenchmarkStatus s = paced.status();
                RC_ASSERT(s.state == heaven::BenchmarkState::Idle);
                RC_ASSERT(!s.has_result);
                RC_ASSERT(s.frames == uint64_t{0});
                RC_ASSERT(s.elapsed == 0.0);
                RC_ASSERT(s.average_fps == 0.0);
                RC_ASSERT(s.scene_index == size_t{0});
            }

            // A refused start must also leave the camera-driving path inert:
            // ticking does nothing because the system never left Idle.
            paced.tick(0.016f, camera);
            RC_ASSERT(paced.state() == heaven::BenchmarkState::Idle);
            RC_ASSERT(paced.status().frames == uint64_t{0});
            RC_ASSERT(paced.status().elapsed == 0.0);

            // ── Contrast sub-check: the SAME valid assets with a probe that
            // reports pacing IS disabled DO start, so the pacing probe is the
            // only differing factor between refusal and success. ────────────
            heaven::BenchmarkSystem unpaced(
                &markers, &path, [] { return true; }); // pacing disabled
            RC_ASSERT(unpaced.request_start());
            RC_ASSERT(unpaced.state() == heaven::BenchmarkState::Running);
        });
}

const heaven::testing::Register reg_start_pacing{&run_start_pacing_property};

} // namespace
