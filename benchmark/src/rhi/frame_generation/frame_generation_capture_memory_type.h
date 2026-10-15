#ifndef HEAVEN_FRAME_GENERATION_CAPTURE_MEMORY_TYPE_H
#define HEAVEN_FRAME_GENERATION_CAPTURE_MEMORY_TYPE_H

#include <vulkan/vulkan_core.h>

#include <cstdint>

namespace heaven {

// Diagnostic readback consumes mapped bytes on the CPU. Prefer caching before
// coherence; noncoherent candidates use the existing mapped-range invalidate.
inline uint32_t select_frame_generation_readback_memory_type(
        const VkPhysicalDeviceMemoryProperties& memory, uint32_t memory_type_bits) noexcept {
    uint32_t selected = UINT32_MAX;
    uint32_t best_rank = 0;
    for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
        const auto flags = memory.memoryTypes[type].propertyFlags;
        if (!(memory_type_bits & (1u << type)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        if (flags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
                VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD))
            continue;
        const uint32_t rank = (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? 2u : 0u) +
                (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? 1u : 0u);
        if (selected == UINT32_MAX || rank > best_rank) {
            selected = type;
            best_rank = rank;
        }
    }
    return selected;
}

} // namespace heaven

#endif // HEAVEN_FRAME_GENERATION_CAPTURE_MEMORY_TYPE_H
