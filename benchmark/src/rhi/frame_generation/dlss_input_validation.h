#ifndef HEAVEN_DLSS_INPUT_VALIDATION_H
#define HEAVEN_DLSS_INPUT_VALIDATION_H

#include "dlss_provider.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>

namespace heaven {

inline std::string_view validate_dlss_present_inputs(
        const DlssPresentInputs& input, uint32_t minimum_dimension,
        uint32_t maximum_generated_frames) noexcept {
    if (!input.token.native_token) return "Missing Streamline frame token";
    if (!input.temporal_coverage_complete)
        return "Complete camera/object/deformation motion vectors are unavailable";
    if (!input.inputs_valid_until_present)
        return "Temporal images are not retained through asynchronous presentation";
    if (!input.all_input_writes_on_presenting_queue)
        return "Cross-queue temporal image reuse needs explicit DLSS completion synchronization";
    if (!input.color_space_matches_backbuffer)
        return "HUDless color must match the backbuffer color space and postprocessing";
    if (input.vsync) return "Vulkan DLSS frame generation currently requires VSync off";
    if (input.hdr) return "This DLSS adapter has not qualified HDR presentation";
    if (input.frames_to_generate == 0 || input.frames_to_generate > maximum_generated_frames)
        return "Requested frame multiplier is unsupported on this GPU";
    if (input.backbuffer_count == 0 || input.backbuffer_format == VK_FORMAT_UNDEFINED)
        return "Missing native swapchain description";
    if (input.backbuffer_width != input.hudless_color.width ||
            input.backbuffer_height != input.hudless_color.height)
        return "HUDless color and swapchain extents differ";

    const auto valid_image = [](const DlssImage& image) {
        return image.image != VK_NULL_HANDLE && image.view != VK_NULL_HANDLE &&
            image.memory != VK_NULL_HANDLE &&
            image.width != 0 && image.height != 0 &&
            image.format != VK_FORMAT_UNDEFINED &&
            image.layout != VK_IMAGE_LAYOUT_UNDEFINED &&
            image.layout != VK_IMAGE_LAYOUT_PREINITIALIZED &&
            image.mip_levels == 1 && image.array_layers == 1 &&
            (image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
    };
    if (!valid_image(input.depth) || !valid_image(input.motion) ||
            !valid_image(input.hudless_color))
        return "Missing or invalid native depth/motion/HUDless image description";
    if (input.depth.width != input.motion.width || input.depth.height != input.motion.height)
        return "Depth and motion-vector extents differ";
    if (input.motion.format != VK_FORMAT_R16G16_SFLOAT &&
            input.motion.format != VK_FORMAT_R32G32_SFLOAT)
        return "Motion vectors must use a signed two-channel floating-point format";
    if (input.hudless_color.width < minimum_dimension ||
            input.hudless_color.height < minimum_dimension)
        return "Output resolution is below the SDK minimum";
    if (input.backbuffer_has_ui && (!valid_image(input.ui_color_alpha) ||
            input.ui_color_alpha.width != input.hudless_color.width ||
            input.ui_color_alpha.height != input.hudless_color.height))
        return "Full-resolution premultiplied UI color/alpha is required";

    const auto finite = [](const auto& values) {
        return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
    };
    const auto& camera = input.camera;
    if (!finite(camera.view_to_clip) || !finite(camera.clip_to_view) ||
            !finite(camera.clip_to_previous_clip) || !finite(camera.previous_clip_to_clip) ||
            !finite(camera.jitter_pixels) || !finite(camera.motion_scale) ||
            !finite(camera.position) || !finite(camera.up) ||
            !finite(camera.right) || !finite(camera.forward))
        return "Non-finite temporal camera constants";
    if (!std::isfinite(camera.near_plane) || !std::isfinite(camera.far_plane) ||
            !std::isfinite(camera.vertical_fov_radians) || !std::isfinite(camera.aspect_ratio) ||
            camera.near_plane <= 0.0f || camera.far_plane <= camera.near_plane ||
            camera.vertical_fov_radians <= 0.0f ||
            camera.vertical_fov_radians >= std::numbers::pi_v<float> ||
            camera.aspect_ratio <= 0.0f || camera.motion_scale[0] == 0.0f ||
            camera.motion_scale[1] == 0.0f)
        return "Invalid temporal camera projection or motion scale";
    // Zero-initialized matrices must never silently produce an enabled feature.
    const auto has_transform = [](const auto& matrix) {
        return std::ranges::any_of(matrix, [](float value) { return value != 0.0f; });
    };
    if (!has_transform(camera.view_to_clip) || !has_transform(camera.clip_to_view) ||
            !has_transform(camera.clip_to_previous_clip) ||
            !has_transform(camera.previous_clip_to_clip))
        return "Temporal camera matrices are uninitialized";
    return {};
}

} // namespace heaven

#endif // HEAVEN_DLSS_INPUT_VALIDATION_H
