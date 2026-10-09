/*
 * HeavenBench — score-v3 baseline unit test
 */
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "benchmark/score_calculator.h"

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool unit_score_scale() {
    const double score = heaven::score_from_rate(60.0);
    const double cpu_score =
        heaven::score_from_cpu_rate(heaven::kCpuReferenceRate);
    const bool ok =
        heaven::kScoreVersion == 3 &&
        heaven::kBaselinePoints == 1000.0 &&
        heaven::kBaselineRate == 60.0 &&
        heaven::kCpuReferenceRate == 10'000.0 &&
        std::fabs(score - 1000.0) < 1e-9 &&
        std::fabs(cpu_score - 1000.0) < 1e-9;
    if (!ok) {
        std::printf(
            "[unit_score_scale] FAILED: version=%d baseline=%.1f@%.1f "
            "score(60)=%.17g cpu-reference=%.1f cpu-score=%.17g\n",
            heaven::kScoreVersion, heaven::kBaselinePoints,
            heaven::kBaselineRate, score, heaven::kCpuReferenceRate,
            cpu_score);
    }
    return ok;
}

const heaven::testing::Register reg_unit_score_scale{unit_score_scale};

} // namespace
