#ifndef HEAVENBENCH_SRC_BENCHMARK_GPU_CLOCK_TELEMETRY_H_
#define HEAVENBENCH_SRC_BENCHMARK_GPU_CLOCK_TELEMETRY_H_

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heaven::profile {
class Recorder;

struct PciIdentity {
    unsigned vendor = 0, device = 0;
    int domain = -1, bus = -1, slot = -1, function = -1;
};
struct GpuClockSource {
    std::filesystem::path directory;
    std::string label;
};
struct GpuClockDiscovery {
    std::string pci_address, reason;
    std::vector<GpuClockSource> sources;
};
struct GpuClockReading {
    std::optional<unsigned> actual_mhz, requested_mhz, minimum_mhz, maximum_mhz;
};

// Read-only xe telemetry for the renderer's exact PCI adapter. Missing paths or
// identity mismatches are unavailable; never substitute another DRM adapter.
[[nodiscard]] GpuClockDiscovery discover_xe_clocks(const PciIdentity& device,
        const std::filesystem::path& pci_root = "/sys/bus/pci/devices");
[[nodiscard]] GpuClockReading read_xe_clocks(const GpuClockSource& source);

// One host sample per second on a worker, independently of GPU frame clocks.
// No graphics calls, hardware writes, main-loop file I/O or implicit recording.
class GpuClockTelemetry {
public:
    GpuClockTelemetry(Recorder& recorder, GpuClockDiscovery discovery);
    ~GpuClockTelemetry();
    GpuClockTelemetry(const GpuClockTelemetry&) = delete;
    GpuClockTelemetry& operator=(const GpuClockTelemetry&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace heaven::profile
#endif
