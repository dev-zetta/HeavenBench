/*
 * HeavenBench — Deterministic real-scene CPU benchmark phase
 */
#ifndef HEAVENBENCH_SRC_BENCHMARK_CPU_BENCHMARK_H_
#define HEAVENBENCH_SRC_BENCHMARK_CPU_BENCHMARK_H_

#include "benchmark/score_calculator.h"
#include "engine/asset_file_system.h"
#include "engine/types.h"

#include <string>

namespace heaven {

struct CameraPath;
class RhiMaterialDatabase;

// Load an independent copy of the real scene through the NullBackend, warm its
// simulation state, then execute deterministic 60 Hz world updates for a fixed
// wall-clock measurement window. Asset loading and tessellation happen before
// the timer and therefore do not contribute to the CPU score.
// Reuse the renderer's mounted assets (including generated memory assets) when
// provided; otherwise mount config.data_dir locally. These calls are synchronous
// and retain the context through loading and all worker joins.
[[nodiscard]] Result<CpuBenchmarkMetrics> run_cpu_benchmark(
    const AppConfig& config,
    RhiMaterialDatabase& materials,
    const std::string& cache_dir,
    const CameraPath& camera_path,
    double measurement_seconds = 3.0,
    AssetFileSystemRef assets = std::nullopt);

[[nodiscard]] Result<CpuStressMetrics> run_cpu_stress_benchmark(
    const AppConfig& config,
    RhiMaterialDatabase& materials,
    const std::string& cache_dir,
    const CameraPath& camera_path,
    AssetFileSystemRef assets = std::nullopt);

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_CPU_BENCHMARK_H_
