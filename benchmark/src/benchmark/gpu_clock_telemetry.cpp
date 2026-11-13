#include "benchmark/gpu_clock_telemetry.h"
#include "benchmark/microprofiler.h"

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <format>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>

namespace heaven::profile {
namespace {
std::optional<unsigned> read_unsigned(const std::filesystem::path& path, int base = 10) {
    std::ifstream input(path);
    std::string token, extra;
    if (!(input >> token) || (input >> extra)) return std::nullopt;
    std::string_view digits(token);
    if (base == 16 && digits.starts_with("0x")) digits.remove_prefix(2);
    unsigned value = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) return std::nullopt;
    return value;
}
}

GpuClockDiscovery discover_xe_clocks(const PciIdentity& device, const std::filesystem::path& pci_root) {
    GpuClockDiscovery result;
    if (device.vendor != 0x8086 || device.domain < 0 || device.domain > 0xffff ||
            device.bus < 0 || device.bus > 0xff || device.slot < 0 || device.slot > 0x1f ||
            device.function < 0 || device.function > 7) {
        result.reason = "Intel xe clock telemetry requires the active adapter's complete PCI identity";
        return result;
    }
    result.pci_address = std::format("{:04x}:{:02x}:{:02x}.{:x}",
        device.domain, device.bus, device.slot, device.function);
    const auto directory = pci_root / result.pci_address;
    if (read_unsigned(directory / "vendor", 16) != device.vendor ||
            read_unsigned(directory / "device", 16) != device.device) {
        result.reason = "PCI sysfs identity differs from the active rendering adapter or is unreadable";
        return result;
    }
    // xe topology is sparse. Bound discovery independently of driver contents.
    for (unsigned tile = 0; tile < 8; ++tile) {
        for (unsigned gt = 0; gt < 16; ++gt) {
            const auto label = std::format("tile{}/gt{}", tile, gt);
            const auto clock = directory / label / "freq0";
            std::error_code error;
            if (std::filesystem::is_directory(clock, error)) result.sources.push_back({clock, label});
        }
    }
    result.reason = result.sources.empty()
        ? "No xe tile/GT frequency sources are available for the active adapter"
        : "Read-only xe sysfs MHz samples at 1 Hz; host observation times, not GPU clock calibration or power measurements";
    return result;
}

GpuClockReading read_xe_clocks(const GpuClockSource& source) {
    return {read_unsigned(source.directory / "act_freq"), read_unsigned(source.directory / "cur_freq"),
        read_unsigned(source.directory / "min_freq"), read_unsigned(source.directory / "max_freq")};
}

struct GpuClockTelemetry::Impl {
    Recorder& recorder;
    GpuClockDiscovery discovery;
    std::mutex mutex;
    std::condition_variable_any wake;
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
    std::jthread worker;
#endif

    Impl(Recorder& target, GpuClockDiscovery sources) : recorder(target), discovery(std::move(sources)) {
        recorder.set_metadata("gpu_clock_telemetry_pci", discovery.pci_address);
        recorder.set_metadata("gpu_clock_telemetry_reason", discovery.reason);
        if (discovery.sources.empty()) return;
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
        worker = std::jthread([this](const std::stop_token& stop) {
            // Cache names before polling so steady-state callbacks allocate no strings.
            struct Names { std::string actual, requested, minimum, maximum, available; };
            std::vector<Names> names;
            for (const auto& source : discovery.sources) {
                const auto prefix = "Xe " + source.label;
                names.push_back({prefix + " actual MHz", prefix + " requested MHz",
                    prefix + " minimum MHz", prefix + " maximum MHz", prefix + " frequency available"});
            }
            bool registered = false;
            while (!stop.stop_requested()) {
                // The first two CPU rings protect high-volume main/backend
                // work. Let the GPU producer register before this 1 Hz source
                // so telemetry cannot claim most of the backend's history.
                if (recorder.active() && recorder.has_gpu_observations()) {
                    if (!registered) {
                        recorder.set_thread_name("GPU clock telemetry");
                        registered = true;
                    }
                    const Context context{0, 0, 0, 0, 0, recorder.session_id()};
                    for (std::size_t i = 0; i < discovery.sources.size(); ++i) {
                        const auto value = read_xe_clocks(discovery.sources[i]);
                        recorder.counter(names[i].available, value.actual_mhz.has_value() ? 1.0 : 0.0,
                            "Host GPU telemetry", context);
                        if (value.actual_mhz) recorder.counter(names[i].actual, *value.actual_mhz, "Host GPU telemetry", context);
                        if (value.requested_mhz) recorder.counter(names[i].requested, *value.requested_mhz, "Host GPU telemetry", context);
                        if (value.minimum_mhz) recorder.counter(names[i].minimum, *value.minimum_mhz, "Host GPU telemetry", context);
                        if (value.maximum_mhz) recorder.counter(names[i].maximum, *value.maximum_mhz, "Host GPU telemetry", context);
                    }
                }
                std::unique_lock lock(mutex);
                wake.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
            }
        });
#else
        recorder.set_metadata("gpu_clock_telemetry_reason",
            "Read-only xe clock telemetry unavailable: the C++ standard library does not provide std::jthread/stop-token support");
#endif
    }
};

GpuClockTelemetry::GpuClockTelemetry(Recorder& recorder, GpuClockDiscovery discovery)
    : impl_(std::make_unique<Impl>(recorder, std::move(discovery))) {}
GpuClockTelemetry::~GpuClockTelemetry() = default;
} // namespace heaven::profile
