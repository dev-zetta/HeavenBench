/*
 * HeavenBench — unit test: first-frame min==max edge case
 *
 *
 *
 * On a fresh StatsCollector, reset() seeds the maximum frame rate at zero and
 * the minimum frame rate at the maximum representable value so the first
 * processed frame's instantaneous frame rate replaces both. This example test
 * pins that behaviour: after exactly one valid (strictly-positive-delta) frame,
 * there is only one observed instantaneous frame rate, so the minimum and the
 * maximum must coincide and both equal the reciprocal of the measured interval.
 *
 * The test registers itself with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, following
 * the same self-registration pattern as the other test translation units. The
 * registered callable returns true on success and false (after printing a
 * diagnostic) on failure.
 */
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "benchmark/stats_collector.h"

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

bool unit_stats_first_frame() {
    bool ok = true;

    // One valid frame: ~60 fps (16 ms), with instantaneous rate 1 / 0.016.
    const float dt = 0.016f;
    const float expected_fps = 1.0f / dt;

    heaven::StatsCollector stats;
    stats.reset();
    stats.add_frame(dt);

    // After a single valid frame the min and max reductions have seen exactly
    // one sample, so they must be equal.
    if (stats.min_fps() != stats.max_fps()) {
        std::printf(
            "[unit_stats_first_frame] FAILED: min_fps() == %.9g, "
            "max_fps() == %.9g, expected them equal after one valid frame\n",
            static_cast<double>(stats.min_fps()),
            static_cast<double>(stats.max_fps()));
        ok = false;
    }

    // Both should equal the reciprocal of the measured interval. Compare within a
    // small tolerance to absorb float division rounding.
    if (std::fabs(stats.min_fps() - expected_fps) > 1e-3f) {
        std::printf(
            "[unit_stats_first_frame] FAILED: min_fps() == %.9g, "
            "expected %.9g (1 / %.9g)\n",
            static_cast<double>(stats.min_fps()),
            static_cast<double>(expected_fps), static_cast<double>(dt));
        ok = false;
    }
    if (std::fabs(stats.max_fps() - expected_fps) > 1e-3f) {
        std::printf(
            "[unit_stats_first_frame] FAILED: max_fps() == %.9g, "
            "expected %.9g (1 / %.9g)\n",
            static_cast<double>(stats.max_fps()),
            static_cast<double>(expected_fps), static_cast<double>(dt));
        ok = false;
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_stats_first_frame{unit_stats_first_frame};

} // namespace
