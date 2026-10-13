#pragma once

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace heaven {

// No Streamline or Filament declarations cross this application-owned boundary.
// The optional implementation targets the pinned Streamline 2.14.1 headers.
struct DlssBootstrapOptions {
    std::filesystem::path plugin_directory;
    std::filesystem::path log_directory;
    uint32_t application_id = 0;
    std::string project_id;
    std::string engine_version = "HeavenBench-1.0.1";
};

struct DlssVulkanRequirements {
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    VkPhysicalDeviceVulkan12Features features12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features features13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    uint32_t extra_graphics_queues = 0;
    uint32_t extra_compute_queues = 0;
    uint32_t extra_optical_flow_queues = 0;
    bool hardware_scheduling_required = true;
    bool vsync_off_required = true;
};

struct DlssVulkanDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphics_family = 0;
    uint32_t graphics_queue_start = 0;
    uint32_t compute_family = 0;
    uint32_t compute_queue_start = 0;
    uint32_t optical_flow_family = 0;
    uint32_t optical_flow_queue_start = 0;
    VkDeviceQueueCreateFlags graphics_queue_flags = 0;
    VkDeviceQueueCreateFlags compute_queue_flags = 0;
    VkDeviceQueueCreateFlags optical_flow_queue_flags = 0;
    bool native_optical_flow = false;
    // The owner confirms every mandatory hook is installed, including surface
    // creation and destruction. Merely exporting hook pointers is insufficient.
    bool hooks_installed = false;
};

struct DlssImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags flags = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 1;
    uint32_t array_layers = 1;
};

// Matrices are unjittered, row-major, and use the same clip/depth convention as
// the tagged depth image. Motion is current-pixel -> previous-pixel displacement.
struct DlssCameraConstants {
    std::array<float, 16> view_to_clip{};
    std::array<float, 16> clip_to_view{};
    std::array<float, 16> clip_to_previous_clip{};
    std::array<float, 16> previous_clip_to_clip{};
    std::array<float, 2> jitter_pixels{};
    std::array<float, 2> motion_scale{1.0f, 1.0f};
    std::array<float, 3> position{};
    std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    std::array<float, 3> right{1.0f, 0.0f, 0.0f};
    std::array<float, 3> forward{0.0f, 0.0f, -1.0f};
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    float vertical_fov_radians = 1.0f;
    float aspect_ratio = 1.0f;
    bool depth_inverted = true;
    bool reset = true;
    bool motion_jittered = false;
};

struct DlssFrameToken {
    void* native_token = nullptr;
    uint64_t application_frame = 0;
    uint32_t stream_frame = 0;
};

enum class DlssLatencyMarker : uint8_t {
    SimulationStart, SimulationEnd, RenderSubmitStart, RenderSubmitEnd,
    PresentStart, PresentEnd, TriggerFlash, LatencyPing
};

struct DlssPresentInputs {
    DlssFrameToken token;
    DlssCameraConstants camera;
    DlssImage depth;
    DlssImage motion;
    DlssImage hudless_color;
    // Premultiplied UI RGB + alpha, with exact zero outside UI. Optional only
    // when the entire backbuffer is already HUDless.
    DlssImage ui_color_alpha;
    VkFormat backbuffer_format = VK_FORMAT_UNDEFINED;
    uint32_t backbuffer_width = 0;
    uint32_t backbuffer_height = 0;
    uint32_t backbuffer_count = 0;
    uint32_t frames_to_generate = 1;
    bool backbuffer_has_ui = false;
    bool vsync = false;
    bool hdr = false;
    // Must cover camera, moving instances, deformation, skinning and grass.
    // Setting this false refuses generation rather than presenting camera-only
    // motion as a complete temporal integration.
    bool temporal_coverage_complete = false;
    bool color_space_matches_backbuffer = false;
    bool inputs_valid_until_present = false;
    bool all_input_writes_on_presenting_queue = false;
};

struct DlssProviderStatus {
    bool built = false;
    bool initialized = false;
    bool available = false;
    bool enabled = false;
    uint32_t maximum_generated_frames = 0;
    // SDK-reported presentations since its last state query and their total.
    // These combine real/generated frames and are not OS display feedback.
    uint32_t last_sdk_presented_frames = 0;
    uint64_t total_sdk_presented_frames = 0;
    uint64_t estimated_vram_bytes = 0;
    uint32_t native_status = 0;
    std::string reason;
};

class DlssProvider final {
public:
    DlssProvider();
    ~DlssProvider();
    DlssProvider(const DlssProvider&) = delete;
    DlssProvider& operator=(const DlssProvider&) = delete;

    // Controller/driver methods below are serialized by the owner. Call
    // bootstrap before creating ANY Vulkan instance/device. No proprietary DLL
    // is shipped or downloaded by this class; the directory is explicit.
    bool bootstrap(const DlssBootstrapOptions& options);
    const DlssVulkanRequirements& requirements() const noexcept;

    // Only mandatory manual hooks return a proxy. All other names return null
    // and the caller retains its native Vulkan resolver. In particular instance
    // and device creation remain native, using requirements() injected by owner.
    PFN_vkVoidFunction instance_hook(VkInstance instance, const char* name) const;
    PFN_vkVoidFunction device_hook(VkDevice device, const char* name) const;
    // Optional SDK creation proxies inject requirements and record SDK queue
    // metadata internally. Use adopt_interposed_device for this route, never
    // combine it with manually injected requirements()/bind_device metadata.
    PFN_vkCreateInstance create_instance_proxy() const;
    PFN_vkCreateDevice create_device_proxy(VkInstance instance) const;
    static std::span<const char* const> mandatory_instance_hooks() noexcept;
    static std::span<const char* const> mandatory_device_hooks() noexcept;
    // Bind immediately after device creation, before any proxied surface or
    // swapchain creation. Feature function imports require this native device.
    bool bind_device(const DlssVulkanDevice& device);
    bool adopt_interposed_device(VkInstance instance, VkPhysicalDevice physical,
            VkDevice device, bool hooks_installed);

    // begin_frame and mark may run on the application/driver threads; use the
    // returned ticket unchanged across them. Sleep occurs before input sampling.
    // Do not call these concurrently with bootstrap/bind/shutdown.
    bool begin_frame(uint64_t application_frame, DlssFrameToken& token);
    bool mark(const DlssFrameToken& token, DlssLatencyMarker marker) const;
    uint32_t latency_ping_message() const noexcept;

    // Driver thread, outside a render pass and before the intercepted present.
    // SDK tags borrow images until present finishes consuming them. Default
    // queue blocking is retained; cross-queue reuse is refused by this adapter.
    bool prepare_present(const DlssPresentInputs& inputs);
    bool collect_state();
    bool suspend(const DlssFrameToken* token = nullptr);
    DlssProviderStatus status() const;

    // Before resize/reload: suspend, drain all client/backend/present work, then
    // release images and recreate the swapchain. Before Vulkan destruction:
    // suspend/drain, shutdown_after_drain, destroy engine/instance, destroy this
    // provider. The module stays loaded until the provider destructor because
    // borrowed Vulkan function pointers can outlive slShutdown.
    void shutdown_after_drain();

private:
    bool bind_device_impl(const DlssVulkanDevice& device, bool automatic_queues);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven
