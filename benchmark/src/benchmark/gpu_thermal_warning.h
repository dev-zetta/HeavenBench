#ifndef HEAVENBENCH_SRC_BENCHMARK_GPU_THERMAL_WARNING_H_
#define HEAVENBENCH_SRC_BENCHMARK_GPU_THERMAL_WARNING_H_

#include <cstdint>
#include <string_view>

namespace heaven {

enum class ThermalSeverity { Normal, High, Extreme };

struct GpuThermalThresholds {
    double high_c = 80.0;
    double extreme_c = 90.0;
};

struct GpuThermalSample {
    double gpu_c = -1.0;
    double vram_c = -1.0;
    double gpu_high_c = -1.0;
    double gpu_critical_c = -1.0;
    double vram_high_c = -1.0;
    double vram_critical_c = -1.0;
    double age_seconds = -1.0;
    std::uint64_t sample_monotonic_ns = 0;
};

struct GpuThermalStatus {
    ThermalSeverity severity = ThermalSeverity::Normal;
    ThermalSeverity gpu = ThermalSeverity::Normal;
    ThermalSeverity vram = ThermalSeverity::Normal;
    GpuThermalThresholds gpu_thresholds;
    GpuThermalThresholds vram_thresholds{90.0, 100.0};
};

[[nodiscard]] std::string_view thermal_severity_label(ThermalSeverity severity);

// Read-only advisory policy in Celsius. Defaults are conservative warning
// ceilings, not claimed manufacturer operating limits. Plausible driver limits
// can lower them, but never raise them. Missing/stale sensors are not healthy
// zero readings and cannot sustain an alert.
class GpuThermalWarning {
public:
    [[nodiscard]] GpuThermalStatus update(const GpuThermalSample& sample);
    void reset();

private:
    struct SensorState {
        ThermalSeverity severity = ThermalSeverity::Normal;
        unsigned high_samples = 0;
    };

    static void update_sensor(SensorState& state, double temperature_c,
                              GpuThermalThresholds limits);
    SensorState gpu_;
    SensorState vram_;
    std::uint64_t last_sample_ns_ = 0;
    GpuThermalStatus status_;
};

} // namespace heaven
#endif
