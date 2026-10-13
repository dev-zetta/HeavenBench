#ifndef HEAVEN_FSR31_VULKAN_H
#define HEAVEN_FSR31_VULKAN_H

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace heaven {

struct Fsr31VulkanImage {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    // Successful dispatch restores each borrowed image to this exact layout.
    // GENERAL and SHADER_READ_ONLY_OPTIMAL are supported for sampled inputs;
    // generated output must enter in GENERAL.
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
};

enum class Fsr31TransferFunction { Srgb, Pq, LinearScRgb };

struct Fsr31VulkanConfig {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
    VkExtent2D max_render_size{};
    VkExtent2D display_size{};
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    bool inverted_depth = false;
    bool infinite_depth = false;
    bool jittered_motion_vectors = false;
    bool hdr_color = false;
    // Vulkan exposes supported features, but cannot query enabled features on
    // an existing logical device. The device owner must attest these enables.
    bool storage_image_extended_formats_enabled = false;
    bool storage_image_read_without_format_enabled = false;
    bool storage_image_write_without_format_enabled = false;
};

struct Fsr31VulkanFrame {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    Fsr31VulkanImage depth;
    Fsr31VulkanImage motion_vectors;
    Fsr31VulkanImage hudless_color;
    Fsr31VulkanImage generated_output;
    uint64_t frame_id = 0;
    bool reset = false;
    float frame_time_ms = 0.0f;
    // Exact rendered projection limits, independent of the culling frustum.
    float camera_near = 0.1f;
    // Used only for finite projection. Infinite projection is canonicalized
    // to the largest finite float before calling the SDK.
    float camera_far = 1000.0f;
    float vertical_fov_radians = 1.0f;
    float view_units_to_meters = 1.0f;
    std::array<float, 2> jitter_pixels{};
    // SDK motion vectors point from current to previous. Pixel vectors use
    // {1,1}; normalized UV vectors use {render_width, render_height}.
    std::array<float, 2> motion_vector_scale{1.0f, 1.0f};
    std::array<float, 3> camera_position{};
    std::array<float, 3> camera_up{0.0f, 1.0f, 0.0f};
    std::array<float, 3> camera_right{1.0f, 0.0f, 0.0f};
    std::array<float, 3> camera_forward{0.0f, 0.0f, -1.0f};
    Fsr31TransferFunction transfer_function = Fsr31TransferFunction::Srgb;
    float min_luminance = 0.0f;
    float max_luminance = 1000.0f;
};

// A synchronous compute provider. The caller owns submission, presentation,
// pacing and HUD composition; no CPU readback or surrogate interpolation is
// used. Calls belong on the Vulkan backend thread, outside any render pass.
// The owning queue must be drained before destruction or replacement. Before
// provider record R (R >= 4), GPU work through record R-3 must have completed:
// final Dispatch advances the four-slot view ring and retires record R-3.
// One record means one Prepare + optical-flow + Dispatch sequence, including
// reset records; frame IDs, history invalidation and discard are not completion.
// Filament's default latency2 admission is stricter on successful Vulkan fences.
// Standalone callers must enforce this completion bound themselves.
class Fsr31Vulkan {
public:
    static std::expected<std::unique_ptr<Fsr31Vulkan>, std::string> create(const Fsr31VulkanConfig& config);
    ~Fsr31Vulkan();
    Fsr31Vulkan(const Fsr31Vulkan&) = delete;
    Fsr31Vulkan& operator=(const Fsr31Vulkan&) = delete;

    // True means history permits an intermediate present; reset/first frames
    // still dispatch to initialize history but must present only the real frame.
    // On failure discard this command buffer. A partially recorded provider
    // must be drained/recreated; invalidating content history does not retire views.
    std::expected<bool, std::string> record(const Fsr31VulkanFrame& frame);
    void invalidate_history() noexcept;
    uint64_t gpu_memory_bytes() const noexcept;
    static constexpr const char* sdk_commit() noexcept { return "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55"; }
    static constexpr const char* version() noexcept { return "FSR 3.1.4 / FidelityFX SDK 1.1.4"; }

private:
    struct Impl;
    Fsr31Vulkan();
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven

#endif // HEAVEN_FSR31_VULKAN_H
