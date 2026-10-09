/*
 * HeavenBench — automatic benchmark result reports
 *
 * The document renderer is GPU-free and deterministic for a supplied timestamp.
 * The filesystem wrapper creates the executable-owned results directory and
 * publishes a matched JSON/HTML pair without exposing partial files.
 */
#ifndef HEAVENBENCH_SRC_BENCHMARK_RESULT_REPORT_H_
#define HEAVENBENCH_SRC_BENCHMARK_RESULT_REPORT_H_

#include "benchmark/benchmark_system.h"
#include "benchmark/build_info.h"
#include "engine/cli_parser.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace heaven {

inline constexpr int kResultReportSchemaVersion = 2;

struct BenchmarkReportDocument {
    std::string report_id;
    std::string json;
    std::string html;
};

struct BenchmarkReportWriteResult {
    std::filesystem::path json_path;
    std::filesystem::path html_path;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool success() const {
        return diagnostics.empty() && !json_path.empty() && !html_path.empty();
    }
};

[[nodiscard]] BenchmarkReportDocument build_benchmark_report(
    const RunResult& result,
    const BenchConfig& config,
    const ApplicationBuildInfo& application,
    std::chrono::system_clock::time_point timestamp);

[[nodiscard]] BenchmarkReportWriteResult write_benchmark_report(
    const RunResult& result,
    const BenchConfig& config,
    const std::filesystem::path& results_directory,
    const ApplicationBuildInfo& application,
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now());

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_RESULT_REPORT_H_
