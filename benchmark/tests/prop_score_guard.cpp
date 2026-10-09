/*
 * HeavenBench — score-v3 degenerate-input guards
 */
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/score_calculator.h"

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool run_score_guard_properties() {
    return rc::check(
        "score v3 rejects non-positive and non-finite rates",
        [] {
            const double magnitude = std::fabs(
                *rc::gen::arbitrary<double>());
            RC_PRE(std::isfinite(magnitude));
            RC_ASSERT(heaven::score_from_rate(-magnitude) == 0.0);
            RC_ASSERT(heaven::score_from_rate(0.0) == 0.0);
            RC_ASSERT(heaven::score_from_rate(
                std::numeric_limits<double>::infinity()) == 0.0);
            RC_ASSERT(heaven::score_from_rate(
                std::numeric_limits<double>::quiet_NaN()) == 0.0);
            RC_ASSERT(heaven::score_from_cpu_rate(-magnitude) == 0.0);
            RC_ASSERT(heaven::score_from_cpu_rate(0.0) == 0.0);
            RC_ASSERT(heaven::score_from_cpu_rate(
                std::numeric_limits<double>::infinity()) == 0.0);
            RC_ASSERT(heaven::score_from_cpu_rate(
                std::numeric_limits<double>::quiet_NaN()) == 0.0);
        });
}

const heaven::testing::Register g_register_score_guard{
    run_score_guard_properties};

} // namespace
