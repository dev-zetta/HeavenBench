#ifndef HEAVENBENCH_SRC_ENGINE_GPU_SELECTION_H_
#define HEAVENBENCH_SRC_ENGINE_GPU_SELECTION_H_

#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace heaven {

struct GpuSelectionIdentity {
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::array<int, 4> pci{-1, -1, -1, -1};
};

// Validate the active Vulkan device, not loader enumeration order or a model
// name. A requested index/BDF additionally needs actual PCI identity so two
// identical adapters cannot silently satisfy the wrong physical selection.
[[nodiscard]] inline std::optional<std::string> requested_gpu_error(
        std::string_view requested_id, std::string_view requested_pci,
        const GpuSelectionIdentity& active) {
    if (requested_id.empty() && requested_pci.empty()) return std::nullopt;
    const auto hex = [](std::string_view text) -> std::optional<std::uint32_t> {
        std::uint32_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
        if (text.empty() || error != std::errc{} || end != text.data() + text.size())
            return std::nullopt;
        return value;
    };
    if (requested_id.size() != 9 || requested_id[4] != ':')
        return "Invalid requested GPU identity; expected vendor:device hexadecimal IDs";
    const auto vendor = hex(requested_id.substr(0, 4));
    const auto device = hex(requested_id.substr(5, 4));
    if (!vendor || !device || *vendor == 0)
        return "Invalid requested GPU identity; expected vendor:device hexadecimal IDs";
    if (active.vendor_id == 0)
        return std::format("Cannot verify requested GPU {}: active Vulkan device identity is unavailable", requested_id);
    if (*vendor != active.vendor_id || *device != active.device_id)
        return std::format("Requested GPU {}, but Vulkan selected {:04x}:{:04x}; the driver did not honor GPU selection", requested_id, active.vendor_id, active.device_id);
    if (requested_pci.empty()) return std::nullopt;
    if (requested_pci.size() != 12 || requested_pci[4] != ':' ||
        requested_pci[7] != ':' || requested_pci[10] != '.')
        return "Invalid requested GPU PCI identity; expected domain:bus:device.function";
    const std::array component_text{
        requested_pci.substr(0, 4), requested_pci.substr(5, 2),
        requested_pci.substr(8, 2), requested_pci.substr(11, 1)};
    std::array<std::uint32_t, 4> components{};
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto component = hex(component_text[i]);
        if (!component)
            return "Invalid requested GPU PCI identity; expected domain:bus:device.function";
        components[i] = *component;
    }
    if (components[2] > 31u || components[3] > 7u)
        return "Invalid requested GPU PCI identity; expected domain:bus:device.function";
    std::array<std::uint32_t, 4> active_components{};
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (active.pci[i] < 0)
            return std::format("Cannot verify requested GPU at {}: active Vulkan device PCI identity is unavailable", requested_pci);
        active_components[i] = static_cast<std::uint32_t>(active.pci[i]);
    }
    if (components != active_components)
        return std::format("Requested GPU at {}, but Vulkan selected {:04x}:{:02x}:{:02x}.{:x}; the driver did not honor the physical GPU selection", requested_pci, active.pci[0], active.pci[1], active.pci[2], active.pci[3]);
    return std::nullopt;
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_GPU_SELECTION_H_
