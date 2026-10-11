/*
 * HeavenBench — gated example test: LinuxHostInfoSource
 *
 *
 *
 * has the Benchmark_System obtain, from the host operating
 * system, the operating system name, the kernel version, and the machine
 * architecture for the Platform_Description.  has it obtain the
 * processor model name, the clock rate, and a logical-core count of at least 1
 * for the CPU_Description. LinuxHostInfoSource is the concrete impure seam that
 * reads those values via uname(2), sysconf(_SC_NPROCESSORS_ONLN), and
 * proc/cpuinfo.
 *
 * This is an EXAMPLE test (not a RapidCheck property): it runs the real source
 * against the host and asserts the host-derived raw comes back populated. It is
 * GATED to Linux and macOS hosts: other platforms currently return empty
 * sentinels, so the test skips rather than asserting unavailable host data.
 *
 * On supported hosts it asserts:
 *   - PlatformRaw comes back populated: non-empty os_name AND non-empty
 *     machine_arch (the two fields uname always supplies;).
 *   - CpuRaw reports at least one logical core ('s "no less
 *     than 1" core count).
 *
 * The test registers itself with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, following
 * the same self-registration pattern as the other test translation units. The
 * registered callable returns true on success/skip and false (after printing a
 * diagnostic) on a genuine failure. No second main() is defined.
 */
#include <cstdio>
#include <functional>
#include <vector>

#include "benchmark/run_info.h"

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

bool unit_host_info_source() {
#if defined(__linux__) || defined(__APPLE__)
    heaven::LinuxHostInfoSource source;

    const heaven::PlatformRaw platform = source.query_platform();
    const heaven::CpuRaw      cpu       = source.query_cpu();

    bool ok = true;

    // uname(2) supplies the OS name and machine architecture
    // on every supported host, so both must come back populated.
    if (platform.os_name.empty()) {
        std::printf(
            "[unit_host_info_source] FAILED: PlatformRaw.os_name is empty\n");
        ok = false;
    }
    if (platform.machine_arch.empty()) {
        std::printf(
            "[unit_host_info_source] FAILED: PlatformRaw.machine_arch is "
            "empty\n");
        ok = false;
    }

    // the logical-core count must be at least 1.
    if (cpu.logical_cores < 1) {
        std::printf(
            "[unit_host_info_source] FAILED: CpuRaw.logical_cores is %d, "
            "expected >= 1\n",
            cpu.logical_cores);
        ok = false;
    }

    if (ok) {
        std::printf(
            "[unit_host_info_source] PASSED: platform \"%s %s %s\", "
            "%d logical core(s)\n",
            platform.os_name.c_str(), platform.kernel_version.c_str(),
            platform.machine_arch.c_str(), cpu.logical_cores);
    }
    return ok;
#else
    std::printf(
        "[unit_host_info_source] SKIPPED: not a Linux or macOS host "
        "(LinuxHostInfoSource has no host data to report here)\n");
    return true;
#endif
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_host_info_source{unit_host_info_source};

} // namespace
