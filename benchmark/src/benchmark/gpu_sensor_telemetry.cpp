#include "benchmark/gpu_sensor_telemetry.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace heaven::profile {
namespace {
constexpr std::size_t k_max_pci_entries = 4096;
constexpr std::size_t k_max_hwmon = 8;
constexpr std::size_t k_max_sensor_channels = 64;
constexpr std::size_t k_max_hwmon_entries = 1024;

std::uint64_t steady_ns() {
    const auto value = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
}

std::optional<std::string> read_token(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::array<char, 257> bytes{};
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = file.gcount();
    if (count <= 0 || std::cmp_greater_equal(count, bytes.size())) return std::nullopt;
    std::string text(bytes.data(), static_cast<std::size_t>(count));
    constexpr std::string_view spaces = " \t\r\n";
    const auto first = text.find_first_not_of(spaces);
    if (first == std::string::npos) return std::nullopt;
    const auto last = text.find_last_not_of(spaces);
    return text.substr(first, last - first + 1);
}

std::optional<std::uint64_t> read_unsigned(const std::filesystem::path& path, int base = 10) {
    const auto token = read_token(path);
    if (!token) return std::nullopt;
    std::string_view digits(*token);
    if (base == 16 && digits.starts_with("0x")) digits.remove_prefix(2);
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) return std::nullopt;
    return value;
}

double bounded_value(const std::filesystem::path& path, double maximum, double scale = 1.0) {
    const auto raw = read_unsigned(path);
    if (!raw) return -1.0;
    const auto value = static_cast<double>(*raw) / scale;
    return value <= maximum ? value : -1.0;
}

bool matches(const std::filesystem::path& path, const PciIdentity& identity) {
    return read_unsigned(path / "vendor", 16) == identity.vendor &&
        read_unsigned(path / "device", 16) == identity.device;
}

bool complete_bdf(const PciIdentity& id) {
    return id.domain >= 0 && id.domain <= 0xffff && id.bus >= 0 && id.bus <= 0xff &&
        id.slot >= 0 && id.slot <= 0x1f && id.function >= 0 && id.function <= 7;
}

bool bdf_name(std::string_view name) {
    if (name.size() != 12 || name[4] != ':' || name[7] != ':' || name[10] != '.') return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (i == 4 || i == 7 || i == 10) continue;
        const char ch = name[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
    }
    unsigned slot = 0;
    const auto parsed = std::from_chars(name.data() + 8, name.data() + 10, slot, 16);
    return parsed.ec == std::errc{} && slot <= 0x1f && name[11] >= '0' && name[11] <= '7';
}

struct Channel {
    std::filesystem::path directory;
    std::string stem;
};

std::vector<Channel> channels(const std::filesystem::path& device, bool& truncated) {
    std::vector<Channel> result;
    std::error_code ec;
    std::size_t directories = 0;
    for (std::filesystem::directory_iterator it(device / "hwmon", ec), end;
            !ec && it != end; it.increment(ec)) {
        if (++directories > k_max_hwmon) { truncated = true; break; }
        std::set<std::string> stems;
        std::error_code inner_error;
        std::size_t visited = 0;
        for (std::filesystem::directory_iterator child(it->path(), inner_error), child_end;
                !inner_error && child != child_end; child.increment(inner_error)) {
            if (++visited > k_max_hwmon_entries) { truncated = true; break; }
            const auto name = child->path().filename().string();
            if (name.size() > 64) continue;
            const auto split = name.find('_');
            if (split == std::string::npos) continue;
            const auto suffix = std::string_view(name).substr(split);
            if (suffix != "_input" && suffix != "_average" && suffix != "_cap") continue;
            const auto stem = name.substr(0, split);
            for (const std::string_view kind : {"temp", "power", "energy", "freq", "fan"}) {
                if (!stem.starts_with(kind)) continue;
                const auto digits = std::string_view(stem).substr(kind.size());
                unsigned index = 0;
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
                if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() && index > 0 && index <= 64) stems.insert(stem);
            }
        }
        for (const auto& stem : stems) {
            if (result.size() == k_max_sensor_channels) { truncated = true; return result; }
            result.push_back({it->path(), stem});
        }
    }
    return result;
}

int temperature_priority(std::string_view label, bool memory, const Channel& channel, unsigned vendor) {
    if (memory) {
        if (label == "vram" || label == "mem" || label == "memory") return 100;
        return 0;
    }
    if (label == "pkg" || label == "gpu" || label == "edge") return 100;
    if (label == "junction") return 90;
    if (label.empty() && channel.stem == "temp1" && vendor == 0x1002) return 50;
    return 0;
}

int power_priority(std::string_view label, std::string_view stem) {
    if (label == "card") return 100;
    if (label == "gpu" || label == "ppt") return 90;
    if (label == "pkg" || label == "package") return 70;
    if (label.empty() && (stem == "power1" || stem == "energy1")) return 80;
    return 0;
}

struct Temperature {
    int priority = 0;
    double value = -1.0, high = -1.0, critical = -1.0;
};

void read_temperature(const Channel& channel, int priority, Temperature& destination) {
    if (priority <= destination.priority) return;
    const auto fault = read_unsigned(channel.directory / (channel.stem + "_fault"));
    if (fault && *fault != 0) return;
    const auto value = bounded_value(channel.directory / (channel.stem + "_input"), 200, 1000);
    if (value < 0) return;
    destination = {priority, value,
        bounded_value(channel.directory / (channel.stem + "_max"), 200, 1000),
        bounded_value(channel.directory / (channel.stem + "_crit"), 200, 1000)};
    if (destination.high <= 0) destination.high = -1;
    if (destination.critical <= 0) destination.critical = -1;
    if (destination.high >= destination.critical && destination.critical > 0) destination.high = -1;
}

std::string read_throttle(const std::filesystem::path& device) {
    const auto path = device / "tile0/gt0/freq0/throttle";
    const auto status = read_unsigned(path / "status");
    if (!status || *status > 1) return {};
    if (*status == 0) return "None";
    constexpr std::array reasons{
        std::pair{"reason_pl1", "power limit 1"}, std::pair{"reason_pl2", "power limit 2"},
        std::pair{"reason_pl4", "power limit 4"}, std::pair{"reason_thermal", "thermal"},
        std::pair{"reason_prochot", "PROCHOT"}, std::pair{"reason_ratl", "RATL"},
        std::pair{"reason_vr_thermalert", "VR thermal"}, std::pair{"reason_vr_tdc", "VR TDC"},
        std::pair{"reason_soc_thermal", "SoC thermal"}, std::pair{"reason_psys_pl1", "system power limit 1"},
        std::pair{"reason_psys_pl2", "system power limit 2"}};
    std::string result;
    for (const auto& [filename, label] : reasons) {
        if (read_unsigned(path / filename) == 1) {
            if (!result.empty()) result += ", ";
            result += label;
        }
    }
    return result.empty() ? "Reported throttling (reason unavailable)" : result;
}
} // namespace

GpuSensorDiscovery discover_gpu_sensors(const PciIdentity& identity, const std::filesystem::path& pci_root) {
    GpuSensorDiscovery result{identity, {}, {}, {}};
    if (identity.vendor == 0 || identity.vendor > 0xffff || identity.device == 0 || identity.device > 0xffff) {
        result.reason = "Active GPU vendor/device identity unavailable";
        return result;
    }
    if (complete_bdf(identity)) {
        result.pci_address = std::format("{:04x}:{:02x}:{:02x}.{:x}", identity.domain, identity.bus, identity.slot, identity.function);
        const auto directory = pci_root / result.pci_address;
        if (matches(directory, identity)) result.device_directory = directory;
        else result.reason = "Active PCI identity differs from sysfs or is unreadable";
    } else if (identity.domain == -1 && identity.bus == -1 && identity.slot == -1 && identity.function == -1) {
        std::error_code ec;
        std::size_t count = 0;
        for (std::filesystem::directory_iterator it(pci_root, ec), end; !ec && it != end; it.increment(ec)) {
            if (++count > k_max_pci_entries) {
                result.device_directory.clear();
                result.reason = "GPU PCI discovery exceeded its bounded inventory";
                return result;
            }
            if (!bdf_name(it->path().filename().string()) || !matches(it->path(), identity)) continue;
            if (!result.device_directory.empty()) {
                result.device_directory.clear();
                result.reason = "Multiple PCI adapters match the active GPU; exact PCI identity required";
                return result;
            }
            result.device_directory = it->path();
            result.pci_address = it->path().filename().string();
        }
        if (ec) result.device_directory.clear();
        if (result.device_directory.empty()) result.reason = "No uniquely matching active GPU PCI sensor source";
    } else {
        result.reason = "Active GPU PCI identity is partial or invalid";
    }
    if (!result.device_directory.empty()) result.reason = "Read-only selected-GPU sysfs sensors at 1 Hz; missing fields are unavailable";
    return result;
}

GpuSensorReading read_gpu_sensors(const GpuSensorDiscovery& source, GpuSensorEnergyState* energy, std::uint64_t sample_ns) {
    GpuSensorReading result;
    result.sample_monotonic_ns = sample_ns != 0 ? sample_ns : steady_ns();
    result.reason = source.reason;
    if (source.device_directory.empty() || !matches(source.device_directory, source.identity)) {
        if (!source.device_directory.empty() || result.reason.empty()) {
            result.reason = "Selected GPU sensors unavailable or PCI identity changed";
        }
        if (energy) *energy = {};
        return result;
    }
    const auto& device = source.device_directory;
    result.utilization_percent = bounded_value(device / "gpu_busy_percent", 100);
    result.memory_utilization_percent = bounded_value(device / "mem_busy_percent", 100);
    constexpr double k_mib = 1024.0 * 1024.0;
    result.device_vram_used_mb = bounded_value(device / "mem_info_vram_used", 1024.0 * 1024.0, k_mib);
    result.device_vram_total_mb = bounded_value(device / "mem_info_vram_total", 1024.0 * 1024.0, k_mib);
    Temperature gpu, vram;
    bool truncated = false;
    int power_score = 0, limit_score = 0, energy_score = 0;
    std::filesystem::path energy_path;
    std::optional<std::uint64_t> energy_value;
    for (const auto& channel : channels(device, truncated)) {
        const auto label = read_token(channel.directory / (channel.stem + "_label")).value_or("");
        if (channel.stem.starts_with("temp")) {
            read_temperature(channel, temperature_priority(label, false, channel, source.identity.vendor), gpu);
            read_temperature(channel, temperature_priority(label, true, channel, source.identity.vendor), vram);
        } else if (channel.stem.starts_with("power")) {
            const int score = power_priority(label, channel.stem);
            double watts = bounded_value(channel.directory / (channel.stem + "_average"), 3000, 1'000'000);
            if (watts < 0) watts = bounded_value(channel.directory / (channel.stem + "_input"), 3000, 1'000'000);
            if (score > power_score && watts >= 0) { result.power_w = watts; power_score = score; }
            const auto limit = bounded_value(channel.directory / (channel.stem + "_cap"), 3000, 1'000'000);
            if (score > limit_score && limit > 0) { result.power_limit_w = limit; limit_score = score; }
        } else if (channel.stem.starts_with("energy")) {
            const int score = power_priority(label, channel.stem);
            const auto value = read_unsigned(channel.directory / (channel.stem + "_input"));
            if (score > energy_score && value) {
                energy_path = channel.directory / (channel.stem + "_input");
                energy_value = value;
                energy_score = score;
            }
        } else if (channel.stem.starts_with("fan") && result.fan_rpm < 0) {
            result.fan_rpm = bounded_value(channel.directory / (channel.stem + "_input"), 100'000);
        } else if (channel.stem.starts_with("freq")) {
            const auto mhz = bounded_value(channel.directory / (channel.stem + "_input"), 100'000, 1'000'000);
            if (label == "mem" || label == "memory" || label == "vram" ||
                    (label.empty() && source.identity.vendor == 0x1002 && channel.stem == "freq2")) result.memory_clock_mhz = mhz;
            if (label == "gpu" || label == "gfx" || label == "core" ||
                    (label.empty() && source.identity.vendor == 0x1002 && channel.stem == "freq1")) result.core_clock_mhz = mhz;
        }
    }
    result.temperature_c = gpu.value;
    result.temperature_high_c = gpu.high;
    result.temperature_critical_c = gpu.critical;
    result.vram_temperature_c = vram.value;
    result.vram_temperature_high_c = vram.high;
    result.vram_temperature_critical_c = vram.critical;
    if (source.identity.vendor == 0x8086) {
        result.core_clock_mhz = bounded_value(device / "tile0/gt0/freq0/act_freq", 100'000);
        result.throttle_reason = read_throttle(device);
    }
    if (energy) {
        if (energy_value) {
            const auto previous = *energy;
            *energy = {energy_path, *energy_value, result.sample_monotonic_ns, true};
            if (result.power_w < 0 && previous.initialized && previous.source == energy_path &&
                    *energy_value >= previous.microjoules && result.sample_monotonic_ns > previous.sample_ns) {
                const double seconds = static_cast<double>(result.sample_monotonic_ns - previous.sample_ns) / 1e9;
                if (seconds >= 0.1 && seconds <= 5.0) {
                    const double watts = static_cast<double>(*energy_value - previous.microjoules) / 1e6 / seconds;
                    if (watts <= 3000) {
                        result.power_w = watts;
                        result.reason += "; power derived from selected GPU energy counter over a host interval";
                    }
                }
            }
        } else {
            *energy = {};
        }
    }
    if (truncated) result.reason += "; bounded sensor inventory truncated";
    result.available = result.temperature_c >= 0 || result.vram_temperature_c >= 0 ||
        result.power_w >= 0 || result.power_limit_w >= 0 || result.fan_rpm >= 0 || result.core_clock_mhz >= 0 ||
        result.memory_clock_mhz >= 0 || result.utilization_percent >= 0 || result.memory_utilization_percent >= 0 ||
        result.device_vram_used_mb >= 0 || result.device_vram_total_mb >= 0 ||
        !result.throttle_reason.empty();
    if (!result.available) result.reason += "; no supported readable sensor fields";
    return result;
}

struct GpuSensorTelemetry::Impl {
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    GpuSensorReading reading;
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
    std::jthread worker;
#endif

    Impl(PciIdentity identity, std::filesystem::path root) {
        reading.reason = "Waiting for sensor sample";
#if defined(__linux__) && !defined(__ANDROID__) && defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
        worker = std::jthread([this, identity, root = std::move(root)](const std::stop_token& stop) {
            const auto discovery = discover_gpu_sensors(identity, root);
            GpuSensorEnergyState energy;
            while (!stop.stop_requested()) {
                auto next = read_gpu_sensors(discovery, &energy);
                {
                    std::scoped_lock lock(mutex);
                    reading = std::move(next);
                }
                std::unique_lock lock(mutex);
                wake.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
            }
        });
#else
        static_cast<void>(identity);
        static_cast<void>(root);
        reading.reason = "Read-only GPU sysfs sensors are unavailable on this platform or standard library";
#endif
    }
};

GpuSensorTelemetry::GpuSensorTelemetry(PciIdentity identity, std::filesystem::path pci_root)
    : impl_(std::make_unique<Impl>(identity, std::move(pci_root))) {}
GpuSensorTelemetry::~GpuSensorTelemetry() = default;
GpuSensorReading GpuSensorTelemetry::latest() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->reading;
}
} // namespace heaven::profile
