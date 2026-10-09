#include "engine/gpu_driver_info.h"

#include <algorithm>
#include <format>

namespace heaven {
namespace {

constexpr bool ascii_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

constexpr char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A')) : value;
}

std::string lower_text(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), ascii_lower);
    return result;
}

// Extract a complete dotted version token, preserving suffixes that indicate
// prereleases. Reject an incomplete or malformed numeric prefix; the policy
// must not compare a made-up release number against a minimum requirement.
std::string version_token(std::string_view text) {
    const auto end = text.find_first_of(" \t\r\n()");
    const std::string_view token = text.substr(0, end);
    std::size_t pos = 0;
    unsigned components = 0;
    while (pos < token.size()) {
        const std::size_t start = pos;
        while (pos < token.size() && ascii_digit(token[pos])) ++pos;
        if (start == pos) return {};
        ++components;
        if (pos == token.size() || token[pos] == '-' || token[pos] == '+') break;
        if (token[pos] != '.') return {};
        ++pos;
        if (pos == token.size()) return {};
    }
    if (components < 2) return {};
    return std::string(token);
}

std::string version_after(std::string_view text, std::string_view marker) {
    const std::string lowered = lower_text(text);
    const auto marker_pos = lowered.find(marker);
    if (marker_pos == std::string::npos) return {};
    return version_token(text.substr(marker_pos + marker.size()));
}

} // namespace

std::string_view driver_family_name(DriverFamily family) noexcept {
    switch (family) {
        case DriverFamily::Unknown: return "Unknown";
        case DriverFamily::Nvidia: return "NVIDIA";
        case DriverFamily::Mesa: return "Mesa";
        case DriverFamily::MesaNvk: return "Mesa NVK";
        case DriverFamily::Amd: return "AMD";
        case DriverFamily::Intel: return "Intel";
        case DriverFamily::Nouveau: return "Nouveau";
        case DriverFamily::Software: return "Software";
    }
    return "Unknown";
}

std::string decode_driver_version(DriverVersionEncoding encoding,
                                  std::uint32_t packed_version,
                                  std::string_view driver_info) {
    if (encoding == DriverVersionEncoding::Mesa &&
            lower_text(driver_info).contains("mesa ")) {
        // Explicit but malformed text is unverified. Falling back to the
        // packed number could erase an unrecognized prerelease suffix.
        return version_after(driver_info, "mesa ");
    }
    if (packed_version == 0) return {};
    // These implementation-defined layouts also appear in Khronos Vulkan-Tools
    // and Dawn's DecodeVulkanDriverVersion. Never apply NVIDIA's layout to NVK,
    // or Intel Windows' layout to the Intel Mesa implementation.
    switch (encoding) {
        case DriverVersionEncoding::Nvidia:
            return std::format("{}.{}.{}.{}", (packed_version >> 22) & 0x3ffu,
                (packed_version >> 14) & 0xffu, (packed_version >> 6) & 0xffu,
                packed_version & 0x3fu);
        case DriverVersionEncoding::IntelWindows:
            return std::format("{}.{}", packed_version >> 14,
                               packed_version & 0x3fffu);
        case DriverVersionEncoding::Mesa:
            return std::format("{}.{}.{}", packed_version >> 22,
                (packed_version >> 12) & 0x3ffu, packed_version & 0xfffu);
        case DriverVersionEncoding::Unknown:
            return {};
    }
    return {};
}

DriverFamily classify_driver_text(std::string_view name, std::string_view detail,
                                   std::string_view renderer) {
    const std::string text = lower_text(std::format("{} {} {}", name, detail, renderer));
    if (text.contains("llvmpipe") || text.contains("softpipe") ||
        text.contains("lavapipe") || text.contains("swiftshader") ||
        text.contains("software rasterizer") || text.contains("gdi generic") ||
        text.contains("microsoft basic render driver") ||
        text.contains("microsoft basic display adapter")) {
        return DriverFamily::Software;
    }
    if (text.contains("nvk")) return DriverFamily::MesaNvk;
    if (text.contains("nouveau")) return DriverFamily::Nouveau;
    if (text.contains("mesa") || text.contains("radv")) return DriverFamily::Mesa;

    // Use driver/vendor identity only here: a renderer name can refer to the
    // hardware behind an unknown implementation and is not enough evidence.
    const std::string identity = lower_text(name);
    if (identity.contains("nvidia")) return DriverFamily::Nvidia;
    if (identity.contains("advanced micro devices") || identity == "amd" ||
        identity.contains("ati technologies") || identity.contains("amdvlk")) {
        return DriverFamily::Amd;
    }
    if (identity.contains("intel")) return DriverFamily::Intel;
    return DriverFamily::Unknown;
}

OpenGlDriverInfo describe_opengl_driver(std::string_view vendor,
                                       std::string_view renderer,
                                       std::string_view version) {
    OpenGlDriverInfo info;
    info.family = classify_driver_text(vendor, version, renderer);
    // GL_VERSION begins with the API version; its trailing text identifies the
    // driver. Keep those separate so OpenGL 4.6 never becomes a driver release.
    std::string_view api_text = version;
    if (api_text.starts_with("OpenGL ES-CM ") ||
        api_text.starts_with("OpenGL ES-CL ")) api_text.remove_prefix(13);
    else if (api_text.starts_with("OpenGL ES ")) api_text.remove_prefix(10);
    info.api_version = version_token(api_text);

    info.driver_version = version_after(version, "mesa ");
    if (!info.driver_version.empty()) return info;
    if (info.family == DriverFamily::Nvidia) {
        info.driver_version = version_after(version, "nvidia ");
    } else if (info.family == DriverFamily::Intel) {
        info.driver_version = version_after(version, "build ");
    }
    // AMD's GL implementation version is not the Adrenalin package version.
    // Unrecognized formats remain available verbatim in GpuInfoRaw::driver_info.
    return info;
}

} // namespace heaven
