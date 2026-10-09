/*
 * HeavenBench — Property test for scene-index selection
 *
 * Scene index selection picks the latest marker not exceeding the
 *             time.
 *
 *
 * SceneMarkers::scene_index_at(t) must return the Scene_Index defined by
 * the index of the latest Scene_Marker whose time does not
 * exceed the current playback time t, or zero when t precedes the first marker.
 * For any non-decreasing, non-empty marker list and any query time t (before the
 * first marker, exactly on a marker, between markers, or after the last marker),
 * the result must satisfy:
 *   - it lies within the bound [0, count()-1]
 *   - if t < times[0], the result is 0
 *   - otherwise the result is the latest index i with times[i] <= t, i.e.
 *     times[result] <= t AND (result == count()-1 OR times[result+1] > t)
 *
 * Marker times are generated from a finite, bounded integer range scaled to
 * float (so the ordering comparisons stay well-defined — a NaN would break
 * them) and normalized non-decreasing; the list is forced non-empty. Query
 * times either land exactly on a marker (the upper_bound boundary case) or are
 * drawn from a wider bounded range that brackets the marker range, so the
 * before/at/between/after cases are all exercised.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/scene_markers.h"

// The registry lives in test_main.cpp (external linkage, no shared header), so
// mirror its minimal interface here to append our test. The struct definition
// is token-identical to the one in test_main.cpp, satisfying the ODR.
namespace heaven::testing {

using TestFn = std::function<bool()>;

std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

bool run_scene_index_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 2: scene index selection picks the latest marker not "
        "exceeding the time (Validates: Requirements 2.4)",
        [] {
            // Element generator: marker times in [-50.0, 50.0] in 0.1 steps,
            // finite (no NaN/Inf) with frequent duplicates so the
            // exactly-on-a-marker and tied-marker cases are well covered.
            const auto marker_gen = rc::gen::map(
                rc::gen::inRange<int>(-500, 501),
                [](int v) { return static_cast<float>(v) * 0.1f; });

            // Build a non-decreasing, non-empty marker list: generate an
            // arbitrary-length sample, force at least one element, then
            // normalize to non-decreasing order (ordering, which
            // scene_index_at relies on).
            std::vector<float> raw =
                *rc::gen::container<std::vector<float>>(marker_gen);
            raw.push_back(*marker_gen);
            heaven::SceneMarkers markers;
            markers.times = heaven::normalize_marker_times(std::move(raw));

            const std::vector<float>& times = markers.times;
            const size_t n = times.size();
            RC_ASSERT(n >= size_t{1});

            // Query time t: half the time land exactly on an existing marker
            // (exercises upper_bound's "time <= t selects that marker" boundary,
            // including tied duplicates); otherwise draw from a wider range
            // [-60.0, 60.0] that brackets the marker range so the before-first
            // and after-last cases are covered as well.
            float t = 0.0f;
            if (*rc::gen::arbitrary<bool>()) {
                const size_t idx = *rc::gen::inRange<size_t>(size_t{0}, n);
                t = times[idx];
            } else {
                t = *rc::gen::map(
                    rc::gen::inRange<int>(-600, 601),
                    [](int v) { return static_cast<float>(v) * 0.1f; });
            }

            const size_t result = markers.scene_index_at(t);

            // The result is always within [0, count()-1].
            RC_ASSERT(result < n);

            if (t < times[0]) {
                // t precedes the first marker -> Scene_Index is 0.
                RC_ASSERT(result == size_t{0});
            } else {
                // Otherwise the result is the LATEST index i with times[i] <= t:
                //   times[result] <= t, and either result is the last marker or
                //   the next marker is strictly beyond t.
                RC_ASSERT(times[result] <= t);
                RC_ASSERT(result == n - 1 || times[result + 1] > t);
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_scene_index{
    run_scene_index_property};

} // namespace
