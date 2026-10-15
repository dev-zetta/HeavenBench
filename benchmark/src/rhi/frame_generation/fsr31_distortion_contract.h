#ifndef HEAVEN_FSR31_DISTORTION_CONTRACT_H
#define HEAVEN_FSR31_DISTORTION_CONTRACT_H

#include "fsr31_vulkan.h"

namespace heaven {

// Shared by the real provider and GPU-free contracts. This validates resource
// metadata only; the production output's pixel oracle establishes the map's
// top-left convention and relation to undistorted depth/motion.
inline const char* fsr31_distortion_error(const Fsr31VulkanFrame& frame) noexcept {
    const auto& field = frame.distortion_field;
    if (!field.image) return nullptr;
    if (field.format != VK_FORMAT_R16G16B16A16_SFLOAT &&
        field.format != VK_FORMAT_R32G32B32A32_SFLOAT)
        return "FSR distortion field requires signed RGBA16F or RGBA32F";
    if (!(field.usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        (field.layout != VK_IMAGE_LAYOUT_GENERAL && field.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
        return "FSR distortion field requires a sampled GENERAL or SHADER_READ_ONLY_OPTIMAL image";
    if (!field.extent.width || !field.extent.height || field.extent.width > 16384 || field.extent.height > 16384 ||
        field.extent.width > frame.hudless_color.extent.width || field.extent.height > frame.hudless_color.extent.height ||
        uint64_t(field.extent.width) * frame.hudless_color.extent.height !=
        uint64_t(field.extent.height) * frame.hudless_color.extent.width)
        return "FSR distortion field must preserve the color aspect ratio without exceeding its extent";
    if (field.image == frame.depth.image || field.image == frame.motion_vectors.image ||
        field.image == frame.hudless_color.image || field.image == frame.generated_output.image)
        return "FSR distortion field must not alias depth, motion, color or generated output";
    return nullptr;
}

// Preserve the SDK's optional identity field explicitly, including a caller
// reusing its dispatch description. Resource conversion stays provider-owned.
template<class Dispatch, class ResourceBuilder>
void fsr31_bind_distortion(Dispatch& dispatch, const Fsr31VulkanFrame& frame, ResourceBuilder&& builder) {
    if (frame.distortion_field.image)
        dispatch.distortionField = builder(frame.distortion_field, L"Heaven coherent distortion");
    else dispatch.distortionField = {};
}

} // namespace heaven

#endif // HEAVEN_FSR31_DISTORTION_CONTRACT_H
