#include "dlss_platform.h"

#include <atomic>
#include <mutex>

namespace heaven {
namespace {
using Base = DlssNativePlatform;
using namespace bluevk;
}

struct DlssPlatform::Impl {
    DlssProvider provider;
    mutable std::mutex mutex;
    DlssProviderStatus snapshot;
    DlssSwapchainDescription swapchain;
    SwapChainPtr surface_chain = nullptr;
    uint64_t surface_flags = 0;
    DlssFrameToken pending;
    bool bootstrapped = false;
    bool creation_enabled = false;
    bool creation_failed = false;
    VkResult creation_result = VK_SUCCESS;
    std::atomic<bool> bound{false};
#ifdef _WIN32
    struct Hooks {
        PFN_vkCreateWin32SurfaceKHR create_surface{};
        PFN_vkDestroySurfaceKHR destroy_surface{};
        PFN_vkCreateSwapchainKHR create_swapchain{};
        PFN_vkDestroySwapchainKHR destroy_swapchain{};
        PFN_vkGetSwapchainImagesKHR images{};
        PFN_vkAcquireNextImageKHR acquire{};
        PFN_vkQueuePresentKHR present{};
        PFN_vkDeviceWaitIdle device_idle{};
    } original, proxy;
    bool hooks_installed = false;

    bool install(VkInstance instance, VkDevice device) {
        proxy.create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(provider.instance_hook(instance, "vkCreateWin32SurfaceKHR"));
        proxy.destroy_surface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(provider.instance_hook(instance, "vkDestroySurfaceKHR"));
        proxy.create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(provider.device_hook(device, "vkCreateSwapchainKHR"));
        proxy.destroy_swapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(provider.device_hook(device, "vkDestroySwapchainKHR"));
        proxy.images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(provider.device_hook(device, "vkGetSwapchainImagesKHR"));
        proxy.acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(provider.device_hook(device, "vkAcquireNextImageKHR"));
        proxy.present = reinterpret_cast<PFN_vkQueuePresentKHR>(provider.device_hook(device, "vkQueuePresentKHR"));
        proxy.device_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(provider.device_hook(device, "vkDeviceWaitIdle"));
        if (!proxy.create_surface || !proxy.destroy_surface || !proxy.create_swapchain ||
                !proxy.destroy_swapchain || !proxy.images || !proxy.acquire ||
                !proxy.present || !proxy.device_idle) return false;
        original = {vkCreateWin32SurfaceKHR, vkDestroySurfaceKHR, vkCreateSwapchainKHR,
            vkDestroySwapchainKHR, vkGetSwapchainImagesKHR, vkAcquireNextImageKHR,
            vkQueuePresentKHR, vkDeviceWaitIdle};
        vkCreateWin32SurfaceKHR = proxy.create_surface;
        vkDestroySurfaceKHR = proxy.destroy_surface;
        vkCreateSwapchainKHR = proxy.create_swapchain;
        vkDestroySwapchainKHR = proxy.destroy_swapchain;
        vkGetSwapchainImagesKHR = proxy.images;
        vkAcquireNextImageKHR = proxy.acquire;
        vkQueuePresentKHR = proxy.present;
        vkDeviceWaitIdle = proxy.device_idle;
        hooks_installed = true;
        return true;
    }

    void restore() {
        if (!hooks_installed) return;
        // Do not overwrite another owner's later binding. Concurrent Vulkan
        // engines are unsupported while process-global Streamline is active.
#define HEAVEN_RESTORE_VK(name, member) if (name == proxy.member) name = original.member
        HEAVEN_RESTORE_VK(vkCreateWin32SurfaceKHR, create_surface);
        HEAVEN_RESTORE_VK(vkDestroySurfaceKHR, destroy_surface);
        HEAVEN_RESTORE_VK(vkCreateSwapchainKHR, create_swapchain);
        HEAVEN_RESTORE_VK(vkDestroySwapchainKHR, destroy_swapchain);
        HEAVEN_RESTORE_VK(vkGetSwapchainImagesKHR, images);
        HEAVEN_RESTORE_VK(vkAcquireNextImageKHR, acquire);
        HEAVEN_RESTORE_VK(vkQueuePresentKHR, present);
        HEAVEN_RESTORE_VK(vkDeviceWaitIdle, device_idle);
#undef HEAVEN_RESTORE_VK
        hooks_installed = false;
    }
#endif

    void publish(std::string reason = {}) {
        auto state = provider.status();
        if (!reason.empty()) { state.available = false; state.enabled = false; state.reason = std::move(reason); }
        std::scoped_lock lock(mutex);
        snapshot = std::move(state);
    }

    void publish_swapchain(const Base::SwapChainBundle& bundle, bool surface, uint64_t flags) {
        std::scoped_lock lock(mutex);
        ++swapchain.epoch;
        swapchain.format = bundle.colorFormat;
        swapchain.extent = bundle.extent;
        swapchain.image_count = static_cast<uint32_t>(bundle.colors.size());
        swapchain.native_surface = surface;
        swapchain.protected_content = bundle.isProtected;
        swapchain.hdr = (flags & filament::backend::SWAP_CHAIN_CONFIG_HDR10_ST2084) != 0;
        swapchain.vsync = bundle.presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
};

DlssPlatform::DlssPlatform(const DlssBootstrapOptions& options) : impl_(std::make_unique<Impl>()) {
    impl_->bootstrapped = impl_->provider.bootstrap(options);
    impl_->creation_enabled = impl_->bootstrapped;
    impl_->publish();
}

DlssPlatform::~DlssPlatform() {
    // Engine must have called terminate first, while its device still existed.
#ifdef _WIN32
    impl_->restore();
#endif
}

VkInstance DlssPlatform::createVkInstance(const VkInstanceCreateInfo& info) noexcept {
    if (!impl_->creation_enabled) return Base::createVkInstance(info);
    const auto create = impl_->provider.create_instance_proxy();
    VkInstance instance = VK_NULL_HANDLE;
    const auto result = create ? create(&info, nullptr, &instance) : VK_ERROR_INITIALIZATION_FAILED;
    if (result == VK_SUCCESS && instance) return instance;
    impl_->creation_failed = true;
    impl_->creation_enabled = false;
    impl_->creation_result = result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
    return Base::createVkInstance(info);
}

VkDevice DlssPlatform::createVkDevice(const VkDeviceCreateInfo& info) noexcept {
    if (!impl_->creation_enabled) return Base::createVkDevice(info);
    const auto create = impl_->provider.create_device_proxy(getInstance());
    VkDevice device = VK_NULL_HANDLE;
    const auto result = create ? create(getPhysicalDevice(), &info, nullptr, &device) : VK_ERROR_INITIALIZATION_FAILED;
    if (result == VK_SUCCESS && device) return device;
    impl_->creation_failed = true;
    impl_->creation_enabled = false;
    impl_->creation_result = result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
    return Base::createVkDevice(info);
}

filament::backend::Driver* DlssPlatform::createDriver(void* sharedContext,
        const filament::backend::Platform::DriverConfig& config) {
    if (sharedContext) impl_->creation_enabled = false;
    auto* driver = Base::createDriver(sharedContext, config);
#ifdef _WIN32
    if (impl_->bootstrapped && !sharedContext && !impl_->creation_failed &&
            impl_->install(getInstance(), getDevice()) &&
            impl_->provider.adopt_interposed_device(getInstance(), getPhysicalDevice(), getDevice(), true)) {
        impl_->bound.store(true, std::memory_order_release);
        impl_->publish();
        return driver;
    }
    impl_->restore();
#endif
    std::string reason;
    if (sharedContext) reason = "DLSS cannot prove precreation requirements for an external shared Vulkan context";
    else if (impl_->creation_failed) reason = "Streamline Vulkan context creation failed; native fallback active: " +
        std::to_string(impl_->creation_result);
    else reason = impl_->provider.status().reason;
    // No surface or swapchain has been created, so unsuccessful optional
    // bootstrap can retire safely and leave the ordinary native engine usable.
    if (getDevice()) vkDeviceWaitIdle(getDevice());
    impl_->provider.shutdown_after_drain();
    impl_->publish(std::move(reason));
    return driver;
}

bool DlssPlatform::begin_frame(uint64_t application_frame, DlssFrameToken& token) {
    if (!impl_->bound.load(std::memory_order_acquire)) { token = {}; return false; }
    return impl_->provider.begin_frame(application_frame, token);
}
bool DlssPlatform::mark(const DlssFrameToken& token, DlssLatencyMarker marker) const {
    return impl_->bound.load(std::memory_order_acquire) && impl_->provider.mark(token, marker);
}
uint32_t DlssPlatform::latency_ping_message() const noexcept {
    return impl_->bound.load(std::memory_order_acquire) ? impl_->provider.latency_ping_message() : 0;
}
bool DlssPlatform::prepare_present(const DlssPresentInputs& inputs) {
    if (!impl_->bound.load(std::memory_order_acquire)) return false;
    impl_->pending = inputs.token;
    const bool ready = impl_->provider.prepare_present(inputs);
    impl_->publish();
    return ready;
}
void DlssPlatform::suspend_generation() {
    impl_->provider.suspend();
    impl_->pending = {};
    impl_->publish();
}
DlssProviderStatus DlssPlatform::provider_status() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->snapshot;
}
DlssSwapchainDescription DlssPlatform::swapchain_description() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->swapchain;
}

DlssPlatform::SwapChainPtr DlssPlatform::createSwapChain(void* nativeWindow,
        uint64_t flags, VkExtent2D extent) {
    const auto handle = Base::createSwapChain(nativeWindow, flags, extent);
    if (handle && nativeWindow && extent.width == 0 && extent.height == 0) {
        impl_->surface_chain = handle;
        impl_->surface_flags = flags;
        impl_->publish_swapchain(Base::getSwapChainBundle(handle), true, flags);
    }
    return handle;
}

VkResult DlssPlatform::present(SwapChainPtr handle, uint32_t index, VkSemaphore finished) {
    if (!impl_->bound.load(std::memory_order_acquire)) return Base::present(handle, index, finished);
    const auto token = impl_->pending;
    impl_->pending = {};
    if (token.native_token) impl_->provider.mark(token, DlssLatencyMarker::PresentStart);
    else impl_->provider.suspend(); // Never reuse stale constants or temporal images.
    const auto result = Base::present(handle, index, finished);
    if (token.native_token) impl_->provider.mark(token, DlssLatencyMarker::PresentEnd);
    impl_->provider.collect_state();
    impl_->publish();
    return result;
}

VkResult DlssPlatform::recreate(SwapChainPtr handle) {
    suspend_generation();
    if (getDevice()) vkDeviceWaitIdle(getDevice());
    const auto result = Base::recreate(handle);
    if (handle == impl_->surface_chain) {
        if (result == VK_SUCCESS)
            impl_->publish_swapchain(Base::getSwapChainBundle(handle), true, impl_->surface_flags);
        else impl_->publish_swapchain({}, false, impl_->surface_flags);
    }
    return result;
}
void DlssPlatform::destroy(SwapChainPtr handle) {
    suspend_generation();
    if (getDevice()) vkDeviceWaitIdle(getDevice());
    Base::destroy(handle);
    if (handle == impl_->surface_chain) {
        impl_->surface_chain = nullptr;
        std::scoped_lock lock(impl_->mutex);
        const auto epoch = impl_->swapchain.epoch + 1;
        impl_->swapchain = {};
        impl_->swapchain.epoch = epoch;
    }
}
void DlssPlatform::terminate() {
    impl_->bound.store(false, std::memory_order_release);
    suspend_generation();
    if (getDevice()) vkDeviceWaitIdle(getDevice());
    impl_->provider.shutdown_after_drain();
#ifdef _WIN32
    impl_->restore();
#endif
    impl_->publish();
    impl_->publish_swapchain({}, false, 0);
    Base::terminate();
}

} // namespace heaven
