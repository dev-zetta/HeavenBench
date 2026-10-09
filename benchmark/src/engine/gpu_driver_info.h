#ifndef HEAVENBENCH_SRC_ENGINE_GPU_DRIVER_INFO_H_
#define HEAVENBENCH_SRC_ENGINE_GPU_DRIVER_INFO_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace heaven {

// Describes the active implementation, independently of the GPU manufacturer.
// An NVIDIA adapter may run NVIDIA's driver, Mesa NVK, or Nouveau, for example.
enum class DriverFamily {
    Unknown, Nvidia, Mesa, MesaNvk, Amd, Intel, Nouveau, Software
};

[[nodiscard]] std::string_view driver_family_name(DriverFamily family) noexcept;

// The backend selects the encoding from the active implementation's identity;
// a PCI vendor ID alone does not establish the packed version format.
enum class DriverVersionEncoding { Unknown, Nvidia, IntelWindows, Mesa };

// Unknown/zero packed versions stay empty. Mesa's explicit text takes priority
// to preserve prerelease suffixes such as -devel; Intel's Windows Vulkan number
// contains only the final two components, not the complete Windows package ID.
[[nodiscard]] std::string decode_driver_version(
    DriverVersionEncoding encoding, std::uint32_t packed_version,
    std::string_view driver_info = {});

struct OpenGlDriverInfo {
    DriverFamily family = DriverFamily::Unknown;
    std::string driver_version;
    std::string api_version;
};

// These are cached strings from the rendering context, never an auxiliary GL
// context or an unrelated adapter enumerated by the operating system. Unknown
// vendor formats keep the complete original text in the backend's driver_info.
[[nodiscard]] OpenGlDriverInfo describe_opengl_driver(
    std::string_view vendor, std::string_view renderer, std::string_view version);

// Conservative fallback when an API does not expose an implementation ID.
// Software and Mesa identities take precedence over hardware vendor names.
[[nodiscard]] DriverFamily classify_driver_text(
    std::string_view name, std::string_view detail,
    std::string_view renderer = {});

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_GPU_DRIVER_INFO_H_
