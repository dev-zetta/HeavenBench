#ifndef HEAVEN_FRAME_GENERATION_PLATFORM_H
#define HEAVEN_FRAME_GENERATION_PLATFORM_H

#ifdef __ANDROID__
#include <backend/platforms/VulkanPlatformAndroid.h>
#elifdef _WIN32
#include <backend/platforms/VulkanPlatformWindows.h>
#elifdef __APPLE__
#include <backend/platforms/VulkanPlatformApple.h>
#else
#include <backend/platforms/VulkanPlatformLinux.h>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "frame_generation_present_trace.h"

namespace heaven {

#ifdef __ANDROID__
using FrameGenerationNativePlatform = filament::backend::VulkanPlatformAndroid;
#elifdef _WIN32
using FrameGenerationNativePlatform = filament::backend::VulkanPlatformWindows;
#elifdef __APPLE__
using FrameGenerationNativePlatform = filament::backend::VulkanPlatformApple;
#else
using FrameGenerationNativePlatform = filament::backend::VulkanPlatformLinux;
#endif

enum class FrameGenerationPresentEventKind : uint8_t {
    RealSubmitted, GeneratedSubmitted, SubmissionFailed, GenerationSkipped,
    HistoryInvalidated, NativeCaptureSaved, NativeCaptureFailed
};

struct FrameGenerationPresentEvent {
    FrameGenerationPresentEventKind kind = FrameGenerationPresentEventKind::GenerationSkipped;
    uint64_t source_frame = 0;
    VkResult result = VK_SUCCESS;
    std::string reason;
};

struct FrameGenerationPresentationStatus {
    bool available = false;
    bool armed = false;
    // Counters cover armed FG attempts, including their ordinary fallback.
    // Unarmed frames delegate directly and are counted by the application.
    uint64_t real_submissions = 0;
    uint64_t generated_submissions = 0;
    uint64_t failed_submissions = 0;
    uint64_t dropped_events = 0;
    uint64_t pacing_wait_ns = 0;
    std::string reason;
};

struct FrameGenerationCaptureImage {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Application-owned extension of Filament's native Vulkan swapchain policy.
// Ordinary frames delegate unchanged. An armed frame preserves the real image,
// presents a generated image, reacquires, then restores/presents the real image.
// No extra acquire occurs while the original native image is still held.
class FrameGenerationPlatform final : public FrameGenerationNativePlatform {
public:
    FrameGenerationPlatform();
    ~FrameGenerationPlatform() override;

    // Driver-thread calls, serialized with acquire/present. The image is GENERAL,
    // transfer-readable, and written on this platform's graphics queue. Its
    // owner retains it through queue completion before resize/destruction.
    // interval_ns is an unpaced real-render interval (0 disables pacing), capped
    // at 100ms; feeding this bridge's own waits back into it causes drift.
    bool arm_generated(VkImage output, VkFormat format, VkExtent2D extent,
            uint64_t source_frame, uint64_t interval_ns);
    // Track one ordinary/warmup real present without extra GPU work.
    bool arm_real(uint64_t source_frame);
    void disarm_generated();
    // Explicit diagnostics only. Record the measured generated/UI inputs before
    // UI composition, restoring GENERAL for both ordinary compute consumers.
    // Commit only after this source is armed; discarded native callbacks abandon
    // the metadata while retaining GPU-referenced storage until queue draining.
    bool wants_ui_capture(uint64_t source_frame) const noexcept;
    bool record_ui_inputs(VkCommandBuffer command,
            const FrameGenerationCaptureImage& generated,
            const FrameGenerationCaptureImage& ui, uint64_t source_frame) noexcept;
    void commit_ui_inputs(uint64_t source_frame) noexcept;
    void abandon_ui_inputs(uint64_t source_frame = 0) noexcept;
    // Driver thread, before borrowed output is retired. No per-frame idle wait.
    void drain_generated();

    // Thread-safe snapshots/event consumption. Successful native presents mean
    // accepted queue submissions; they do not prove display scanout.
    FrameGenerationPresentationStatus presentation_status() const;
    std::vector<FrameGenerationPresentEvent> take_presentation_events();
    // Immutable opt-in flag; querying it creates no runtime or GPU resources.
    bool presentation_trace_requested() const noexcept;
    // Independent of application event consumption. Native callbacks latch
    // this immediately before interpolation after an implicit recreation.
    uint64_t history_epoch() const noexcept;
    // A failed native chain remains real-only until successful recreation.
    // This is separate from lazy initial presentation capability discovery.
    bool generation_blocked() const noexcept;
#if defined(HEAVEN_FG_FAULT_INJECTION) && HEAVEN_FG_FAULT_INJECTION
    // Native smoke only: the next outer acquire follows the ordinary
    // OUT_OF_DATE retry/recreation path before any image is acquired.
    void debug_out_of_date_once() noexcept;
    // Native smoke only: fail one generated command-buffer begin before any
    // command recording, exercising the existing broken-chain fallback.
    void debug_generated_work_failure_once() noexcept;
    uint64_t debug_generated_work_failure_source() const noexcept;
#endif

    Customization getCustomization() const noexcept override;

    SwapChainPtr createSwapChain(void* nativeWindow, uint64_t flags = 0,
            VkExtent2D extent = {0, 0}) override;
    VkResult acquire(SwapChainPtr handle, ImageSyncData* sync) override;
    VkResult present(SwapChainPtr handle, uint32_t index,
            VkSemaphore finishedDrawing) override;
    VkResult recreate(SwapChainPtr handle) override;
    void destroy(SwapChainPtr handle) override;
    void terminate() override;

protected:
    VkDevice createVkDevice(const VkDeviceCreateInfo& createInfo) noexcept override;

private:
    VkResult present_traced(SwapChainPtr handle, uint32_t index,
            VkSemaphore finishedDrawing, uint64_t source_frame, PresentTraceKind kind);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven

#endif // HEAVEN_FRAME_GENERATION_PLATFORM_H
