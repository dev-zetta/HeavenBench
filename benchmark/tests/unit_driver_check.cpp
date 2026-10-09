#include "benchmark/driver_check.h"

#include <array>
#include <functional>
#include <iostream>
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

bool unit_driver_check_boundaries() {
    using enum heaven::DriverFamily;
    using enum heaven::DriverPlatform;
    using enum heaven::DriverCheckSeverity;
    struct Case {
        heaven::DriverPlatform platform;
        heaven::DriverFamily family;
        std::string_view version;
        heaven::DriverCheckSeverity severity;
        std::string_view summary_part;
    };
    // Verify both APIs' normalized forms, component ordering, prereleases,
    // platform isolation, unsupported schemas and non-comparable AMD numbers.
    constexpr std::array cases{
        Case{Linux, Mesa, "23.3.9", Warning, "predates"},
        Case{Linux, Mesa, "24.0.0", Information, "meets"},
        Case{Linux, Mesa, "24.0.0-devel", Warning, "predates"},
        Case{Linux, Mesa, "24.0.0-rc2", Warning, "predates"},
        Case{Linux, Mesa, "24.0.0-1ubuntu2", Information, "meets"},
        Case{Linux, Mesa, "25.2.0-devel", Information, "meets"},
        Case{Linux, Mesa, "24.10.0", Information, "meets"},
        Case{Windows, Mesa, "23.3.1", Warning, "predates"},
        Case{Linux, MesaNvk, "25.0.9", Warning, "predates"},
        Case{Linux, MesaNvk, "25.1.0", Information, "meets"},
        Case{Linux, MesaNvk, "25.1.0-devel", Warning, "predates"},
        Case{Linux, MesaNvk, "26.0.0", Information, "meets"},
        Case{Linux, Nvidia, "550.54.13", Warning, "predates"},
        Case{Linux, Nvidia, "550.54.14", Information, "meets"},
        Case{Linux, Nvidia, "550.54.14.0", Information, "meets"},
        Case{Linux, Nvidia, "550.54.14.1", Information, "meets"},
        Case{Linux, Nvidia, "550.54", Warning, "predates"},
        Case{Linux, Nvidia, "570.9", Information, "meets"},
        Case{Windows, Nvidia, "551.60", Warning, "predates"},
        Case{Windows, Nvidia, "551.61", Information, "meets"},
        Case{Windows, Nvidia, "550.54.14", Warning, "predates"},
        Case{Windows, Nvidia, "31.0.15.5161", Information, "could not"},
        Case{Windows, Intel, "101.5185", Warning, "predates"},
        Case{Windows, Intel, "101.5186", Information, "meets"},
        Case{Windows, Intel, "31.0.101.5185", Warning, "predates"},
        Case{Windows, Intel, "31.0.101.5186", Information, "meets"},
        Case{Windows, Intel, "27.20.100.9466", Warning, "predates"},
        Case{Windows, Intel, "31.0.102.1", Information, "meets"},
        Case{Windows, Intel, "10.18.10.5161", Information, "could not"},
        Case{Windows, Intel, "101.5186.0", Information, "could not"},
        Case{Linux, Intel, "101.1000", Information, "could not"},
        Case{Windows, Amd, "1.0.0", Information, "could not"},
        Case{Windows, Amd, "24.1.1", Information, "could not"},
        Case{Linux, Amd, "2.0.1", Information, "could not"},
        Case{Linux, Unknown, "1.0.0", Information, "could not"},
        Case{Other, Nvidia, "400.1", Information, "could not"},
        Case{Other, Software, "", Information, "could not"},
        Case{Windows, Software, "999.99", Warning, "Software rendering"},
        Case{Linux, Software, "25.2.0", Warning, "Software rendering"},
        Case{Linux, Nouveau, "25.2.0", Warning, "legacy Nouveau"},
    };
    for (const auto& test : cases) {
        const auto check = heaven::check_graphics_driver(
            {test.platform, test.family, test.version,
             test.family == Nouveau ? "OpenGL" : "Vulkan"});
        if (check.severity != test.severity ||
            !check.summary.contains(test.summary_part) || check.recommendation.empty()) {
            std::cerr << "Driver policy mismatch for version " << test.version << '\n';
            return false;
        }
        if (check.summary.contains("baseline (") &&
            (!check.recommendation.contains("not a required or latest") ||
             !check.recommendation.contains("Legacy hardware"))) return false;
        if (check.severity == Warning && !check.help_url.starts_with("https://")) return false;
    }
    return true;
}

bool unit_driver_check_unverified() {
    constexpr std::array invalid{
        "", "unknown", "0.0.0", "24", "24.", "24..0", "24.0.", "-24.0.0",
        "+24.0.0", "24.0.0.1.2", "999999999999999999999.0.0", "Mesa 24.0.0",
        "24.0.0 garbage", "24.0.0\n", "24.0.0-", "24.0.0-rc 2",
    };
    for (const std::string_view version : invalid) {
        for (const auto family : {heaven::DriverFamily::Mesa, heaven::DriverFamily::Nvidia,
                                  heaven::DriverFamily::Intel}) {
            const auto check = heaven::check_graphics_driver(
                {heaven::DriverPlatform::Windows, family, version, "OpenGL"});
            if (check.severity != heaven::DriverCheckSeverity::Information ||
                !check.summary.contains("could not")) return false;
        }
    }
    // API strings cannot override the supplied active driver family or turn
    // a missing driver version into an API-version check.
    const auto unknown = heaven::check_graphics_driver(
        {heaven::DriverPlatform::Linux, heaven::DriverFamily::Unknown, "", "OpenGL 4.6.0"});
    const auto amd = heaven::check_graphics_driver(
        {heaven::DriverPlatform::Windows, heaven::DriverFamily::Amd, "1.0.0", "Vulkan 1.3"});
    return unknown.severity == heaven::DriverCheckSeverity::Information &&
           amd.recommendation.contains("not reliably comparable with an Adrenalin") &&
           amd.help_url == "https://www.amd.com/en/support/download/drivers.html";
}

const heaven::testing::Register register_boundaries(unit_driver_check_boundaries);
const heaven::testing::Register register_unverified(unit_driver_check_unverified);

} // namespace
