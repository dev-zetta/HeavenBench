#include "benchmark/gpu_thermal_warning.h"

#include <algorithm>
#include <cmath>

namespace heaven {
namespace {
constexpr double k_hysteresis_c = 3.0;
constexpr double k_maximum_age_seconds = 5.0;
constexpr unsigned k_high_sample_count = 2;

bool valid_temperature(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 150.0;
}

GpuThermalThresholds thresholds(double driver_high, double driver_critical,
                                bool vram) {
    GpuThermalThresholds result = vram
        ? GpuThermalThresholds{90.0, 100.0} : GpuThermalThresholds{80.0, 90.0};
    const double minimum_critical = vram ? 85.0 : 75.0;
    if (std::isfinite(driver_critical) && driver_critical >= minimum_critical &&
            driver_critical <= 125.0) {
        result.extreme_c = std::min(result.extreme_c, driver_critical);
        result.high_c = std::min(result.high_c, driver_critical - 10.0);
    }
    // xe's pkg temp_max can report 60 C, which is not a useful warning
    // threshold. Treat implausible/inverted values as unavailable instead of
    // issuing a constant alarm on an otherwise normally operating card.
    const double minimum_high = vram ? 75.0 : 65.0;
    if (std::isfinite(driver_high) && driver_high >= minimum_high &&
            driver_high <= result.extreme_c - k_hysteresis_c) {
        result.high_c = std::min(result.high_c, driver_high);
    }
    return result;
}
}

std::string_view thermal_severity_label(ThermalSeverity severity) {
    switch (severity) {
        case ThermalSeverity::Normal: return "Normal";
        case ThermalSeverity::High: return "High";
        case ThermalSeverity::Extreme: return "Extreme";
    }
    return "Normal";
}

void GpuThermalWarning::reset() {
    gpu_ = {};
    vram_ = {};
    last_sample_ns_ = 0;
    status_ = {};
}

void GpuThermalWarning::update_sensor(SensorState& state, double temperature_c,
                                      GpuThermalThresholds limits) {
    if (!valid_temperature(temperature_c)) {
        state = {};
        return;
    }
    if (temperature_c >= limits.extreme_c ||
            (state.severity == ThermalSeverity::Extreme &&
             temperature_c >= limits.extreme_c - k_hysteresis_c)) {
        state = {ThermalSeverity::Extreme, k_high_sample_count};
        return;
    }
    if (state.severity != ThermalSeverity::Normal &&
            temperature_c >= limits.high_c - k_hysteresis_c) {
        state = {ThermalSeverity::High, k_high_sample_count};
        return;
    }
    state.severity = ThermalSeverity::Normal;
    if (temperature_c >= limits.high_c) {
        state.high_samples = std::min(k_high_sample_count, state.high_samples + 1);
        if (state.high_samples == k_high_sample_count) {
            state.severity = ThermalSeverity::High;
        }
    } else {
        state.high_samples = 0;
    }
}

GpuThermalStatus GpuThermalWarning::update(const GpuThermalSample& sample) {
    if (sample.sample_monotonic_ns == 0 || !std::isfinite(sample.age_seconds) ||
            sample.age_seconds < 0.0 || sample.age_seconds > k_maximum_age_seconds ||
            (!valid_temperature(sample.gpu_c) && !valid_temperature(sample.vram_c))) {
        reset();
        return status_;
    }
    // A cached sample may be drawn many times. Only genuinely new samples can
    // advance the two-sample high-temperature persistence requirement.
    if (sample.sample_monotonic_ns <= last_sample_ns_) return status_;
    last_sample_ns_ = sample.sample_monotonic_ns;
    status_.gpu_thresholds = thresholds(sample.gpu_high_c, sample.gpu_critical_c, false);
    status_.vram_thresholds = thresholds(sample.vram_high_c, sample.vram_critical_c, true);
    update_sensor(gpu_, sample.gpu_c, status_.gpu_thresholds);
    update_sensor(vram_, sample.vram_c, status_.vram_thresholds);
    status_.gpu = gpu_.severity;
    status_.vram = vram_.severity;
    status_.severity = std::max(status_.gpu, status_.vram);
    return status_;
}

} // namespace heaven
