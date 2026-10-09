/*
 * HeavenBench — RapidCheck test harness entry point
 *
 * This file provides the single main() for the host-runnable
 * `heaven_benchmark_tests` target. The pure-logic classes under
 * `src/benchmark/` and `engine/camera_path.*` are exercised here with
 * RapidCheck property-based tests plus plain example assertions.
 *
 * RapidCheck is a TEST-ONLY vendored dependency (lib/rapidcheck): it is
 * never linked into the shipping `heaven_benchmark` executable
 * (AGENTS.md  — no new runtime dependencies).
 *
 * Individual property / unit test translation units (added by later spec
 * tasks) register themselves with this harness via the small registry
 * below, so new test files under benchmark/tests are picked up by the
 * file(GLOB) in CMake without editing this entry point.
 */
#include <cstdio>
#include <exception>
#include <functional>
#include <vector>

#include <rapidcheck.h>

namespace heaven::testing {

// Each test TU appends a callable returning `true` on success. Keeping the
// registry in a function-local static avoids static-initialisation-order
// problems across translation units.
using TestFn = std::function<bool()>;

// Other test translation units use this externally linked registry.
std::vector<TestFn>& registry(); // NOLINT(misc-use-internal-linkage): shared by every test translation unit.

std::vector<TestFn>& registry() {
    static std::vector<TestFn> tests;
    return tests;
}

// Helper used by test TUs: register a test at static-init time.
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

int main() try {
    // Trivial smoke check so the target configures, links, and runs before
    // any real logic exists. Exercising rc::check confirms RapidCheck is
    // wired in correctly.
    bool ok = rc::check("smoke: addition is commutative",
                        [](unsigned a, unsigned b) { RC_ASSERT(a + b == b + a); });

    // Run any tests registered by other translation units.
    std::size_t check_index = 0;
    for (const auto& test : heaven::testing::registry()) {
        const bool passed = test();
        if (!passed) {
            std::fprintf(stderr, "[heaven_benchmark_tests] registered check %zu failed (%s)\n",
                         check_index, test.target_type().name());
        }
        ok = passed && ok;
        ++check_index;
    }

    if (ok) {
        std::printf("[heaven_benchmark_tests] all checks passed\n");
        return 0;
    }
    std::printf("[heaven_benchmark_tests] FAILED\n");
    return 1;
} catch (const std::exception& error) {
    std::fprintf(stderr, "[heaven_benchmark_tests] exception: %s\n", error.what());
    return 1;
} catch (...) {
    std::fprintf(stderr, "[heaven_benchmark_tests] unknown exception\n");
    return 1;
}
