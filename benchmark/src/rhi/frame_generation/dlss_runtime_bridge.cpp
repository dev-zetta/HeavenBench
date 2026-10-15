#include "dlss_runtime_bridge.h"

#include "dlss_platform.h"
#include "dlss_runtime_inputs.h"
#include "dlss_input_validation.h"
#include "frame_generation_output.h"
#include "frame_generation_history.h"
#include "engine/camera.h"

#include <filament/Texture.h>

#include <limits>
#include <utility>

namespace heaven {

DlssRuntimeBridge::DlssRuntimeBridge(DlssPlatform& platform,
        std::shared_ptr<FrameGenerationOutput> output)
    : platform_(platform), output_(std::move(output)) {}

DlssRuntimeBridge::~DlssRuntimeBridge() = default;

DlssFrameToken DlssRuntimeBridge::begin_frame() {
    DlssFrameToken token;
    if (!platform_.begin_frame(token)) return {};
    // Preserve an acquired ticket even when its first marker fails. The
    // provider's deferred failure then reaches the accepted native callback.
    platform_.mark(token, DlssLatencyMarker::SimulationStart);
    return token;
}

bool DlssRuntimeBridge::mark(const DlssFrameToken& token, DlssLatencyMarker marker) const {
    return platform_.mark(token, marker);
}

void DlssRuntimeBridge::suspend_generation() {
    platform_.suspend_generation();
    previous_logical_frame_.reset();
    previous_native_callback_ = {};
    swapchain_epoch_ = 0;
}

VkResult DlssRuntimeBridge::drain_generation() {
    previous_logical_frame_.reset();
    previous_native_callback_ = {};
    swapchain_epoch_ = 0;
    return platform_.drain_generation();
}

std::expected<void, std::string> DlssRuntimeBridge::prepare_native(
        const filament::backend::VulkanCommandContext& context,
        const Camera& camera, const DlssRuntimeFrame& frame) {
    const auto callback_time = std::chrono::steady_clock::now();
    const auto fail = [this](std::string reason) -> std::expected<void, std::string> {
        suspend_generation();
        return std::unexpected(std::move(reason));
    };
    const auto swapchain = platform_.swapchain_description();
    if (!swapchain.native_surface || swapchain.protected_content)
        return fail("DLSS requires the actual unprotected native surface swapchain");
    if (swapchain.hdr || swapchain.vsync ||
            !dlss_runtime_detail::sdr_backbuffer_format(swapchain.format))
        return fail("DLSS currently requires SDR UNORM presentation with VSync off");
    if (!context.commandBuffer || context.device != platform_.getDevice() ||
            context.physicalDevice != platform_.getPhysicalDevice() ||
            context.queueFamily != platform_.getGraphicsQueueFamilyIndex())
        return fail("DLSS temporal inputs must be recorded on the presenting graphics queue");
    if (!output_ || !output_->valid() || !output_->color_texture() ||
            output_->color_texture()->getWidth() != swapchain.extent.width ||
            output_->color_texture()->getHeight() != swapchain.extent.height)
        return fail("Persistent DLSS output targets do not match the acquired swapchain");

    const auto& images = context.images;
    if (images[0].format != VK_FORMAT_R8G8B8A8_UNORM ||
            images[1].format != VK_FORMAT_R32_SFLOAT ||
            images[2].format != VK_FORMAT_R16G16_SFLOAT ||
            images[3].format != VK_FORMAT_R8G8B8A8_UNORM)
        return fail("DLSS requires the owned SDR color/depth/motion/UI target formats");
    // The first four resources are mandatory; the fifth is a separately
    // validated optional scene-distortion field.
    for (size_t i = 0; i != 4; ++i) {
        if (images[i].extent.width != swapchain.extent.width ||
                images[i].extent.height != swapchain.extent.height ||
                images[i].layout != VK_IMAGE_LAYOUT_GENERAL ||
                images[i].aspect != VK_IMAGE_ASPECT_COLOR_BIT)
            return fail("DLSS native callback image extent or synchronized state is invalid");
        for (size_t j = 0; j != i; ++j)
            if (images[i].image == images[j].image)
                return fail("DLSS temporal inputs unexpectedly alias one another");
    }

    // These are prospective rendered-source identities, not main-loop attempt
    // IDs. Rejected attempts retain the same token and do not break history.
    const bool consecutive = previous_logical_frame_ &&
        *previous_logical_frame_ != std::numeric_limits<uint64_t>::max() &&
        frame.token.application_frame == *previous_logical_frame_ + 1;
    const bool reset = frame.reset || !consecutive || swapchain.epoch != swapchain_epoch_ ||
        frame_generation_callback_gap(previous_native_callback_, callback_time);
    const auto camera_constants = dlss_runtime_detail::camera_constants(
        glm::inverse(camera.view_matrix()), frame.projection,
        frame.current_view_projection, frame.previous_view_projection,
        frame.near_plane, frame.far_plane, glm::radians(camera.fov),
        static_cast<float>(swapchain.extent.width) / static_cast<float>(swapchain.extent.height), reset);
    if (!camera_constants) return fail(camera_constants.error());

    DlssPresentInputs inputs;
    inputs.token = frame.token;
    inputs.camera = *camera_constants;
    inputs.hudless_color = dlss_runtime_detail::present_image(images[0]);
    inputs.depth = dlss_runtime_detail::present_image(images[1]);
    inputs.motion = dlss_runtime_detail::present_image(images[2]);
    inputs.ui_color_alpha = dlss_runtime_detail::present_image(images[3]);
    if (const auto distortion = dlss_runtime_detail::owned_distortion_input(inputs, context); !distortion)
        return fail(std::string(distortion.error()));
    inputs.backbuffer_format = swapchain.format;
    inputs.backbuffer_width = swapchain.extent.width;
    inputs.backbuffer_height = swapchain.extent.height;
    inputs.backbuffer_count = swapchain.image_count;
    inputs.frames_to_generate = 1;
    inputs.backbuffer_has_ui = frame.has_ui;
    inputs.vsync = swapchain.vsync;
    inputs.hdr = swapchain.hdr;
    inputs.temporal_coverage_complete = frame.temporal_coverage_complete;
    inputs.color_space_matches_backbuffer = true;
    inputs.inputs_valid_until_present = true;
    inputs.all_input_writes_on_presenting_queue = true;
    if (const auto invalid = validate_dlss_present_inputs(inputs, 1, 1); !invalid.empty())
        return fail(std::string(invalid));
    // Mark the end only after extraction/composition recording. The platform
    // owns PresentStart/PresentEnd around the actual intercepted native call.
    if (!platform_.mark(frame.token, DlssLatencyMarker::RenderSubmitEnd))
        return fail("DLSS render submission marker failed");
    if (!platform_.prepare_present(inputs))
        return fail(platform_.provider_status().reason);
    previous_logical_frame_ = frame.token.application_frame;
    previous_native_callback_ = callback_time;
    swapchain_epoch_ = swapchain.epoch;
    return {};
}

} // namespace heaven
