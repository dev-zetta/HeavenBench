/*
 * HeavenBench — application build identity
 */
#ifndef HEAVENBENCH_SRC_BENCHMARK_BUILD_INFO_H_
#define HEAVENBENCH_SRC_BENCHMARK_BUILD_INFO_H_

#include <string>

namespace heaven {

struct ApplicationBuildInfo {
    std::string version;
    std::string architecture;
};

[[nodiscard]] inline std::string format_application_title(
        const ApplicationBuildInfo& info) {
    return "HeavenBench " + info.version + " [" + info.architecture + "]";
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_BUILD_INFO_H_
