#ifndef HEAVENBENCH_SRC_BENCHMARK_DRIVER_CHECK_H_
#define HEAVENBENCH_SRC_BENCHMARK_DRIVER_CHECK_H_

#include "engine/gpu_driver_info.h"

#include <string>
#include <string_view>

namespace heaven {

enum class DriverPlatform { Linux, Windows, Other };
enum class DriverCheckSeverity { Information, Warning };

struct DriverCheckInput {
    DriverPlatform platform = DriverPlatform::Other;
    DriverFamily family = DriverFamily::Unknown;
    std::string_view driver_version;
    std::string_view api_name;
};

struct DriverCheck {
    DriverCheckSeverity severity = DriverCheckSeverity::Information;
    std::string summary;
    std::string recommendation;
    std::string help_url;
};

// Offline, advisory-only policy for the active rendering device. Baselines are
// dated project recommendations, not vendor requirements or a latest-version
// lookup; see docs/GRAPHICS_DRIVERS.md for provenance and legacy-driver limits.
[[nodiscard]] DriverCheck check_graphics_driver(const DriverCheckInput& input);

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_DRIVER_CHECK_H_
