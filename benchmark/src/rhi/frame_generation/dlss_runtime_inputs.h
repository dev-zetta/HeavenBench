#ifndef HEAVEN_DLSS_RUNTIME_INPUTS_H
#define HEAVEN_DLSS_RUNTIME_INPUTS_H

#include "dlss_provider.h"

#include <backend/VulkanCommandContext.h>
#include <glm/glm.hpp>

#include <cmath>
#include <expected>
#include <numbers>
#include <string>
#include <string_view>

namespace heaven::dlss_runtime_detail {

inline bool finite_matrix(const glm::mat4& matrix) noexcept {
    for (int column = 0; column != 4; ++column)
        for (int row = 0; row != 4; ++row)
            if (!std::isfinite(matrix[column][row])) return false;
    return true;
}

// Streamline matrices act on row vectors (sl_matrix_helpers.h). GLM acts on
// column vectors, hence the transpose, in addition to row-major storage.
inline std::array<float, 16> streamline_matrix(const glm::mat4& column_transform) noexcept {
    std::array<float, 16> result{};
    for (int row = 0; row != 4; ++row)
        for (int column = 0; column != 4; ++column)
            result[static_cast<size_t>(row) * 4 + static_cast<size_t>(column)] =
                column_transform[row][column];
    return result;
}

// GL user clip coordinates -> Streamline clip coordinates. Its mvec.hlsl
// reconstructs Y as 1 - 2 * UV.y, so GL Y is retained even for Vulkan images.
// Extracted reversed depth is 0.5 - 0.5 * GL clip Z / W.
inline glm::mat4 native_clip_transform() noexcept {
    glm::mat4 result(1.0f);
    result[2][2] = -0.5f;
    result[3][2] = 0.5f;
    return result;
}

inline std::expected<DlssCameraConstants, std::string> camera_constants(
        const glm::mat4& world_from_view, const glm::mat4& gl_projection,
        const glm::mat4& current_gl_view_projection,
        const glm::mat4& previous_gl_view_projection,
        float near_plane, float far_plane, float vertical_fov_radians,
        float aspect_ratio, bool reset) {
    const std::array matrices{world_from_view, gl_projection,
        current_gl_view_projection, previous_gl_view_projection};
    for (const auto& matrix : matrices) {
        const float determinant = glm::determinant(matrix);
        if (!finite_matrix(matrix) || !std::isfinite(determinant) ||
                std::abs(determinant) < 1e-12f)
            return std::unexpected("Non-finite or singular DLSS camera transform");
    }
    if (!std::isfinite(near_plane) || !std::isfinite(far_plane) ||
            near_plane <= 0.0f || far_plane <= near_plane ||
            !std::isfinite(vertical_fov_radians) || vertical_fov_radians <= 0.0f ||
            vertical_fov_radians >= std::numbers::pi_v<float> ||
            !std::isfinite(aspect_ratio) || aspect_ratio <= 0.0f)
        return std::unexpected("Invalid DLSS camera projection limits");

    const auto native_clip = native_clip_transform();
    const auto projection = native_clip * gl_projection;
    const auto current = native_clip * current_gl_view_projection;
    const auto previous = native_clip * previous_gl_view_projection;
    const auto previous_from_current = reset ? glm::mat4(1.0f) : previous * glm::inverse(current);
    DlssCameraConstants constants;
    constants.view_to_clip = streamline_matrix(projection);
    constants.clip_to_view = streamline_matrix(glm::inverse(projection));
    constants.clip_to_previous_clip = streamline_matrix(previous_from_current);
    constants.previous_clip_to_clip = streamline_matrix(glm::inverse(previous_from_current));
    if (!finite_matrix(projection) || !finite_matrix(previous_from_current))
        return std::unexpected("DLSS camera inversion produced non-finite values");
    const glm::vec3 position(world_from_view[3]);
    const glm::vec3 right = glm::normalize(glm::vec3(world_from_view[0]));
    const glm::vec3 up = glm::normalize(glm::vec3(world_from_view[1]));
    const glm::vec3 forward = -glm::normalize(glm::vec3(world_from_view[2]));
    constants.position = {position.x, position.y, position.z};
    constants.right = {right.x, right.y, right.z};
    constants.up = {up.x, up.y, up.z};
    constants.forward = {forward.x, forward.y, forward.z};
    constants.near_plane = near_plane;
    constants.far_plane = far_plane;
    constants.vertical_fov_radians = vertical_fov_radians;
    constants.aspect_ratio = aspect_ratio;
    constants.depth_inverted = true;
    constants.reset = reset;
    // Packed motion is current -> previous normalized top-left UV. The
    // unjittered temporal proxy does not include Filament's TAA sample offset.
    constants.motion_scale = {1.0f, 1.0f};
    constants.jitter_pixels = {0.0f, 0.0f};
    constants.motion_jittered = false;
    return constants;
}

inline DlssImage present_image(const filament::backend::VulkanCommandImage& native) noexcept {
    DlssImage result;
    result.image = native.image;
    result.view = native.view;
    result.memory = native.memory;
    result.format = native.format;
    // The callback's synchronized GENERAL layout is temporary. The SDK reads
    // later, after Filament has restored its recorded original image state.
    result.layout = native.restoredLayout;
    result.usage = native.usage;
    result.width = native.extent.width;
    result.height = native.extent.height;
    result.mip_levels = native.mipLevels;
    result.array_layers = native.arrayLayers;
    return result;
}

// Slot four is supplied only by the owned coherent distortion pipeline: its
// HUDless/backbuffer warp matches and depth/motion remain undistorted. Native
// metadata can verify compatibility and lifetime state, not inverse-field pixel
// quality. The runtime must omit this slot when that pipeline is inactive.
inline std::expected<void, std::string_view> owned_distortion_input(
        DlssPresentInputs& inputs, const filament::backend::VulkanCommandContext& context) noexcept {
    static_assert(filament::backend::NativeCommand::TEXTURE_COUNT > 4);
    inputs.bidirectional_distortion = {};
    inputs.distortion_active = false;
    inputs.distortion_field_valid = false;
    inputs.depth_motion_undistorted = false;
    inputs.hudless_distortion_matches_backbuffer = false;
    const auto& field = context.images[4];
    if (!field.image) {
        if (field.view || field.memory)
            return std::unexpected("Optional DLSS distortion metadata has no native image");
        return {};
    }
    if (!field.view || !field.memory ||
            (field.format != VK_FORMAT_R16G16B16A16_SFLOAT &&
             field.format != VK_FORMAT_R32G32B32A32_SFLOAT))
        return std::unexpected("DLSS distortion requires an owned signed RGBA16F/RGBA32F image");
    if (!field.extent.width || !field.extent.height ||
            field.extent.width > inputs.hudless_color.width ||
            field.extent.height > inputs.hudless_color.height ||
            uint64_t(field.extent.width) * inputs.hudless_color.height !=
                uint64_t(field.extent.height) * inputs.hudless_color.width)
        return std::unexpected("DLSS distortion field and HUDless scene aspect/extents differ");
    if (field.layout != VK_IMAGE_LAYOUT_GENERAL || field.aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
            field.mipLevels != 1 || field.arrayLayers != 1 ||
            !(field.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        return std::unexpected("DLSS distortion native callback state or sampled view is invalid");
    if (field.restoredLayout != VK_IMAGE_LAYOUT_GENERAL &&
            field.restoredLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            field.restoredLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            field.restoredLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            field.restoredLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        return std::unexpected("DLSS distortion restored color-image layout is invalid");
    for (size_t index = 0; index != 4; ++index)
        if (field.image == context.images[index].image)
            return std::unexpected("DLSS distortion field aliases a mandatory temporal input");
    inputs.bidirectional_distortion = present_image(field);
    inputs.distortion_active = true;
    inputs.distortion_field_valid = true;
    inputs.depth_motion_undistorted = true;
    inputs.hudless_distortion_matches_backbuffer = true;
    return {};
}

inline bool sdr_backbuffer_format(VkFormat format) noexcept {
    // Both targets and the final composite already contain encoded SDR RGB.
    // An SRGB attachment would apply an additional encoding on the real frame.
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
}

} // namespace heaven::dlss_runtime_detail

#endif // HEAVEN_DLSS_RUNTIME_INPUTS_H
