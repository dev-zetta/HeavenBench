#ifndef HEAVEN_DLSS_DEVICE_CREATION_H
#define HEAVEN_DLSS_DEVICE_CREATION_H

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <new>
#include <vector>

namespace heaven::dlss_device_creation_detail {

// Streamline's automatic creation path records the physical-device parent in
// its enumeration proxy. Native enumeration alone does not populate that map.
// Keep the selected Filament device, but establish its parent before creation.
inline VkResult create_device(VkInstance instance, VkPhysicalDevice physical,
        const VkDeviceCreateInfo& info, PFN_vkEnumeratePhysicalDevices enumerate,
        PFN_vkCreateDevice create, VkDevice* device) noexcept {
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    *device = VK_NULL_HANDLE;
    if (!instance || !physical || !enumerate || !create)
        return VK_ERROR_INITIALIZATION_FAILED;
    try {
        constexpr unsigned maximum_attempts = 4;
        constexpr uint32_t maximum_devices = 4096;
        for (unsigned attempt = 0; attempt != maximum_attempts; ++attempt) {
            uint32_t count = 0;
            auto result = enumerate(instance, &count, nullptr);
            if (result == VK_INCOMPLETE) continue;
            if (result != VK_SUCCESS) return result;
            if (count == 0 || count > maximum_devices)
                return VK_ERROR_INITIALIZATION_FAILED;
            std::vector<VkPhysicalDevice> devices(count);
            result = enumerate(instance, &count, devices.data());
            if (result == VK_INCOMPLETE) continue;
            if (result != VK_SUCCESS) return result;
            if (count > devices.size()) return VK_ERROR_INITIALIZATION_FAILED;
            const auto end = devices.begin() + count;
            if (std::find(devices.begin(), end, physical) == end)
                return VK_ERROR_INITIALIZATION_FAILED;
            return create(physical, &info, nullptr, device);
        }
        return VK_INCOMPLETE;
    } catch (const std::bad_alloc&) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (...) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

} // namespace heaven::dlss_device_creation_detail

#endif // HEAVEN_DLSS_DEVICE_CREATION_H
