/*
 * HeavenBench — System_Info / Settings_Info formatter (pure)
 *
 * The formatters, truncation, range checks, and per-datum placeholder
 * substitution below are PURE (standard library only). The impure host query
 * bodies for LinuxHostInfoSource (uname(2) / sysconf / proc/cpuinfo) are
 * implemented separately; the pure formatters do not depend on them.
 */
#include "benchmark/run_info.h"

#include <cmath>

// The LinuxHostInfoSource query bodies below are the only impure code in this
// translation unit: they read the host OS. They are guarded for the Linux build
// target (uname(2), sysconf, proc/cpuinfo) or macOS (uname(2), sysctl). Other
// platforms return the undetermined sentinels for the pure formatter.
#ifdef __linux__
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#elifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <array>
#include <cstdint>
#endif

namespace heaven {

// ── Pure field helper ───────────────────────────────────────────

std::string clean_text_field(std::string value, const char* placeholder) {
    // Truncate to at most kMaxFieldChars.
    if (value.size() > kMaxFieldChars) {
        value.resize(kMaxFieldChars);
    }

    // An empty or whitespace-only value is treated as undetermined and yields
    // the placeholder, so the field is never blank.
    if (value.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
        return placeholder;
    }
    return value;
}

// ── Pure description assemblers ─────────────────────────────────

std::string format_platform_description(const PlatformRaw& raw) {
    // "<os> <kernel> <arch>" — each text sub-field falls back to the platform
    // placeholder when empty, so the line is never blank.
    const std::string os     = clean_text_field(raw.os_name, kUnknownPlatform);
    const std::string kernel = clean_text_field(raw.kernel_version, kUnknownPlatform);
    const std::string arch   = clean_text_field(raw.machine_arch, kUnknownPlatform);
    return os + " " + kernel + " " + arch;
}

std::string format_cpu_description(const CpuRaw& raw) {
    // "<model> (<mhz>MHz) x<cores>". A negative clock or a core count below 1
    // is undetermined and substitutes the CPU placeholder for that group; the
    // clock is reported as a whole number of MHz.
    const std::string model = clean_text_field(raw.model_name, kUnknownCpu);
    const std::string clock = (raw.clock_mhz >= 0.0)
        ? "(" + std::to_string(std::llround(raw.clock_mhz)) + "MHz)"
        : std::string(kUnknownCpu);
    const std::string cores = (raw.logical_cores >= 1)
        ? "x" + std::to_string(raw.logical_cores)
        : std::string(kUnknownCpu);
    return model + " " + clock + " " + cores;
}

std::string format_gpu_description(const GpuRaw& raw) {
    // Describe only the graphics device selected by the active backend. The
    // number of other physical devices enumerated by Vulkan is deliberately
    // absent: it does not describe the GPU used for this benchmark.
    const std::string model = clean_text_field(raw.model_name, kUnknownGpu);
    const std::string vram  = (raw.vram_mb >= 0)
        ? "(" + std::to_string(raw.vram_mb) + "MB)"
        : std::string(kUnknownGpu);
    return model + " " + vram;
}

std::string format_display_mode(const DisplayRaw& raw) {
    // "<w>x<h> <aa> <windowed|fullscreen>".

    // Resolution: both dimensions must be positive integers, else the group is
    // undetermined and substitutes the display placeholder.
    const std::string resolution = (raw.width >= 1 && raw.height >= 1)
        ? std::to_string(raw.width) + "x" + std::to_string(raw.height)
        : std::string(kUnknownDisplay);

    // Anti-aliasing: a sample count of 1 denotes no AA; [2, 64] is "<n>xAA";
    // anything outside [1, 64] is out of range and substitutes the placeholder.
    std::string aa;
    if (raw.aa_samples == 1) {
        aa = "no AA";
    } else if (raw.aa_samples >= 2 && raw.aa_samples <= 64) {
        aa = std::to_string(raw.aa_samples) + "xAA";
    } else {
        aa = kUnknownDisplay;
    }

    // Window state is always determinable from the boolean flag.
    const std::string window = raw.fullscreen ? "fullscreen" : "windowed";

    return resolution + " " + aa + " " + window;
}

std::string format_render_api(const std::string& api_name) {
    // An empty API name is undetermined and substitutes the placeholder

    return clean_text_field(api_name, kUnknownRenderApi);
}

std::string format_quality_preset(const std::string& label) {
    // An empty preset label is undetermined and substitutes the placeholder

    return clean_text_field(label, kUnknownPreset);
}

// ── Pure composition helpers ────────────────────────────────────

SystemInfo gather_system_info(IHostInfoSource& host, const GpuRaw& gpu) {
    const PlatformRaw platform = host.query_platform();
    const CpuRaw      cpu       = host.query_cpu();

    SystemInfo info;
    info.platform = format_platform_description(platform);
    info.cpu      = format_cpu_description(cpu);
    info.gpu      = format_gpu_description(gpu);
    info.platform_raw = platform;
    info.cpu_raw      = cpu;
    info.gpu_raw      = gpu;
    return info;
}

SettingsInfo gather_settings_info(const std::string& render_api_name,
                                  const DisplayRaw&  display,
                                  const std::string& quality_preset_label) {
    SettingsInfo info;
    info.render_api     = format_render_api(render_api_name);
    info.display_mode   = format_display_mode(display);
    info.quality_preset = format_quality_preset(quality_preset_label);
    info.display_raw    = display;
    return info;
}

// ── Impure host source (LinuxHostInfoSource) ────────────────────
// These query bodies read the host OS and so are deliberately separated from
// the pure formatters above. Each leaves any datum it cannot determine at its
// undetermined sentinel (empty string / clock_mhz = -1.0 / logical_cores = 0)
// so the pure formatter substitutes the placeholder.

#ifdef __linux__

PlatformRaw LinuxHostInfoSource::query_platform() {
    // uname(2) supplies the operating system name (sysname), the kernel version
    // (release), and the machine architecture (machine). On failure each field
    // stays empty and the formatter falls back to the platform placeholder.
    PlatformRaw raw;
    struct utsname info{};
    if (::uname(&info) == 0) {
        raw.os_name        = info.sysname;
        raw.kernel_version = info.release;
        raw.machine_arch   = info.machine;
    }
    return raw;
}

CpuRaw LinuxHostInfoSource::query_cpu() {
    CpuRaw raw;

    // Logical core count from sysconf; a non-positive result leaves the
    // undetermined sentinel (0) in place.
    const long cores = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) {
        raw.logical_cores = static_cast<int>(cores);
    }

    // Parse /proc/cpuinfo for the processor model name ("model name") and clock
    // rate ("cpu MHz"). If the file cannot be opened, model_name stays empty and
    // clock_mhz stays at its undetermined sentinel (-1.0).
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::string line;
        while (std::getline(cpuinfo, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }

            // Trim trailing whitespace from the key and leading whitespace from
            // the value around the colon separator.
            std::string key = line.substr(0, colon);
            const auto key_end = key.find_last_not_of(" \t");
            key = (key_end == std::string::npos) ? std::string() : key.substr(0, key_end + 1);

            std::string value = line.substr(colon + 1);
            const auto value_begin = value.find_first_not_of(" \t");
            value = (value_begin == std::string::npos) ? std::string() : value.substr(value_begin);

            // Take the first processor's model name.
            if (raw.model_name.empty() && key == "model name") {
                raw.model_name = value;
            }

            // Take the first processor's clock rate; parse defensively so a
            // malformed value leaves the undetermined sentinel.
            if (raw.clock_mhz < 0.0 && key == "cpu MHz") {
                try {
                    const double mhz = std::stod(value);
                    if (mhz >= 0.0) {
                        raw.clock_mhz = mhz;
                    }
                } catch (...) {
                    raw.clock_mhz = -1.0;  // Undetermined on parse failure.
                }
            }

            // Stop once both fields are populated.
            if (!raw.model_name.empty() && raw.clock_mhz >= 0.0) {
                break;
            }
        }
    }

    return raw;
}

#elifdef __APPLE__

namespace {

std::string macos_host_string(const char* name) {
    std::array<char, kMaxFieldChars + 1> text{};
    size_t size = text.size();
    if (::sysctlbyname(name, text.data(), &size, nullptr, 0) != 0) return {};
    text.back() = '\0';
    return text.data();
}

} // namespace

PlatformRaw LinuxHostInfoSource::query_platform() {
    PlatformRaw raw;
    struct utsname info{};
    if (::uname(&info) == 0) {
        raw.os_name = "macOS";
        raw.kernel_version = info.release;
        raw.machine_arch = info.machine;
    }
    const auto product_version = macos_host_string("kern.osproductversion");
    if (!product_version.empty()) {
        raw.os_name = "macOS " + product_version + " (Darwin)";
    }
    return raw;
}

CpuRaw LinuxHostInfoSource::query_cpu() {
    CpuRaw raw;
    raw.model_name = macos_host_string("machdep.cpu.brand_string");
    int cores = 0;
    size_t size = sizeof(cores);
    if (::sysctlbyname("hw.logicalcpu", &cores, &size, nullptr, 0) == 0 && cores > 0) {
        raw.logical_cores = cores;
    }

    // Apple Silicon may omit the clock sysctl. Leave it unavailable instead of
    // presenting a guessed or nominal frequency as a measured CPU clock.
    std::uint64_t frequency = 0;
    size = sizeof(frequency);
    if (::sysctlbyname("hw.cpufrequency", &frequency, &size, nullptr, 0) == 0 && frequency > 0) {
        raw.clock_mhz = static_cast<double>(frequency) / 1'000'000.0;
    }
    return raw;
}

#else

// Other platforms have no host queries here, so report the undetermined
// sentinels and let the pure formatter substitute the placeholder. This keeps
// the symbol defined for the linker on every target.
PlatformRaw LinuxHostInfoSource::query_platform() {
    return PlatformRaw{};
}

CpuRaw LinuxHostInfoSource::query_cpu() {
    return CpuRaw{};
}

#endif

} // namespace heaven
