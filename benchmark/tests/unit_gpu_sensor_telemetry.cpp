#include "benchmark/gpu_sensor_telemetry.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}
namespace {
using heaven::profile::GpuSensorDiscovery;
using heaven::profile::GpuSensorEnergyState;
using heaven::profile::PciIdentity;
using heaven::profile::discover_gpu_sensors;
using heaven::profile::read_gpu_sensors;

bool check(bool passed, const char* message) {
    if (!passed) std::printf("[unit_gpu_sensor_telemetry] %s\n", message);
    return passed;
}

bool near(double value, double expected) { return std::abs(value - expected) < 0.00001; }

void write(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << value << '\n';
}

struct Fixture {
    std::filesystem::path root;
    Fixture() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        static unsigned sequence = 0;
        root = std::filesystem::temp_directory_path() /
            ("heaven-gpu-sensors-" + std::to_string(now) + "-" + std::to_string(++sequence));
        std::filesystem::create_directories(root);
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    std::filesystem::path adapter(std::string_view name = "adapter", bool amd = false) const {
        const auto path = root / name;
        write(path / "vendor", amd ? "0x1002" : "0x8086");
        write(path / "device", amd ? "0x744c" : "0xe223");
        return path;
    }
};

bool gpu_sensor_identity_test() {
    Fixture fixture;
    const PciIdentity exact{0x8086, 0xe223, 0, 3, 0, 0};
    const auto unknown = discover_gpu_sensors({}, fixture.root);
    if (!check(unknown.device_directory.empty(), "unknown identity accepted")) return false;
    if (!check(!unknown.reason.empty() && read_gpu_sensors(unknown).reason == unknown.reason,
            "missing-identity discovery reason lost in reading")) return false;
#ifndef _WIN32
    const auto first = fixture.adapter("0000:03:00.0");
    fixture.adapter("0000:04:00.0", true);
    if (!check(discover_gpu_sensors(exact, fixture.root).device_directory == first, "exact selected adapter not found")) return false;
    PciIdentity fallback{0x8086, 0xe223};
    if (!check(discover_gpu_sensors(fallback, fixture.root).device_directory == first, "unique vendor/device fallback failed")) return false;
    fixture.adapter("0000:05:00.0");
    const auto ambiguous = discover_gpu_sensors(fallback, fixture.root);
    if (!check(ambiguous.device_directory.empty(), "ambiguous duplicate adapter selected")) return false;
    if (!check(!read_gpu_sensors(ambiguous).available && read_gpu_sensors(ambiguous).reason == ambiguous.reason,
            "ambiguous-adapter discovery reason lost in reading")) return false;
    if (!check(discover_gpu_sensors(exact, fixture.root).device_directory == first, "exact BDF became ambiguous")) return false;
    auto wrong = exact;
    wrong.bus = 4;
    if (!check(discover_gpu_sensors(wrong, fixture.root).device_directory.empty(), "different adapter substituted for exact mismatch")) return false;
    wrong = exact;
    wrong.function = -1;
    if (!check(discover_gpu_sensors(wrong, fixture.root).device_directory.empty(), "partial BDF accepted")) return false;
    wrong = exact;
    wrong.slot = 32;
    if (!check(discover_gpu_sensors(wrong, fixture.root).device_directory.empty(), "invalid slot accepted")) return false;
    const GpuSensorDiscovery saved{exact, first, "0000:03:00.0", "fixture"};
    write(first / "vendor", "0x1002");
    const auto changed = read_gpu_sensors(saved);
    return check(!changed.available && changed.temperature_c < 0, "changed adapter identity retained measurements");
#else
    return check(discover_gpu_sensors(exact, fixture.root).device_directory.empty(), "missing Linux BDF path invented source");
#endif
}

bool gpu_sensor_units_labels_test() {
    Fixture fixture;
    const auto adapter = fixture.adapter();
    const auto hwmon = adapter / "hwmon/hwmon7";
    const GpuSensorDiscovery source{{0x8086, 0xe223}, adapter, "fixture", "fixture"};
    write(hwmon / "temp1_input", "99000"); // Intel temp1 must not be assumed to be GPU.
    write(hwmon / "temp1_label", "unrelated");
    write(hwmon / "temp2_input", "42250");
    write(hwmon / "temp2_label", "pkg");
    write(hwmon / "temp2_max", "60000");
    write(hwmon / "temp2_crit", "100000");
    write(hwmon / "temp3_input", "53750");
    write(hwmon / "temp3_label", "vram");
    write(hwmon / "temp3_max", "95000");
    write(hwmon / "temp3_crit", "105000");
    write(hwmon / "fan1_input", "0");
    write(hwmon / "power1_cap", "190000000");
    write(hwmon / "power1_label", "pkg");
    write(adapter / "tile0/gt0/freq0/act_freq", "2300");
    write(adapter / "tile0/gt1/freq0/act_freq", "1600"); // Media GT is not VRAM clock.
    write(adapter / "tile0/gt0/freq0/throttle/status", "0");
    write(adapter / "gpu_busy_percent", "99");
    write(adapter / "mem_busy_percent", "17");
    write(adapter / "mem_info_vram_used", "1073741824");
    write(adapter / "mem_info_vram_total", "8589934592");
    const auto reading = read_gpu_sensors(source, nullptr, 1'000'000'000);
    if (!check(reading.available && near(reading.temperature_c, 42.25) && near(reading.vram_temperature_c, 53.75) &&
            reading.temperature_high_c == 60 && reading.temperature_critical_c == 100 &&
            reading.vram_temperature_high_c == 95 && reading.vram_temperature_critical_c == 105,
            "labelled Intel temperatures, units or driver advisory metadata changed")) return false;
    if (!check(reading.power_w < 0 && reading.power_limit_w == 190 && reading.fan_rpm == 0 &&
            reading.core_clock_mhz == 2300 && reading.memory_clock_mhz < 0 && reading.throttle_reason == "None",
            "power cap became power draw, idle fan lost, media GT became VRAM clock or throttle state invented")) return false;
    if (!check(reading.device_vram_used_mb == 1024 && reading.device_vram_total_mb == 8192 &&
            reading.utilization_percent == 99 && reading.memory_utilization_percent == 17 &&
            reading.sample_monotonic_ns == 1'000'000'000, "VRAM/utilization units or injected sample time changed")) return false;
    write(hwmon / "power1_average", "123450000");
    write(hwmon / "power1_input", "170000000");
    write(adapter / "tile0/gt0/freq0/throttle/status", "1");
    write(adapter / "tile0/gt0/freq0/throttle/reason_thermal", "1");
    write(adapter / "tile0/gt0/freq0/throttle/reason_pl1", "1");
    const auto active = read_gpu_sensors(source);
    if (!check(near(active.power_w, 123.45) && active.throttle_reason == "power limit 1, thermal", "average power preference or explicit throttle reasons failed")) return false;
    std::filesystem::remove_all(adapter / "hwmon");
    std::filesystem::remove_all(adapter / "tile0");
    for (const auto* name : {"gpu_busy_percent", "mem_busy_percent", "mem_info_vram_used", "mem_info_vram_total"}) std::filesystem::remove(adapter / name);
    const auto removed = read_gpu_sensors(source);
    return check(!removed.available && removed.temperature_c < 0 && removed.power_w < 0 && removed.fan_rpm < 0 &&
        removed.core_clock_mhz < 0 && removed.throttle_reason.empty(), "missing sensors retained stale valid values");
}

bool gpu_sensor_amd_malformed_test() {
    Fixture fixture;
    const auto adapter = fixture.adapter("adapter", true);
    const auto hwmon = adapter / "hwmon/hwmon2";
    const GpuSensorDiscovery source{{0x1002, 0x744c}, adapter, "fixture", "fixture"};
    write(hwmon / "temp1_input", "48000");
    write(hwmon / "temp1_label", "edge");
    write(hwmon / "temp2_input", "67000");
    write(hwmon / "temp2_label", "junction");
    write(hwmon / "temp3_input", "56000");
    write(hwmon / "temp3_label", "mem");
    write(hwmon / "freq1_input", "2600000000");
    write(hwmon / "freq2_input", "1200000000");
    write(hwmon / "power1_input", "180000000");
    const auto reading = read_gpu_sensors(source);
    if (!check(reading.temperature_c == 48 && reading.vram_temperature_c == 56 && reading.core_clock_mhz == 2600 &&
            reading.memory_clock_mhz == 1200 && reading.power_w == 180 && reading.throttle_reason.empty(),
            "AMD edge/junction/memory labels or Hz units failed")) return false;
    write(hwmon / "temp1_fault", "1");
    write(hwmon / "temp2_input", "-2000");
    write(hwmon / "temp3_input", "56000 garbage");
    write(hwmon / "freq1_input", "18446744073709551616");
    write(hwmon / "freq2_input", "not a value");
    write(hwmon / "power1_input", "3000000001");
    write(adapter / "gpu_busy_percent", "101");
    const auto malformed = read_gpu_sensors(source);
    return check(!malformed.available && malformed.temperature_c < 0 && malformed.vram_temperature_c < 0 &&
        malformed.core_clock_mhz < 0 && malformed.memory_clock_mhz < 0 && malformed.power_w < 0 &&
        malformed.utilization_percent < 0, "faulted, malformed, overflow or unreasonable readings accepted");
}

bool gpu_sensor_energy_test() {
    Fixture fixture;
    const auto adapter = fixture.adapter();
    const auto hwmon = adapter / "hwmon/hwmon0";
    const auto path = hwmon / "energy1_input";
    const GpuSensorDiscovery source{{0x8086, 0xe223}, adapter, "fixture", "fixture"};
    write(path, "1000000000");
    write(hwmon / "energy1_label", "card");
    write(hwmon / "energy2_input", "500000000");
    write(hwmon / "energy2_label", "pkg");
    write(hwmon / "power1_cap", "190000000");
    GpuSensorEnergyState energy;
    if (!check(read_gpu_sensors(source, &energy, 1'000'000'000).power_w < 0, "first energy counter sample invented draw")) return false;
    write(path, "1100000000");
    const auto derived = read_gpu_sensors(source, &energy, 2'000'000'000);
    if (!check(derived.power_w == 100 && derived.power_limit_w == 190 && energy.source == path,
            "card energy selection or microjoule/host-time conversion failed")) return false;
    write(path, "1250000000");
    if (!check(read_gpu_sensors(source, &energy, 2'050'000'000).power_w < 0, "too-short energy interval accepted")) return false;
    write(path, "1300000000");
    if (!check(read_gpu_sensors(source, &energy, 9'000'000'000).power_w < 0, "suspend-sized energy interval accepted")) return false;
    write(path, "1000000");
    if (!check(read_gpu_sensors(source, &energy, 10'000'000'000).power_w < 0, "counter reset/wrap invented huge power")) return false;
    write(path, "101000000");
    if (!check(read_gpu_sensors(source, &energy, 11'000'000'000).power_w == 100, "energy did not recover after reset")) return false;
    write(path, "201000000");
    if (!check(read_gpu_sensors(source, &energy, 10'000'000'000).power_w < 0, "backwards host timestamp accepted")) return false;
    write(hwmon / "power1_input", "45000000");
    write(path, "301000000");
    if (!check(read_gpu_sensors(source, &energy, 11'000'000'000).power_w == 45, "energy overrode direct measured power")) return false;
    std::filesystem::remove(path);
    std::filesystem::remove(hwmon / "energy2_input");
    static_cast<void>(read_gpu_sensors(source, &energy, 12'000'000'000));
    return check(!energy.initialized, "missing energy source retained previous valid baseline");
}

bool gpu_sensor_inventory_test() {
    Fixture fixture;
    const auto adapter = fixture.adapter();
    const auto hwmon = adapter / "hwmon/hwmon0";
    for (unsigned index = 1; index <= 64; ++index) {
        const auto stem = "temp" + std::to_string(index);
        write(hwmon / (stem + "_input"), "42000");
        write(hwmon / (stem + "_label"), "gpu");
    }
    write(hwmon / "fan1_input", "1000");
    const auto reading = read_gpu_sensors({{0x8086, 0xe223}, adapter, "fixture", "fixture"});
    return check(reading.available && reading.reason.find("bounded sensor inventory truncated") != std::string::npos,
        "bounded sensor overflow was not disclosed");
}

const heaven::testing::Register identity_registered{gpu_sensor_identity_test};
const heaven::testing::Register labels_registered{gpu_sensor_units_labels_test};
const heaven::testing::Register malformed_registered{gpu_sensor_amd_malformed_test};
const heaven::testing::Register energy_registered{gpu_sensor_energy_test};
const heaven::testing::Register inventory_registered{gpu_sensor_inventory_test};
}
