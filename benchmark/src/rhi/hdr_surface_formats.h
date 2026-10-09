#ifndef HEAVENBENCH_SRC_RHI_HDR_SURFACE_FORMATS_H_
#define HEAVENBENCH_SRC_RHI_HDR_SURFACE_FORMATS_H_

#include "engine/types.h"

#include <bluevk/BlueVK.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace heaven {

// Keep this list in sync with the HDR selection in VulkanPlatformSwapChainImpl.
// A high-precision SDR format or an HDR colorspace with an unsupported pixel
// format cannot accept the renderer's PQ output.
inline bool supports_hdr10_surface(std::span<const VkSurfaceFormatKHR> formats) {
    return std::ranges::any_of(formats, [](const VkSurfaceFormatKHR& format) {
        return format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
            (format.format == VK_FORMAT_R16G16B16A16_SFLOAT ||
             format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
             format.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
    });
}

// Vulkan permits the list to grow between its count and data queries. Retry a
// bounded number of times; an incomplete list is not evidence of HDR support.
// The query is injected so enumeration failures can be tested without a GPU.
template <typename Query>
Result<void> require_hdr10_surface(Query&& query) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::uint32_t count = 0;
        if (const auto result = query(&count, nullptr); result != VK_SUCCESS) {
            return std::unexpected(Error(std::format(
                "HDR surface format query failed (VkResult {})", int(result))));
        }
        if (count == 0) break;
        std::vector<VkSurfaceFormatKHR> formats(count);
        const auto result = query(&count, formats.data());
        if (result == VK_INCOMPLETE) continue;
        if (result != VK_SUCCESS) {
            return std::unexpected(Error(std::format(
                "HDR surface format query failed (VkResult {})", int(result))));
        }
        if (count > formats.size()) {
            return std::unexpected(Error("HDR surface format query returned an invalid count"));
        }
        formats.resize(count);
        if (supports_hdr10_surface(formats)) return {};
        break;
    }
    return std::unexpected(Error(
        "--hdr requires an HDR10 ST.2084 surface format. Enable HDR for this "
        "display in the Wayland compositor, or run without --hdr."));
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_HDR_SURFACE_FORMATS_H_
