/*
 * Minimum and maximum FPS use the same positive finite intervals as the score.
 * Invalid samples leave the statistics unchanged; reset clears all reductions.
 */
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/stats_collector.h"

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

// Relative comparison with an absolute floor. The expected extrema are SELECTED
// from values computed with the same float reciprocal the collector uses, so
// the difference is effectively zero; the tolerance only guards against
// incidental rounding in the comparison itself.
bool approx_equal(float a, float b) {
    const float diff  = std::fabs(a - b);
    const float scale = std::max({1.0f, std::fabs(a), std::fabs(b)});
    return diff <= 1e-5f * scale;
}

// One delta generator covering the whole input space the property is stated
// over: negatives, exactly zero, small positive intervals, and long hitches.
// Drawn from an integer millisecond range so the floats
// are well-conditioned (no NaN/Inf) while still spanning every region.
//   [-200, 300] ms -> [-0.2, 0.3] s in 0.001 s steps.
rc::Gen<float> mixed_delta_gen() {
    return rc::gen::map(rc::gen::inRange<int>(-200, 301),
                        [](int milli) { return static_cast<float>(milli) / 1000.0f; });
}

// Non-positive deltas only: [-200, 0] ms -> [-0.2, 0.0] s. None is a valid frame.
rc::Gen<float> nonpositive_delta_gen() {
    return rc::gen::map(rc::gen::inRange<int>(-200, 1),
                        [](int milli) { return static_cast<float>(milli) / 1000.0f; });
}

bool run_stats_minmax_properties() {
    // ── (a) Mixed sequence: min/max equal the reductions over valid frames,
    //        exclusion leaves them unchanged, and reset returns them to zero.
    const bool ok_mixed = rc::check(
        "Property 11 (a): min/max are reductions over valid frames; exclusion "
        "and reset hold (Validates: Requirements 5.3, 5.4, 5.5, 5.6, 7.7)",
        [] {
            const std::vector<float> deltas =
                *rc::gen::container<std::vector<float>>(mixed_delta_gen());

            heaven::StatsCollector sc;
            sc.reset();

            // Before any frame is fed, both extrema read as zero.
            RC_ASSERT(sc.min_fps() == 0.0f);
            RC_ASSERT(sc.max_fps() == 0.0f);

            // Expected extrema over valid wall-clock intervals.
            bool  any_valid = false;
            std::uint64_t valid_frames = 0;
            float expected_min = std::numeric_limits<float>::max();
            float expected_max = 0.0f;
            for (float d : deltas) {
                if (d > 0.0f && std::isfinite(d)) {
                    ++valid_frames;
                    const float fps = 1.0f / d;
                    expected_min = std::min(expected_min, fps);
                    expected_max = std::max(expected_max, fps);
                    any_valid = true;
                }
            }

            for (float d : deltas) {
                sc.add_frame(d);
            }

            // Invalid intervals are excluded from every statistic.
            RC_ASSERT(sc.frames() == valid_frames);

            if (any_valid) {
                // Min/Max equal the reductions over the valid frames

                RC_ASSERT(approx_equal(sc.max_fps(), expected_max));
                RC_ASSERT(approx_equal(sc.min_fps(), expected_min));
            } else {
                // No valid frame fed -> both remain zero.
                RC_ASSERT(sc.min_fps() == 0.0f);
                RC_ASSERT(sc.max_fps() == 0.0f);
            }

            // Feeding additional non-positive frames must not change any statistic.
            const float min_before = sc.min_fps();
            const float max_before = sc.max_fps();
            const std::vector<float> tail =
                *rc::gen::container<std::vector<float>>(nonpositive_delta_gen());
            for (float d : tail) {
                sc.add_frame(d);
            }
            RC_ASSERT(sc.min_fps() == min_before);
            RC_ASSERT(sc.max_fps() == max_before);
            RC_ASSERT(sc.frames() == valid_frames);

            // After reset(), both extrema return to zero,
            // even after valid frames were recorded above.
            sc.reset();
            RC_ASSERT(sc.min_fps() == 0.0f);
            RC_ASSERT(sc.max_fps() == 0.0f);
            RC_ASSERT(sc.frames() == uint64_t{0});
        });

    // ── (b) All-non-positive sequence: every frame is excluded from min/max,
    //        so both stay zero however many frames are fed.
    const bool ok_excluded = rc::check(
        "Property 11 (b): non-positive frames are excluded, leaving min/max at "
        "zero (Validates: Requirements 5.6, 7.7)",
        [] {
            const std::vector<float> deltas =
                *rc::gen::container<std::vector<float>>(nonpositive_delta_gen());

            heaven::StatsCollector sc;
            sc.reset();

            for (float d : deltas) {
                sc.add_frame(d);
            }

            // All deltas are <= 0, so no valid frame is ever seen.
            RC_ASSERT(sc.min_fps() == 0.0f);
            RC_ASSERT(sc.max_fps() == 0.0f);
            // Invalid intervals do not count toward the total.
            RC_ASSERT(sc.frames() == 0);
        });

    return ok_mixed && ok_excluded;
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_stats_minmax{run_stats_minmax_properties};

} // namespace
