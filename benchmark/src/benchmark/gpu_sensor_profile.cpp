#include "benchmark/gpu_sensor_profile.h"

#include "benchmark/gpu_sensor_telemetry.h"
#include "benchmark/microprofiler.h"

#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace heaven::profile {
namespace {
constexpr std::string_view kCategory = "Host GPU telemetry";
constexpr std::size_t kReasonBytes = 256;

struct Metric {
    std::string_view name;
    std::string_view availability;
    double GpuSensorReading::*value;
};

constexpr std::array kMetrics{
    Metric{"GPU temperature C", "GPU temperature available", &GpuSensorReading::temperature_c},
    Metric{"GPU VRAM temperature C", "GPU VRAM temperature available", &GpuSensorReading::vram_temperature_c},
    Metric{"GPU temperature high C", "GPU temperature high available", &GpuSensorReading::temperature_high_c},
    Metric{"GPU temperature critical C", "GPU temperature critical available", &GpuSensorReading::temperature_critical_c},
    Metric{"GPU VRAM temperature high C", "GPU VRAM temperature high available", &GpuSensorReading::vram_temperature_high_c},
    Metric{"GPU VRAM temperature critical C", "GPU VRAM temperature critical available", &GpuSensorReading::vram_temperature_critical_c},
    Metric{"GPU power W", "GPU power available", &GpuSensorReading::power_w},
    Metric{"GPU power limit W", "GPU power limit available", &GpuSensorReading::power_limit_w},
    Metric{"GPU graphics clock MHz", "GPU graphics clock available", &GpuSensorReading::core_clock_mhz},
    Metric{"GPU memory clock MHz", "GPU memory clock available", &GpuSensorReading::memory_clock_mhz},
    Metric{"GPU fan RPM", "GPU fan available", &GpuSensorReading::fan_rpm},
    Metric{"GPU utilization percent", "GPU utilization available", &GpuSensorReading::utilization_percent},
    Metric{"GPU memory utilization percent", "GPU memory utilization available", &GpuSensorReading::memory_utilization_percent},
    Metric{"GPU device VRAM used MiB", "GPU device VRAM used available", &GpuSensorReading::device_vram_used_mb},
    Metric{"GPU device VRAM total MiB", "GPU device VRAM total available", &GpuSensorReading::device_vram_total_mb},
};

std::string bounded_reason(std::string_view text, std::string_view fallback) {
    if (text.empty()) text = fallback;
    if (text.size() <= kReasonBytes) return std::string(text);
    constexpr std::string_view suffix = " [truncated]";
    std::size_t count = kReasonBytes - suffix.size();
    // Leave a complete UTF-8 prefix when a driver label crosses the bound.
    while (count > 0 && (static_cast<unsigned char>(text[count]) & 0xc0u) == 0x80u) --count;
    return std::string(text.substr(0, count)) + std::string(suffix);
}
} // namespace

void record_gpu_sensor_sample(Recorder& recorder, const GpuSensorReading& reading) {
    if (!recorder.active()) return;

    // These observations span many rendered frames and may arrive late. Never
    // inherit the caller's latest application, submission, view or GPU IDs.
    const Context context{0, 0, 0, 0, 0, recorder.session_id()};
    recorder.counter("GPU sensors available", reading.available ? 1.0 : 0.0, kCategory, context);
    for (const auto& metric : kMetrics) {
        const double value = reading.*(metric.value);
        const bool available = std::isfinite(value) && value >= 0.0;
        recorder.counter(metric.availability, available ? 1.0 : 0.0, kCategory, context);
        if (available) recorder.counter(metric.name, value, kCategory, context);
    }

    const auto now = Recorder::steady_now_ns();
    const bool sample_time_available = reading.sample_monotonic_ns != 0 && reading.sample_monotonic_ns <= now;
    recorder.counter("GPU sensor sample age available", sample_time_available ? 1.0 : 0.0, kCategory, context);
    if (sample_time_available) {
        recorder.counter("GPU sensor sample age ms",
            static_cast<double>(now - reading.sample_monotonic_ns) / 1'000'000.0, kCategory, context);
    }

    // The provider reserves exactly "None" for a measured clear throttle
    // status. Empty is unsupported/unreadable, never a false "not throttled".
    const bool throttle_available = !reading.throttle_reason.empty();
    recorder.counter("GPU throttle status available", throttle_available ? 1.0 : 0.0, kCategory, context);
    if (throttle_available) {
        recorder.counter("GPU throttled", reading.throttle_reason == "None" ? 0.0 : 1.0, kCategory, context);
    }

    // Fixed keys retain only the latest bounded state. Changes cannot grow a
    // label/string table, while availability and throttle counters keep history.
    recorder.set_metadata("gpu_sensor_clock",
        "Host monotonic observations; counter timestamps are import times. Sample age describes the cached reading. No GPU clock calibration or per-frame attribution.");
    recorder.set_metadata("gpu_sensor_sample_monotonic_ns", std::to_string(reading.sample_monotonic_ns));
    recorder.set_metadata("gpu_sensor_reason", bounded_reason(reading.reason,
        reading.available ? "Available" : "Unavailable"));
    recorder.set_metadata("gpu_sensor_throttle_reason", bounded_reason(reading.throttle_reason, "Unavailable"));
}

} // namespace heaven::profile
