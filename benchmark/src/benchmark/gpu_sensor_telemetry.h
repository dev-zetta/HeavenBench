#ifndef HEAVENBENCH_SRC_BENCHMARK_GPU_SENSOR_TELEMETRY_H_
#define HEAVENBENCH_SRC_BENCHMARK_GPU_SENSOR_TELEMETRY_H_

#include "benchmark/gpu_clock_telemetry.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace heaven::profile {

// Negative values are unavailable, never measured zero. These are host sensor
// observations for the selected adapter, not GPU-calibrated timestamps.
struct GpuSensorReading {
    double temperature_c = -1.0;
    double vram_temperature_c = -1.0;
    double temperature_high_c = -1.0;
    double temperature_critical_c = -1.0;
    double vram_temperature_high_c = -1.0;
    double vram_temperature_critical_c = -1.0;
    double power_w = -1.0;
    double power_limit_w = -1.0;
    double core_clock_mhz = -1.0;
    double memory_clock_mhz = -1.0;
    double fan_rpm = -1.0;
    double utilization_percent = -1.0;
    double memory_utilization_percent = -1.0;
    double device_vram_used_mb = -1.0;
    double device_vram_total_mb = -1.0;
    std::uint64_t sample_monotonic_ns = 0;
    std::string reason;
    // Empty is unavailable; "None" means the hardware explicitly reports no
    // throttling. Any other nonempty string is a hardware-reported active
    // reason (or an active flag whose individual reasons are unavailable).
    std::string throttle_reason;
    bool available = false;
};

struct GpuSensorDiscovery {
    PciIdentity identity;
    std::filesystem::path device_directory;
    std::string pci_address;
    std::string reason;
};

// The optional counter history belongs to one sampling worker. Counter resets,
// source changes and host intervals outside 0.1..5 seconds produce no power
// estimate; caps are always independent metadata rather than measured draw.
struct GpuSensorEnergyState {
    std::filesystem::path source;
    std::uint64_t microjoules = 0;
    std::uint64_t sample_ns = 0;
    bool initialized = false;
};

// Discovery/read are public to permit ordinary-filesystem fixtures. Exact PCI
// identities never fall back to another adapter; a missing BDF is accepted only
// when vendor/device IDs uniquely identify one directory under pci_root.
[[nodiscard]] GpuSensorDiscovery discover_gpu_sensors(const PciIdentity& identity,
    const std::filesystem::path& pci_root = "/sys/bus/pci/devices");
[[nodiscard]] GpuSensorReading read_gpu_sensors(const GpuSensorDiscovery& source,
    GpuSensorEnergyState* energy = nullptr, std::uint64_t sample_ns = 0);

// Only this worker performs sensor I/O. latest() copies a mutex-protected
// snapshot; shutdown requests stop and joins before its state is destroyed.
class GpuSensorTelemetry {
public:
    explicit GpuSensorTelemetry(PciIdentity identity,
        std::filesystem::path pci_root = "/sys/bus/pci/devices");
    ~GpuSensorTelemetry();
    GpuSensorTelemetry(const GpuSensorTelemetry&) = delete;
    GpuSensorTelemetry& operator=(const GpuSensorTelemetry&) = delete;
    [[nodiscard]] GpuSensorReading latest() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven::profile
#endif
