#ifndef HEAVEN_DLSS_PLATFORM_H
#define HEAVEN_DLSS_PLATFORM_H

#ifdef __ANDROID__
#include <backend/platforms/VulkanPlatformAndroid.h>
#else
#ifdef _WIN32
#include <backend/platforms/VulkanPlatformWindows.h>
#else
#ifdef __APPLE__
#include <backend/platforms/VulkanPlatformApple.h>
#else
#include <backend/platforms/VulkanPlatformLinux.h>
#endif
#endif
#endif
#include "dlss_provider.h"

#include <memory>

namespace heaven {

#ifdef __ANDROID__
using DlssNativePlatform = filament::backend::VulkanPlatformAndroid;
#else
#ifdef _WIN32
using DlssNativePlatform = filament::backend::VulkanPlatformWindows;
#else
#ifdef __APPLE__
using DlssNativePlatform = filament::backend::VulkanPlatformApple;
#else
using DlssNativePlatform = filament::backend::VulkanPlatformLinux;
#endif
#endif
#endif

struct DlssSwapchainDescription {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t image_count = 0;
    uint64_t epoch = 0;
    bool native_surface = false;
    bool protected_content = false;
    bool hdr = false;
    // Unknown modes are conservatively rejected along with synchronized modes.
    bool vsync = true;
};

// Separate from the portable FSR presentation bridge. On Windows, Streamline
// owns frame-generation acquire/present behavior via its official proxies.
class DlssPlatform final : public DlssNativePlatform {
public:
    // Must be constructed before Engine, hence before any Vulkan APIs. Failed
    // optional initialization preserves the ordinary native platform behavior.
    explicit DlssPlatform(const DlssBootstrapOptions& options);
    ~DlssPlatform() override;

    filament::backend::Driver* createDriver(void* sharedContext,
            const filament::backend::Platform::DriverConfig& config) override;
    VkResult present(SwapChainPtr handle, uint32_t index, VkSemaphore finished) override;
    SwapChainPtr createSwapChain(void* nativeWindow, uint64_t flags = 0,
            VkExtent2D extent = {0, 0}) override;
    VkResult recreate(SwapChainPtr handle) override;
    void destroy(SwapChainPtr handle) override;
    void terminate() override;

    // begin_frame/mark follow the same threading contract as DlssProvider.
    bool begin_frame(DlssFrameToken& token);
    bool mark(const DlssFrameToken& token, DlssLatencyMarker marker) const;
    uint32_t latency_ping_message() const noexcept;
    // Serialized driver-thread native callback before renderer.endFrame.
    bool prepare_present(const DlssPresentInputs& inputs);
    void suspend_generation();
    // Serialized driver thread, only before retiring tagged image resources.
    // The SDK's device-idle proxy drains asynchronous presentation work that
    // Filament's ordinary graphics-queue wait does not own.
    [[nodiscard]] VkResult drain_generation();
    // Snapshot is safe for application/UI access while driver callbacks run.
    DlssProviderStatus provider_status() const;
    DlssSwapchainDescription swapchain_description() const;

protected:
    VkInstance createVkInstance(const VkInstanceCreateInfo& info) noexcept override;
    VkDevice createVkDevice(const VkDeviceCreateInfo& info) noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven

#endif // HEAVEN_DLSS_PLATFORM_H
