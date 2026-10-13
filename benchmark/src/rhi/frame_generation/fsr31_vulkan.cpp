#include "fsr31_vulkan.h"
#include "fsr31_prepare_scope.h"

#include <bit>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace heaven {
namespace {

std::string sdk_error(const char* operation, FfxErrorCode code) {
    return std::string(operation) + " failed (FidelityFX error " + std::to_string(static_cast<int>(code)) + ")";
}

bool valid_extent(VkExtent2D extent) {
    return extent.width > 0 && extent.height > 0 && extent.width <= 16384 && extent.height <= 16384;
}

bool same_extent(VkExtent2D a, VkExtent2D b) {
    return a.width == b.width && a.height == b.height;
}

FfxDimensions2D dimensions(VkExtent2D e) { return {e.width, e.height}; }

bool finite_basis(const std::array<float, 3>& v) {
    float length_squared = 0.0f;
    for (float value : v) {
        if (!std::isfinite(value)) return false;
        length_squared += value * value;
    }
    return std::abs(length_squared - 1.0f) < 0.01f;
}

FfxResource image_resource(const Fsr31VulkanImage& image, const wchar_t* name) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = image.format;
    info.extent = {image.extent.width, image.extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = image.usage;
    const auto description = ffxGetImageResourceDescriptionVK(image.image, info);
    return ffxGetResourceVK(reinterpret_cast<void*>(image.image), description, name,
        image.layout == VK_IMAGE_LAYOUT_GENERAL ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ);
}

} // namespace

struct Fsr31Vulkan::Impl {
    // The SDK maps alignas(32) EffectContext records inside this allocation.
    struct alignas(32) ScratchBlock { std::array<std::byte, 32> bytes; };
    Fsr31VulkanConfig config;
    VkDeviceContext device_context{};
    std::vector<ScratchBlock> scratch;
    FfxInterface backend{};
    std::unique_ptr<FfxFrameInterpolationContext> interpolation;
    std::unique_ptr<FfxOpticalflowContext> optical_flow;
    uint32_t shared_context = 0;
    std::array<FfxResourceInternal, 5> shared{};
    size_t shared_count = 0;
    bool shared_created = false;
    bool interpolation_started = false;
    bool optical_flow_started = false;
    bool force_reset = true;
    bool has_history = false;
    uint64_t last_frame = 0;
    uint64_t memory_bytes = 0;

    ~Impl() {
        // The owner drains GPU work first. Keep scratch/interface alive until
        // every component and its externally allocated shared image is gone.
        if (interpolation_started) ffxFrameInterpolationContextDestroy(interpolation.get());
        if (optical_flow_started) ffxOpticalflowContextDestroy(optical_flow.get());
        for (size_t i = 0; i < shared_count; ++i) backend.fpDestroyResource(&backend, shared[i], shared_context);
        if (shared_created) backend.fpDestroyBackendContext(&backend, shared_context);
    }

    FfxResource resource(size_t index) {
        return backend.fpGetResource(&backend, shared[index]);
    }

    std::expected<void, std::string> initialize(const Fsr31VulkanConfig& input) {
        config = input;
        if (!config.physical_device || !config.device || !config.get_device_proc_addr ||
            !valid_extent(config.max_render_size) || !valid_extent(config.display_size)) {
            return std::unexpected("FSR 3.1 requires a native Vulkan device and valid render/display extents");
        }
        if (!config.storage_image_extended_formats_enabled || !config.storage_image_read_without_format_enabled ||
            !config.storage_image_write_without_format_enabled) {
            return std::unexpected("FSR 3.1 requires enabled Vulkan storage-image extended formats and formatless read/write features");
        }
        VkPhysicalDeviceSubgroupProperties subgroups{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroups};
        vkGetPhysicalDeviceProperties2(config.physical_device, &properties);
        if (properties.properties.apiVersion < VK_API_VERSION_1_1) {
            return std::unexpected("FSR 3.1 shaders require Vulkan 1.1");
        }
        constexpr VkSubgroupFeatureFlags required = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
        if (!(subgroups.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || (subgroups.supportedOperations & required) != required) {
            return std::unexpected("FSR 3.1 requires compute subgroup basic, arithmetic and quad operations");
        }
        // The pinned FP32 permutations include a 256x1x1 workgroup; Vulkan
        // 1.1 only guarantees 128 invocations and an X dimension of 128.
        const auto& limits = properties.properties.limits;
        if (limits.maxComputeWorkGroupInvocations < 256 || limits.maxComputeWorkGroupSize[0] < 256 ||
            limits.maxComputeWorkGroupSize[1] < 16 || limits.maxComputeWorkGroupSize[2] < 4) {
            return std::unexpected("FSR 3.1 requires 256-invocation compute workgroups and X/Y/Z limits of at least 256/16/4");
        }
        // The inpainting-pyramid permutation binds twelve storage images,
        // beyond the baseline guarantee for a Vulkan 1.1 device.
        if (limits.maxPerStageDescriptorStorageImages < 12 || limits.maxDescriptorSetStorageImages < 12) {
            return std::unexpected("FSR 3.1 requires at least 12 storage-image descriptors per compute stage and descriptor set");
        }
        if (config.max_render_size.width > limits.maxImageDimension2D || config.max_render_size.height > limits.maxImageDimension2D ||
            config.display_size.width > limits.maxImageDimension2D || config.display_size.height > limits.maxImageDimension2D) {
            return std::unexpected("FSR 3.1 render/display dimensions exceed the physical device image limit");
        }
        VkFormatProperties color_properties{};
        vkGetPhysicalDeviceFormatProperties(config.physical_device, config.color_format, &color_properties);
        constexpr VkFormatFeatureFlags color_required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((color_properties.optimalTilingFeatures & color_required) != color_required ||
            ffxGetSurfaceFormatVK(config.color_format) == FFX_SURFACE_FORMAT_UNKNOWN) {
            return std::unexpected("FSR 3.1 color format must support sampled and storage images");
        }
        const size_t scratch_bytes = ffxGetScratchMemorySizeVK(config.physical_device, 3);
        scratch.resize((scratch_bytes + sizeof(ScratchBlock) - 1) / sizeof(ScratchBlock));
        device_context = {config.device, config.physical_device, config.get_device_proc_addr};
        auto result = ffxGetInterfaceVK(&backend,
            ffxGetDeviceVK(&device_context),
            scratch.data(), scratch.size() * sizeof(ScratchBlock), 3);
        if (result != FFX_OK) return std::unexpected(sdk_error("Vulkan backend initialization", result));
        result = backend.fpCreateBackendContext(&backend, FFX_EFFECT_SHAREDRESOURCES, nullptr, &shared_context);
        if (result != FFX_OK) return std::unexpected(sdk_error("Shared context creation", result));
        shared_created = true;

        optical_flow = std::make_unique<FfxOpticalflowContext>();
        FfxOpticalflowContextDescription optical_description{};
        optical_description.backendInterface = backend;
        optical_description.resolution = dimensions(config.display_size);
        optical_flow_started = true;
        result = ffxOpticalflowContextCreate(optical_flow.get(), &optical_description);
        if (result != FFX_OK) return std::unexpected(sdk_error("Optical-flow context creation", result));

        interpolation = std::make_unique<FfxFrameInterpolationContext>();
        FfxFrameInterpolationContextDescription description{};
        description.backendInterface = backend;
        description.maxRenderSize = dimensions(config.max_render_size);
        description.displaySize = dimensions(config.display_size);
        description.backBufferFormat = ffxGetSurfaceFormatVK(config.color_format);
        description.previousInterpolationSourceFormat = description.backBufferFormat;
        if (config.inverted_depth) description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
        if (config.infinite_depth) description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE;
        if (config.jittered_motion_vectors) description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS;
        if (config.hdr_color) description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT;
        interpolation_started = true;
        result = ffxFrameInterpolationContextCreate(interpolation.get(), &description);
        if (result != FFX_OK) return std::unexpected(sdk_error("Interpolation context creation", result));

        FfxOpticalflowSharedResourceDescriptions optical_resources{};
        FfxFrameInterpolationSharedResourceDescriptions interpolation_resources{};
        result = ffxOpticalflowGetSharedResourceDescriptions(optical_flow.get(), &optical_resources);
        if (result != FFX_OK) return std::unexpected(sdk_error("Optical-flow resource descriptions", result));
        result = ffxFrameInterpolationGetSharedResourceDescriptions(interpolation.get(), &interpolation_resources);
        if (result != FFX_OK) return std::unexpected(sdk_error("Interpolation resource descriptions", result));
        std::array<FfxCreateResourceDescription, 5> descriptions{
            optical_resources.opticalFlowVector, optical_resources.opticalFlowSCD,
            interpolation_resources.dilatedDepth, interpolation_resources.dilatedMotionVectors,
            interpolation_resources.reconstructedPrevNearestDepth};
        for (const auto& resource_description : descriptions) {
            result = backend.fpCreateResource(&backend, &resource_description, shared_context, &shared[shared_count]);
            if (result != FFX_OK) return std::unexpected(sdk_error("Shared image allocation", result));
            ++shared_count;
        }
        FfxEffectMemoryUsage shared_memory{}, optical_memory{}, interpolation_memory{};
        backend.fpGetEffectGpuMemoryUsage(&backend, shared_context, &shared_memory);
        ffxOpticalflowContextGetGpuMemoryUsage(optical_flow.get(), &optical_memory);
        ffxFrameInterpolationContextGetGpuMemoryUsage(interpolation.get(), &interpolation_memory);
        memory_bytes = shared_memory.totalUsageInBytes + optical_memory.totalUsageInBytes + interpolation_memory.totalUsageInBytes;
        return {};
    }
};

Fsr31Vulkan::Fsr31Vulkan() : impl_(std::make_unique<Impl>()) {}
Fsr31Vulkan::~Fsr31Vulkan() = default;

std::expected<std::unique_ptr<Fsr31Vulkan>, std::string> Fsr31Vulkan::create(const Fsr31VulkanConfig& config) {
    auto provider = std::unique_ptr<Fsr31Vulkan>(new Fsr31Vulkan());
    auto result = provider->impl_->initialize(config);
    if (!result) return std::unexpected(result.error());
    return provider;
}

std::expected<bool, std::string> Fsr31Vulkan::record(const Fsr31VulkanFrame& f) {
    auto& state = *impl_;
    if (!f.commands || !f.depth.image || !f.motion_vectors.image || !f.hudless_color.image || !f.generated_output.image) {
        return std::unexpected("FSR 3.1 dispatch requires native commands and four live images");
    }
    if (!same_extent(f.depth.extent, f.motion_vectors.extent) || !valid_extent(f.depth.extent) ||
        f.depth.extent.width > state.config.max_render_size.width || f.depth.extent.height > state.config.max_render_size.height ||
        !same_extent(f.hudless_color.extent, state.config.display_size) || !same_extent(f.generated_output.extent, state.config.display_size) ||
        f.hudless_color.format != state.config.color_format || f.generated_output.format != state.config.color_format ||
        f.hudless_color.image == f.generated_output.image) {
        return std::unexpected("FSR 3.1 dispatch image extents/formats do not match the context");
    }
    for (const auto* input : {&f.depth, &f.motion_vectors, &f.hudless_color}) {
        if (!(input->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
            (input->layout != VK_IMAGE_LAYOUT_GENERAL && input->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
            return std::unexpected("FSR 3.1 sampled inputs must enter in GENERAL or SHADER_READ_ONLY_OPTIMAL");
        }
    }
    if (!(f.hudless_color.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        !(f.generated_output.usage & VK_IMAGE_USAGE_STORAGE_BIT) || f.generated_output.layout != VK_IMAGE_LAYOUT_GENERAL ||
        (f.motion_vectors.format != VK_FORMAT_R16G16_SFLOAT && f.motion_vectors.format != VK_FORMAT_R32G32_SFLOAT) ||
        (f.depth.format != VK_FORMAT_D32_SFLOAT && f.depth.format != VK_FORMAT_R32_SFLOAT)) {
        return std::unexpected("FSR 3.1 requires R32F/D32F depth, RG16F/RG32F velocity, transferable color and GENERAL storage output");
    }
    // Infinite projection uses the exact rendered near plane and the depth
    // flags. A finite sentinel avoids inf/inf in the SDK's unused finite
    // coefficient calculation and never substitutes the culling far plane.
    const float sdk_far = state.config.infinite_depth ? std::numeric_limits<float>::max() : f.camera_far;
    if (!std::isfinite(f.frame_time_ms) || f.frame_time_ms <= 0.0f ||
        !std::isfinite(f.camera_near) || f.camera_near <= 0.0f || !std::isfinite(sdk_far) || sdk_far <= f.camera_near ||
        !std::isfinite(f.vertical_fov_radians) || f.vertical_fov_radians <= 0.0f || f.vertical_fov_radians >= 3.14f ||
        !std::isfinite(f.view_units_to_meters) || f.view_units_to_meters <= 0.0f ||
        !finite_basis(f.camera_up) || !finite_basis(f.camera_right) || !finite_basis(f.camera_forward)) {
        return std::unexpected("FSR 3.1 requires finite frame timing and a normalized perspective camera");
    }
    for (float value : f.camera_position) if (!std::isfinite(value)) return std::unexpected("Nonfinite FSR camera position");
    for (float value : f.jitter_pixels) if (!std::isfinite(value)) return std::unexpected("Nonfinite FSR jitter");
    for (float value : f.motion_vector_scale) if (!std::isfinite(value) || value == 0.0f) return std::unexpected("Invalid FSR motion-vector scale");
    auto dot = [](const auto& a, const auto& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
    if (std::abs(dot(f.camera_up, f.camera_right)) > 0.01f || std::abs(dot(f.camera_up, f.camera_forward)) > 0.01f ||
        std::abs(dot(f.camera_right, f.camera_forward)) > 0.01f ||
        !std::isfinite(f.min_luminance) || !std::isfinite(f.max_luminance) || f.min_luminance < 0.0f || f.max_luminance <= f.min_luminance ||
        (f.transfer_function != Fsr31TransferFunction::Srgb && f.transfer_function != Fsr31TransferFunction::Pq && f.transfer_function != Fsr31TransferFunction::LinearScRgb)) {
        return std::unexpected("FSR 3.1 requires an orthogonal camera basis and valid color-transfer/luminance inputs");
    }

    const bool reset = f.reset || state.force_reset || !state.has_history || state.last_frame == std::numeric_limits<uint64_t>::max() || f.frame_id != state.last_frame + 1;
    const auto commands = ffxGetCommandListVK(f.commands);
    const auto color = image_resource(f.hudless_color, L"Heaven HUDless color");
    FfxFrameInterpolationPrepareDescription prepare{};
    prepare.commandList = commands;
    prepare.renderSize = dimensions(f.depth.extent);
    prepare.jitterOffset = {f.jitter_pixels[0], f.jitter_pixels[1]};
    prepare.motionVectorScale = {f.motion_vector_scale[0], f.motion_vector_scale[1]};
    prepare.frameTimeDelta = f.frame_time_ms;
    prepare.cameraNear = f.camera_near;
    prepare.cameraFar = sdk_far;
    prepare.viewSpaceToMetersFactor = f.view_units_to_meters;
    prepare.cameraFovAngleVertical = f.vertical_fov_radians;
    prepare.depth = image_resource(f.depth, L"Heaven depth");
    prepare.motionVectors = image_resource(f.motion_vectors, L"Heaven velocity");
    prepare.frameID = f.frame_id;
    prepare.dilatedDepth = state.resource(2);
    prepare.dilatedMotionVectors = state.resource(3);
    prepare.reconstructedPrevDepth = state.resource(4);
    std::copy(f.camera_position.begin(), f.camera_position.end(), prepare.cameraPosition);
    std::copy(f.camera_up.begin(), f.camera_up.end(), prepare.cameraUp);
    std::copy(f.camera_right.begin(), f.camera_right.end(), prepare.cameraRight);
    std::copy(f.camera_forward.begin(), f.camera_forward.end(), prepare.cameraForward);
    FfxErrorCode result;
    {
        Fsr31PrepareViewScope prepare_scope(state.backend);
        result = ffxFrameInterpolationPrepare(state.interpolation.get(), &prepare);
    }
    if (result != FFX_OK) { state.force_reset = true; return std::unexpected(sdk_error("Interpolation prepare", result)); }

    const auto transfer = static_cast<FfxBackbufferTransferFunction>(f.transfer_function);
    FfxOpticalflowDispatchDescription optical{};
    optical.commandList = commands;
    optical.color = color;
    optical.opticalFlowVector = state.resource(0);
    optical.opticalFlowSCD = state.resource(1);
    optical.reset = reset;
    optical.backbufferTransferFunction = transfer;
    optical.minMaxLuminance = {f.min_luminance, f.max_luminance};
    result = ffxOpticalflowContextDispatch(state.optical_flow.get(), &optical);
    if (result != FFX_OK) { state.force_reset = true; return std::unexpected(sdk_error("Optical-flow dispatch", result)); }

    FfxFrameInterpolationDispatchDescription dispatch{};
    dispatch.commandList = commands;
    dispatch.displaySize = dimensions(state.config.display_size);
    dispatch.renderSize = dimensions(f.depth.extent);
    dispatch.currentBackBuffer = color;
    dispatch.output = image_resource(f.generated_output, L"Heaven generated frame");
    dispatch.interpolationRect = {0, 0, static_cast<int32_t>(state.config.display_size.width), static_cast<int32_t>(state.config.display_size.height)};
    dispatch.opticalFlowVector = state.resource(0);
    dispatch.opticalFlowSceneChangeDetection = state.resource(1);
    dispatch.opticalFlowBufferSize = {dispatch.opticalFlowVector.description.width, dispatch.opticalFlowVector.description.height};
    dispatch.opticalFlowScale = {1.0f / state.config.display_size.width, 1.0f / state.config.display_size.height};
    dispatch.opticalFlowBlockSize = 8;
    dispatch.cameraNear = f.camera_near;
    dispatch.cameraFar = sdk_far;
    dispatch.cameraFovAngleVertical = f.vertical_fov_radians;
    dispatch.viewSpaceToMetersFactor = f.view_units_to_meters;
    dispatch.frameTimeDelta = f.frame_time_ms;
    dispatch.reset = reset;
    dispatch.backBufferTransferFunction = transfer;
    dispatch.minMaxLuminance[0] = f.min_luminance;
    dispatch.minMaxLuminance[1] = f.max_luminance;
    dispatch.frameID = f.frame_id;
    dispatch.dilatedDepth = state.resource(2);
    dispatch.dilatedMotionVectors = state.resource(3);
    dispatch.reconstructedPrevDepth = state.resource(4);
    result = ffxFrameInterpolationDispatch(state.interpolation.get(), &dispatch);
    if (result != FFX_OK) { state.force_reset = true; return std::unexpected(sdk_error("Interpolation dispatch", result)); }
    state.force_reset = false;
    state.has_history = true;
    state.last_frame = f.frame_id;
    return !reset;
}

void Fsr31Vulkan::invalidate_history() noexcept { impl_->force_reset = true; }
uint64_t Fsr31Vulkan::gpu_memory_bytes() const noexcept { return impl_->memory_bytes; }

} // namespace heaven
