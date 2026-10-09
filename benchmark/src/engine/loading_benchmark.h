/*
 * HeavenBench — headless loose/ZIP loading benchmark
 */
#ifndef HEAVENBENCH_SRC_ENGINE_LOADING_BENCHMARK_H_
#define HEAVENBENCH_SRC_ENGINE_LOADING_BENCHMARK_H_

#include "engine/cli_parser.h"

#include <filesystem>

namespace heaven {

[[nodiscard]] Result<void> run_loading_benchmark(
    const BenchConfig& config, const std::filesystem::path& output_dir);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_LOADING_BENCHMARK_H_
