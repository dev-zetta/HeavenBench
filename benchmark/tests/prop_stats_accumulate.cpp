/*
 * Run statistics retain positive finite wall-clock intervals, including long
 * hitches. Invalid samples must not inflate the frame count or elapsed time.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/stats_collector.h"

namespace heaven::testing {

// Mirrors the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is identical across test TUs (ODR-safe).
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Relative comparison with an absolute floor. The accumulated sums match the
// implementation operation-for-operation, so the difference is typically zero;
// the epsilon guards only against incidental reordering by the optimiser.
bool approx_equal(double a, double b) {
    const double diff  = std::abs(a - b);
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return diff <= 1e-9 * scale;
}

bool run_stats_accumulate_property() {
    // RapidCheck's default is 100 iterations (maxSuccess = 100), meeting the
    // "≥ 100 iterations" requirement; it can be raised via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Property 10: statistics accumulate valid frames and real elapsed time over "
        "any delta sequence (Validates: Requirements 5.1, 5.2)",
        [] {
            // Element generator covering the full input space the property is
            // stated over: a continuous band over [-1, 2] (so negatives, zeros,
            // small positives, and long hitches all appear), biased
            // with explicit edge cases around the simulation's 100 ms cap.
            const auto delta_gen = rc::gen::oneOf(
                rc::gen::map(rc::gen::inRange<int>(-1000, 2001),
                             [](int milli) {
                                 return static_cast<float>(milli) / 1000.0f;
                             }),
                rc::gen::element<float>(-1.0f, -0.0001f, 0.0f, 0.05f,
                                        0.1f,
                                        0.10001f, 0.5f, 2.0f));

            const std::vector<float> deltas =
                *rc::gen::container<std::vector<float>>(delta_gen);

            heaven::StatsCollector collector;
            collector.reset();

            double expected_total = 0.0;
            std::uint64_t expected_frames = 0;
            for (float d : deltas) {
                collector.add_frame(d);
                if (d > 0.0f && std::isfinite(d)) {
                    expected_total += static_cast<double>(d);
                    ++expected_frames;
                }
            }

            RC_ASSERT(collector.frames() == expected_frames);
            RC_ASSERT(approx_equal(collector.total_time(), expected_total));
        });
}

const heaven::testing::Register reg_stats_accumulate{&run_stats_accumulate_property};

} // namespace
