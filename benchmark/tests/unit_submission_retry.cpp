/* HeavenBench — rejected-submission backoff without GPU dependencies. */
#include "engine/submission_retry.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

bool expect_delay(const heaven::SubmissionRetryDelay& retry, double expected,
        const char* context) {
    const double actual = retry.delay().count();
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-12) {
        std::printf("[unit_submission_retry] FAIL: %s: %.17g ms, expected %.17g ms\n",
            context, actual, expected);
        return false;
    }
    return true;
}

bool unit_submission_retry_invalid_and_reset() {
    heaven::SubmissionRetryDelay retry;
    bool ok = expect_delay(retry, 1.0, "initial fallback");
    const std::array invalid{
        -1.0, 0.0, -0.0, -128.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for (const double value : invalid) {
        retry.observe_gpu_ms(value);
        ok = expect_delay(retry, 1.0, "invalid initial sample") && ok;
    }

    retry.observe_gpu_ms(48.0);
    ok = expect_delay(retry, 3.0, "valid sample") && ok;
    for (const double value : invalid) {
        retry.observe_gpu_ms(value);
        ok = expect_delay(retry, 3.0, "invalid sample replaced retained timing") && ok;
    }

    retry.reset();
    ok = expect_delay(retry, 1.0, "reset fallback") && ok;
    retry.observe_gpu_ms(-1.0);
    ok = expect_delay(retry, 1.0, "missing sample after reset") && ok;
    retry.observe_gpu_ms(4.0);
    ok = expect_delay(retry, 0.25, "valid sample after reset") && ok;
    retry.reset();
    retry.reset();
    return expect_delay(retry, 1.0, "repeated reset") && ok;
}

bool unit_submission_retry_scaling_and_cap() {
    heaven::SubmissionRetryDelay retry;
    struct Example { double gpu_ms; double expected_delay_ms; };
    constexpr std::array examples{
        Example{0.125, 0.0078125},
        Example{2.0, 0.125},
        Example{8.0, 0.5},
        Example{16.0, 1.0},
        Example{32.0, 2.0},
        Example{127.0, 7.9375},
        Example{128.0, 8.0},
        Example{129.0, 8.0},
        Example{1000.0, 8.0},
        Example{std::numeric_limits<double>::max(), 8.0},
        Example{1.0, 0.0625}};
    bool ok = true;
    for (const auto example : examples) {
        retry.observe_gpu_ms(example.gpu_ms);
        ok = expect_delay(retry, example.expected_delay_ms, "scaling or cap") && ok;
    }
    return ok;
}

bool unit_submission_retry_delayed_arrivals() {
    heaven::SubmissionRetryDelay retry;
    // Delayed GPU results need not arrive in submission order. The policy is
    // deliberately based on the last valid arrival, not maximum frame ID,
    // maximum duration, or an average. Missing per-attempt reports retain it.
    struct Arrival {
        unsigned source_frame;
        double reported_gpu_ms;
        double expected_delay_ms;
    };
    const std::array arrivals{
        Arrival{0, -1.0, 1.0},
        Arrival{0, -1.0, 1.0},
        Arrival{101, 32.0, 2.0},
        Arrival{0, -1.0, 2.0},
        Arrival{100, 8.0, 0.5},
        Arrival{103, 64.0, 4.0},
        Arrival{0, std::numeric_limits<double>::quiet_NaN(), 4.0},
        Arrival{102, 2.0, 0.125},
        Arrival{104, 160.0, 8.0},
        Arrival{0, -1.0, 8.0},
        Arrival{105, 12.0, 0.75}};
    bool ok = true;
    for (const auto arrival : arrivals) {
        retry.observe_gpu_ms(arrival.reported_gpu_ms);
        char context[64];
        std::snprintf(context, sizeof(context), "arrival from frame %u", arrival.source_frame);
        ok = expect_delay(retry, arrival.expected_delay_ms, context) && ok;
    }
    // Retaining a backoff does not turn the current attempt's missing GPU
    // report into a measurement: the helper accepts a value and only exposes
    // a delay, so callers keep reporting the original missing sentinel.
    const double reported_gpu_ms = -1.0;
    retry.observe_gpu_ms(reported_gpu_ms);
    ok = expect_delay(retry, 0.75, "missing report after a valid arrival") && ok;
    return ok;
}

const heaven::testing::Register kRegisterInvalid(unit_submission_retry_invalid_and_reset);
const heaven::testing::Register kRegisterScaling(unit_submission_retry_scaling_and_cap);
const heaven::testing::Register kRegisterDelayed(unit_submission_retry_delayed_arrivals);

} // namespace
