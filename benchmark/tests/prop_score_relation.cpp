/*
 * HeavenBench — score-v3 rate normalization property
 */
#include <algorithm>
#include <cmath>
#include <functional>
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

bool approx_equal(double a, double b) {
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= 1e-9 * scale;
}

bool run_score_relation_property() {
    return rc::check(
        "score v3 is linear in positive workload rate",
        [] {
            const double rate = *rc::gen::map(
                rc::gen::inRange<int>(1, 1'000'000),
                [](int milli_rate) {
                    return static_cast<double>(milli_rate) / 1000.0;
                });
            const double expected =
                heaven::kBaselinePoints * rate / heaven::kBaselineRate;
            RC_ASSERT(approx_equal(
                heaven::score_from_rate(rate), expected));
            const double expected_cpu =
                heaven::kBaselinePoints * rate /
                heaven::kCpuReferenceRate;
            RC_ASSERT(approx_equal(
                heaven::score_from_cpu_rate(rate), expected_cpu));
        });
}

const heaven::testing::Register reg_score_relation{
    run_score_relation_property};

} // namespace
