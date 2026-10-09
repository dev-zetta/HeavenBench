#include "benchmark/driver_check.h"

#include <array>
#include <charconv>
#include <optional>
#include <system_error>
#include <utility>

namespace heaven {
namespace {

struct Version {
    std::array<unsigned int, 4> parts{};
    std::size_t count = 0;
    bool prerelease = false;
};

// Accept only a complete normalized version supplied by the active backend.
// Never search arbitrary driver/API strings for numbers: OpenGL 4.6 and an AMD
// internal Vulkan version must not become invented vendor package versions.
std::optional<Version> parse_version(std::string_view text, bool mesa_suffix) {
    Version result;
    while (!text.empty() && result.count < result.parts.size()) {
        const auto [end, error] = std::from_chars(
            text.data(), text.data() + text.size(), result.parts[result.count]);
        if (error != std::errc{} || end == text.data()) return std::nullopt;
        ++result.count;
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        if (text.empty()) break;
        if (text.front() != '.') break;
        text.remove_prefix(1);
        if (text.empty()) return std::nullopt;
    }
    if (result.count < 2 || result.parts[0] == 0) return std::nullopt;
    if (!text.empty()) {
        if (!mesa_suffix || (text.front() != '-' && text.front() != '+') ||
            text.size() == 1) return std::nullopt;
        result.prerelease = text.starts_with("-devel") || text.starts_with("-rc");
        for (const char character : text.substr(1)) {
            const bool letter = (character >= 'a' && character <= 'z') ||
                                (character >= 'A' && character <= 'Z');
            const bool digit = character >= '0' && character <= '9';
            if (!letter && !digit && character != '.' && character != '-' &&
                character != '+' && character != '~') return std::nullopt;
        }
    }
    return result;
}

DriverCheck baseline_check(const Version& version,
                           const std::array<unsigned int, 4>& baseline,
                           std::string_view label, std::string recommendation,
                           std::string url) {
    const bool older = version.parts < baseline ||
                       (version.parts == baseline && version.prerelease);
    const std::string summary = older
        ? "The active driver predates HeavenBench's recommended baseline (" +
              std::string(label) + ")."
        : "The active driver meets HeavenBench's recommended baseline (" +
              std::string(label) + ").";
    if (older) recommendation += " Older drivers may have rendering or performance issues.";
    recommendation += " This is a project recommendation, not a required or latest driver version. "
                      "Legacy hardware may need an older supported branch.";
    return {older ? DriverCheckSeverity::Warning : DriverCheckSeverity::Information,
            summary, std::move(recommendation), std::move(url)};
}

DriverCheck unverified(std::string recommendation, std::string url) {
    return {DriverCheckSeverity::Information,
            "The active driver version could not be compared with a project baseline.",
            std::move(recommendation), std::move(url)};
}

} // namespace

DriverCheck check_graphics_driver(const DriverCheckInput& input) {
    const bool windows = input.platform == DriverPlatform::Windows;
    if (input.platform == DriverPlatform::Other) {
        return unverified("Use your device manufacturer's supported system and graphics updates.", "");
    }

    switch (input.family) {
    case DriverFamily::Software:
        return {DriverCheckSeverity::Warning,
                "Software rendering is active; this run measures CPU rendering instead of GPU performance.",
                windows
                    ? "Select a hardware GPU and install its latest compatible graphics driver through "
                      "Windows Update, your computer manufacturer, or the GPU vendor. Restart HeavenBench afterward."
                    : "Select a hardware GPU and install or update its supported graphics driver using your "
                      "distribution's driver tools. Check GPU access in remote or virtual sessions, then restart HeavenBench.",
                windows
                    ? "https://support.microsoft.com/en-us/windows/hardware/display-graphics/microsoft-basic-display-adapter-in-windows"
                    : "https://docs.mesa3d.org/drivers/llvmpipe.html"};
    case DriverFamily::Nouveau:
        return {DriverCheckSeverity::Warning,
                input.api_name.starts_with("OpenGL")
                    ? "The legacy Nouveau OpenGL driver is active; performance and feature support depend on your GPU and kernel."
                    : "The Nouveau driver is active; performance and feature support depend on your GPU and kernel.",
                "Update Mesa and the kernel using your distribution's driver tools. Check whether your GPU supports "
                "NVK (or NVK with Zink for OpenGL) or NVIDIA's driver. Choose a supported driver for your hardware.",
                "https://docs.mesa3d.org/drivers/nvk.html"};
    case DriverFamily::Mesa:
    case DriverFamily::MesaNvk: {
        const bool nvk = input.family == DriverFamily::MesaNvk;
        const std::string recommendation = windows
            ? "Use the latest compatible Mesa package supplied by your graphics environment."
            : "Update Mesa and the kernel through your distribution's supported packages.";
        const std::string url = nvk ? "https://docs.mesa3d.org/drivers/nvk.html"
                                    : "https://docs.mesa3d.org/faq.html";
        const auto version = parse_version(input.driver_version, true);
        if (!version || version->count != 3) return unverified(recommendation, url);
        return baseline_check(*version, nvk ? std::array<unsigned int, 4>{25, 1, 0, 0}
                                            : std::array<unsigned int, 4>{24, 0, 0, 0},
                              nvk ? "Mesa NVK 25.1.0" : "Mesa 24.0.0", recommendation, url);
    }
    case DriverFamily::Nvidia: {
        const std::string recommendation = windows
            ? "Check your computer manufacturer's updates first, or use NVIDIA's driver selector for "
              "the latest compatible driver for your GPU and Windows version."
            : "Use your distribution's driver tools to install the latest NVIDIA driver compatible "
              "with your GPU and kernel. Prefer distribution packages over an unmanaged installer.";
        const std::string url = "https://www.nvidia.com/en-us/drivers/";
        const auto version = parse_version(input.driver_version, false);
        if (!version || version->parts[0] < 100) return unverified(recommendation, url);
        return baseline_check(*version, windows ? std::array<unsigned int, 4>{551, 61, 0, 0}
                                                : std::array<unsigned int, 4>{550, 54, 14, 0},
                              windows ? "NVIDIA 551.61" : "NVIDIA 550.54.14", recommendation, url);
    }
    case DriverFamily::Intel: {
        const std::string recommendation = windows
            ? "Check your computer manufacturer's supported graphics updates first. Intel's Driver "
              "& Support Assistant can identify compatible generic updates; preserve required OEM customizations."
            : "Use your distribution's supported Intel graphics, Mesa and kernel updates.";
        const std::string url = "https://www.intel.com/content/www/us/en/support/detect.html";
        auto version = parse_version(input.driver_version, false);
        if (!windows || !version || (version->count != 2 && version->count != 4)) {
            return unverified(recommendation, url);
        }
        if (version->count == 4) {
            // Intel's first two Windows fields describe WDDM; compare the
            // final two build fields, also exposed by its Vulkan encoding.
            version->parts = {version->parts[2], version->parts[3], 0, 0};
        }
        if (version->parts[0] < 100) return unverified(recommendation, url);
        return baseline_check(*version, {101, 5186, 0, 0}, "Intel Windows build 101.5186",
                              recommendation, url);
    }
    case DriverFamily::Amd:
        return unverified(windows
            ? "The exposed AMD driver number is not reliably comparable with an Adrenalin package version. "
              "Check your computer manufacturer's updates or AMD's selector for the latest compatible Windows driver."
            : "The exposed AMD driver number is not reliably comparable with a public package version. "
              "Use your distribution's supported graphics updates; Mesa RADV is also a supported AMD driver.",
            "https://www.amd.com/en/support/download/drivers.html");
    case DriverFamily::Unknown:
        return unverified(windows
            ? "Check the active GPU in Windows graphics settings, then use Windows Update or your computer/GPU "
              "manufacturer's latest compatible graphics driver."
            : "Check the active GPU and use your distribution's supported graphics updates. An unknown driver "
              "version alone does not indicate a broken or outdated driver.", "");
    }
    return unverified("Check your device manufacturer's supported graphics updates.", "");
}

} // namespace heaven
