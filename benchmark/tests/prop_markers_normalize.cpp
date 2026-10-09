/*
 * HeavenBench — Property test for scene-marker normalization
 *
 * Scene markers are stored as a non-decreasing permutation, with a
 *             faithful count.
 *
 *
 * normalize_marker_times() must return the same multiset of times it was given,
 * reordered non-decreasing — no element added, dropped, or altered. For any
 * input vector (duplicates, negatives, and unsorted order all allowed), the
 * output must satisfy:
 *   - non-decreasing order: out[i] <= out[i+1] for all i
 *   - multiset permutation of the input: same size, and a sorted copy of the
 *     input equals the output element-for-element
 *   - faithful count: a SceneMarkers built from the result reports
 *     count() == input.size()
 *
 * Inputs are generated from a finite, bounded range mapped to float so the
 * permutation/ordering checks stay well-defined (a NaN would break the ordering
 * comparisons); the range and step deliberately produce negatives and frequent
 * duplicates as well as arbitrary (unsorted) order.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
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

bool run_markers_normalize_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 1: scene markers normalize to a non-decreasing permutation "
        "with a faithful count (Validates: Requirements 1.2, 1.3)",
        [] {
            // Generate marker times from a finite, bounded integer range scaled
            // to float: values land in [-100.0, 100.0] in 0.1 steps, so the
            // sample naturally includes negatives and frequent duplicates while
            // staying finite (no NaN/Inf to undefine the ordering checks). The
            // container generator yields arbitrary (unsorted) lengths and order,
            // including the empty vector.
            const std::vector<float> input =
                *rc::gen::container<std::vector<float>>(rc::gen::map(
                    rc::gen::inRange<int>(-1000, 1001),
                    [](int v) { return static_cast<float>(v) * 0.1f; }));

            const std::vector<float> out = heaven::normalize_marker_times(input);

            // Output is non-decreasing.
            for (size_t i = 1; i < out.size(); ++i) {
                RC_ASSERT(out[i - 1] <= out[i]);
            }

            // Output is a multiset permutation of the input:
            // identical size, and a sorted copy of the input equals the output
            // element-for-element. This proves no element was added, dropped, or
            // changed — only reordered.
            RC_ASSERT(out.size() == input.size());

            std::vector<float> sorted_input = input;
            std::sort(sorted_input.begin(), sorted_input.end());
            RC_ASSERT(sorted_input == out);

            // The stored count is faithful to the input size.
            heaven::SceneMarkers markers;
            markers.times = out;
            RC_ASSERT(markers.count() == input.size());
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_markers_normalize{
    run_markers_normalize_property};

} // namespace
