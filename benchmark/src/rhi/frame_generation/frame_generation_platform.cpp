#include "frame_generation_platform.h"
#include "frame_generation_capture.h"
#include "frame_generation_history.h"

#include <algorithm>
#include <array>
#if defined(HEAVEN_FG_FAULT_INJECTION) && HEAVEN_FG_FAULT_INJECTION
#include <atomic>
#endif
#include <chrono>
#include <cstdio>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace heaven {
namespace {
using Base = FrameGenerationNativePlatform;
using namespace bluevk;
using Clock = std::chrono::steady_clock;

bool submitted(VkResult result) { return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR; }

void transition(VkCommandBuffer command, VkImage image, VkImageLayout from,
        VkImageLayout to, VkAccessFlags source, VkAccessFlags destination,
        VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source;
    barrier.dstAccessMask = destination;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, source_stage, destination_stage, 0,
        0, nullptr, 0, nullptr, 1, &barrier);
}

VkImageCopy copy_region(VkExtent2D extent) {
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {extent.width, extent.height, 1};
    return region;
}
} // namespace

struct FrameGenerationPlatform::Impl {
    struct ImageWork {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkFence complete = VK_NULL_HANDLE;
        VkSemaphore present_ready = VK_NULL_HANDLE;
    };
    struct Chain {
        Base::SwapChainBundle bundle;
        std::vector<ImageWork> work;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkImage real_backup = VK_NULL_HANDLE;
        VkDeviceMemory backup_memory = VK_NULL_HANDLE;
        bool backup_initialized = false;
        bool surface = false;
        bool broken = false;
        Clock::time_point last_real_present;
    };
    struct Armed {
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        uint64_t source = 0;
        uint64_t interval_ns = 0;
    } armed;

    std::unordered_map<SwapChainPtr, Chain> chains;
    mutable std::mutex status_mutex;
    FrameGenerationPresentationStatus status;
    FrameGenerationHistoryEpoch history;
#if defined(HEAVEN_FG_FAULT_INJECTION) && HEAVEN_FG_FAULT_INJECTION
    std::atomic<bool> out_of_date_once{false};
#endif
    std::deque<FrameGenerationPresentEvent> events;
    FrameGenerationCapture capture;

    void capture_failed(uint64_t source, const char* reason) noexcept {
        const auto* message = reason && *reason ? reason : "Native diagnostic capture failed";
        std::fprintf(stderr, "Frame generation capture failed: %s\n", message);
        // Diagnostic allocation or reporting failure must not poison the
        // interpolation provider or discard its successfully recorded work.
        try {
            event(FrameGenerationPresentEventKind::NativeCaptureFailed, source, VK_NOT_READY, message);
        } catch (...) {
            std::fputs("Frame generation capture failure event could not be retained\n", stderr);
        }
    }

    void event(FrameGenerationPresentEventKind kind, uint64_t source, VkResult result,
            std::string reason = {}) {
        if (kind == FrameGenerationPresentEventKind::SubmissionFailed) {
            history.invalidate();
            if (reason.empty()) reason = "Native presentation failed: VkResult " + std::to_string(result);
        }
        std::scoped_lock lock(status_mutex);
        if (kind == FrameGenerationPresentEventKind::RealSubmitted) ++status.real_submissions;
        if (kind == FrameGenerationPresentEventKind::GeneratedSubmitted) ++status.generated_submissions;
        if (kind == FrameGenerationPresentEventKind::SubmissionFailed) {
            ++status.failed_submissions;
            status.available = false;
        }
        if (!reason.empty()) status.reason = reason;
        constexpr size_t maximum_events = 256;
        if (events.size() == maximum_events) { events.pop_front(); ++status.dropped_events; }
        events.push_back({kind, source, result, std::move(reason)});
    }

    void set_availability(bool available, std::string reason) {
        std::scoped_lock lock(status_mutex);
        status.available = available;
        status.reason = std::move(reason);
    }

    void clear_armed() {
        armed = {};
        std::scoped_lock lock(status_mutex);
        status.armed = false;
    }

    void release_chain(VkDevice device, Chain& chain) {
        for (auto& work : chain.work) {
            if (work.complete) vkDestroyFence(device, work.complete, nullptr);
            if (work.present_ready) vkDestroySemaphore(device, work.present_ready, nullptr);
        }
        if (chain.pool) vkDestroyCommandPool(device, chain.pool, nullptr);
        if (chain.real_backup) vkDestroyImage(device, chain.real_backup, nullptr);
        if (chain.backup_memory) vkFreeMemory(device, chain.backup_memory, nullptr);
        chain.work.clear();
        chain.pool = VK_NULL_HANDLE;
        chain.real_backup = VK_NULL_HANDLE;
        chain.backup_memory = VK_NULL_HANDLE;
        chain.backup_initialized = false;
    }

    bool prepare_chain(FrameGenerationPlatform& platform, Chain& chain) {
        // Keep the actual submission error until successful recreation, rather
        // than replacing it with an unrelated surface-capability explanation.
        if (chain.broken) return false;
        if (!chain.surface || chain.bundle.isProtected ||
                chain.bundle.layerCount != 1 || chain.bundle.colors.empty()) {
            set_availability(false, "Generated presentation requires an unprotected native surface swapchain");
            return false;
        }
        if (armed.extent.width != chain.bundle.extent.width ||
                armed.extent.height != chain.bundle.extent.height) {
            set_availability(false, "Generated output and native swapchain extents differ");
            return false;
        }
        if ((armed.format != VK_FORMAT_R8G8B8A8_UNORM && armed.format != VK_FORMAT_B8G8R8A8_UNORM) ||
                (chain.bundle.colorFormat != VK_FORMAT_R8G8B8A8_UNORM &&
                 chain.bundle.colorFormat != VK_FORMAT_B8G8R8A8_UNORM)) {
            set_availability(false, "Generated presentation requires SDR RGBA8/BGRA8 UNORM images");
            return false;
        }
        VkFormatProperties source{}, destination{};
        vkGetPhysicalDeviceFormatProperties(platform.getPhysicalDevice(), armed.format, &source);
        vkGetPhysicalDeviceFormatProperties(platform.getPhysicalDevice(), chain.bundle.colorFormat, &destination);
        if (armed.format != chain.bundle.colorFormat &&
                (!(source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
                 !(destination.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))) {
            set_availability(false, "Native formats do not support generated-output transfer conversion");
            return false;
        }
        if (!chain.work.empty()) {
            set_availability(true, "Native generated-frame presentation is ready");
            return true;
        }
        const auto device = platform.getDevice();
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = platform.getGraphicsQueueFamilyIndex();
        VkResult result = vkCreateCommandPool(device, &pool, nullptr, &chain.pool);
        if (result != VK_SUCCESS) return allocation_failed(device, chain, result);
        chain.work.resize(chain.bundle.colors.size());
        for (auto& work : chain.work) {
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool = chain.pool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            result = vkAllocateCommandBuffers(device, &allocation, &work.command);
            if (result != VK_SUCCESS)
                return allocation_failed(device, chain, result);
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vkCreateFence(device, &fence, nullptr, &work.complete);
            if (result != VK_SUCCESS)
                return allocation_failed(device, chain, result);
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            result = vkCreateSemaphore(device, &semaphore, nullptr, &work.present_ready);
            if (result != VK_SUCCESS)
                return allocation_failed(device, chain, result);
        }
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = chain.bundle.colorFormat;
        image.extent = {chain.bundle.extent.width, chain.bundle.extent.height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        result = vkCreateImage(device, &image, nullptr, &chain.real_backup);
        if (result != VK_SUCCESS)
            return allocation_failed(device, chain, result);
        VkMemoryRequirements required{};
        vkGetImageMemoryRequirements(device, chain.real_backup, &required);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(platform.getPhysicalDevice(), &memory);
        uint32_t memory_type = UINT32_MAX;
        for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
            if ((required.memoryTypeBits & (1u << type)) &&
                    (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                    !(memory.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
                        VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD))) {
                memory_type = type; break;
            }
        }
        if (memory_type == UINT32_MAX)
            return allocation_failed(device, chain, VK_ERROR_FEATURE_NOT_PRESENT);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = required.size;
        allocation.memoryTypeIndex = memory_type;
        result = vkAllocateMemory(device, &allocation, nullptr, &chain.backup_memory);
        if (result != VK_SUCCESS) return allocation_failed(device, chain, result);
        result = vkBindImageMemory(device, chain.real_backup, chain.backup_memory, 0);
        if (result != VK_SUCCESS) return allocation_failed(device, chain, result);
        set_availability(true, "Native generated-frame presentation is ready");
        return true;
    }

    bool allocation_failed(VkDevice device, Chain& chain, VkResult result) {
        release_chain(device, chain);
        chain.broken = true;
        set_availability(false, "Generated presentation resource allocation failed: " + std::to_string(result));
        return false;
    }

    VkResult begin_work(VkDevice device, ImageWork& work) {
        // Reacquiring this native image proves the presentation engine consumed
        // its previous wait semaphore. The fence separately protects command
        // buffer reuse; an unrelated frame counter is never used as proof.
        auto result = vkWaitForFences(device, 1, &work.complete, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) return result;
        result = vkResetCommandBuffer(work.command, 0);
        if (result != VK_SUCCESS) return result;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(work.command, &begin);
    }

    VkResult submit_work(FrameGenerationPlatform& platform, ImageWork& work, VkSemaphore wait) {
        auto result = vkEndCommandBuffer(work.command);
        if (result != VK_SUCCESS) return result;
        result = vkResetFences(platform.getDevice(), 1, &work.complete);
        if (result != VK_SUCCESS) return result;
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = wait ? 1u : 0u;
        submit.pWaitSemaphores = wait ? &wait : nullptr;
        submit.pWaitDstStageMask = wait ? &wait_stage : nullptr;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &work.command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &work.present_ready;
        return vkQueueSubmit(platform.getGraphicsQueue(), 1, &submit, work.complete);
    }

    void pace(Clock::time_point target) {
        const auto before = Clock::now();
        if (target <= before) return;
        std::this_thread::sleep_until(target);
        const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - before).count();
        std::scoped_lock lock(status_mutex);
        status.pacing_wait_ns += static_cast<uint64_t>(waited);
    }
};

FrameGenerationPlatform::FrameGenerationPlatform() : impl_(std::make_unique<Impl>()) {
    const auto error = impl_->capture.last_error();
    if (!error.empty()) impl_->capture_failed(0, error.c_str());
}
FrameGenerationPlatform::~FrameGenerationPlatform() = default;

FrameGenerationPlatform::Customization FrameGenerationPlatform::getCustomization() const noexcept {
    auto result = Base::getCustomization();
    result.enableFrameGenerationStorageImageFeatures = true;
    return result;
}

bool FrameGenerationPlatform::arm_generated(VkImage output, VkFormat format, VkExtent2D extent,
        uint64_t source_frame, uint64_t interval_ns) {
    if (!output || format == VK_FORMAT_UNDEFINED || extent.width == 0 || extent.height == 0 ||
            source_frame == 0 || !getDevice()) {
        disarm_generated();
        impl_->set_availability(false, "Generated native image description or Vulkan device is unavailable");
        return false;
    }
    if (impl_->armed.image)
        impl_->event(FrameGenerationPresentEventKind::GenerationSkipped, impl_->armed.source,
            VK_NOT_READY, "Unpresented generated output was replaced");
    if (impl_->armed.source && impl_->armed.source != source_frame)
        impl_->capture.abandon_ui_inputs(impl_->armed.source);
    impl_->armed = {output, format, extent, source_frame, std::min(interval_ns, uint64_t{100'000'000})};
    std::scoped_lock lock(impl_->status_mutex);
    impl_->status.armed = true;
    return true;
}

bool FrameGenerationPlatform::arm_real(uint64_t source_frame) {
    impl_->capture.abandon_ui_inputs();
    if (source_frame == 0) { disarm_generated(); return false; }
    if (impl_->armed.image)
        impl_->event(FrameGenerationPresentEventKind::GenerationSkipped, impl_->armed.source,
            VK_NOT_READY, "Generated output was replaced by a real-only presentation");
    impl_->armed = {};
    impl_->armed.source = source_frame;
    std::scoped_lock lock(impl_->status_mutex);
    impl_->status.armed = true;
    return true;
}

void FrameGenerationPlatform::disarm_generated() {
    impl_->capture.abandon_ui_inputs();
    impl_->clear_armed();
}

bool FrameGenerationPlatform::wants_ui_capture(uint64_t source_frame) const noexcept {
    return impl_->capture.ui_inputs_enabled() && impl_->capture.wants_source(source_frame);
}

bool FrameGenerationPlatform::record_ui_inputs(VkCommandBuffer command,
        const FrameGenerationCaptureImage& generated,
        const FrameGenerationCaptureImage& ui, uint64_t source_frame) noexcept {
    if (!wants_ui_capture(source_frame)) return false;
    // All validation and allocation precede layout changes. Unsupported
    // diagnostic inputs leave the actual interpolation/composition untouched.
    const char* invalid = nullptr;
    if (!command || !getDevice() || !getPhysicalDevice() || !generated.image || !ui.image)
        invalid = "Pre-UI capture requires valid Vulkan commands, device and images";
    else if (generated.image == ui.image)
        invalid = "Pre-UI capture requires distinct generated and UI images";
    else if (generated.format != VK_FORMAT_R8G8B8A8_UNORM || ui.format != VK_FORMAT_R8G8B8A8_UNORM)
        invalid = "Pre-UI capture requires actual RGBA8_UNORM generated and UI images";
    else if (!generated.extent.width || !generated.extent.height ||
            generated.extent.width != ui.extent.width || generated.extent.height != ui.extent.height)
        invalid = "Pre-UI capture requires equal nonzero generated and UI extents";
    else if (!(generated.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
            !(ui.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        invalid = "Pre-UI capture images lack native TRANSFER_SRC usage";
    else if (generated.layout != VK_IMAGE_LAYOUT_GENERAL || ui.layout != VK_IMAGE_LAYOUT_GENERAL)
        invalid = "Pre-UI capture requires generated and UI images in GENERAL layout";
    if (invalid) {
        impl_->capture.abandon_ui_inputs(source_frame);
        impl_->capture_failed(source_frame, invalid);
        return false;
    }
    try {
        if (!impl_->capture.prepare_ui_inputs(getDevice(), getPhysicalDevice(), generated.extent,
                generated.format, ui.format, source_frame)) {
            impl_->capture.abandon_ui_inputs(source_frame);
            impl_->capture_failed(source_frame, impl_->capture.last_error().c_str());
            return false;
        }
    } catch (const std::exception& error) {
        impl_->capture.abandon_ui_inputs(source_frame);
        impl_->capture_failed(source_frame, error.what());
        return false;
    } catch (...) {
        impl_->capture.abandon_ui_inputs(source_frame);
        impl_->capture_failed(source_frame, "Pre-UI capture preparation failed");
        return false;
    }
    transition(command, generated.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    transition(command, ui.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    bool recorded = false;
    try {
        recorded = impl_->capture.record_ui_inputs(command, generated.image, ui.image);
    } catch (...) {
        // Restore both borrowed layouts even if diagnostic recording fails.
        recorded = false;
    }
    transition(command, generated.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    transition(command, ui.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (!recorded) {
        impl_->capture.abandon_ui_inputs(source_frame);
        impl_->capture_failed(source_frame, "Pre-UI diagnostic copies were not recorded");
    }
    return recorded;
}

void FrameGenerationPlatform::commit_ui_inputs(uint64_t source_frame) noexcept {
    if (!impl_->armed.image || impl_->armed.source != source_frame ||
            !impl_->capture.commit_ui_inputs(source_frame)) {
        impl_->capture.abandon_ui_inputs(source_frame);
        impl_->capture_failed(source_frame, "Pre-UI diagnostic input source was not recorded or armed");
    }
}

void FrameGenerationPlatform::abandon_ui_inputs(uint64_t source_frame) noexcept {
    impl_->capture.abandon_ui_inputs(source_frame);
}

void FrameGenerationPlatform::drain_generated() {
    impl_->capture.abandon_ui_inputs();
    impl_->clear_armed();
    if (getDevice() && getGraphicsQueue()) vkQueueWaitIdle(getGraphicsQueue());
}

FrameGenerationPresentationStatus FrameGenerationPlatform::presentation_status() const {
    std::scoped_lock lock(impl_->status_mutex);
    return impl_->status;
}

uint64_t FrameGenerationPlatform::history_epoch() const noexcept {
    return impl_->history.current();
}
#if defined(HEAVEN_FG_FAULT_INJECTION) && HEAVEN_FG_FAULT_INJECTION
void FrameGenerationPlatform::debug_out_of_date_once() noexcept {
    impl_->out_of_date_once.store(true, std::memory_order_release);
}
#endif

std::vector<FrameGenerationPresentEvent> FrameGenerationPlatform::take_presentation_events() {
    std::scoped_lock lock(impl_->status_mutex);
    std::vector<FrameGenerationPresentEvent> result;
    result.reserve(impl_->events.size());
    while (!impl_->events.empty()) {
        result.push_back(std::move(impl_->events.front()));
        impl_->events.pop_front();
    }
    return result;
}

FrameGenerationPlatform::SwapChainPtr FrameGenerationPlatform::createSwapChain(
        void* nativeWindow, uint64_t flags, VkExtent2D extent) {
    const auto handle = Base::createSwapChain(nativeWindow, flags, extent);
    if (handle) {
        auto& chain = impl_->chains[handle];
        chain.bundle = Base::getSwapChainBundle(handle);
        chain.surface = nativeWindow && extent.width == 0 && extent.height == 0;
    }
    return handle;
}

VkResult FrameGenerationPlatform::acquire(SwapChainPtr handle, ImageSyncData* sync) {
#if defined(HEAVEN_FG_FAULT_INJECTION) && HEAVEN_FG_FAULT_INJECTION
    if (impl_->out_of_date_once.exchange(false, std::memory_order_acq_rel))
        return VK_ERROR_OUT_OF_DATE_KHR;
#endif
    return Base::acquire(handle, sync);
}

VkResult FrameGenerationPlatform::present(SwapChainPtr handle, uint32_t index,
        VkSemaphore finishedDrawing) {
    // Preserve the ordinary path: no tracing allocations, locking, waits or
    // extra submission when no generated image was supplied for this frame.
    if (!impl_->armed.image && !impl_->armed.source) return Base::present(handle, index, finishedDrawing);
    const auto armed = impl_->armed;
    impl_->clear_armed();
    const auto found = impl_->chains.find(handle);
    auto ordinary = [&] {
        impl_->capture.abandon_ui_inputs(armed.source);
        const auto result = Base::present(handle, index, finishedDrawing);
        impl_->event(submitted(result) ? FrameGenerationPresentEventKind::RealSubmitted :
            FrameGenerationPresentEventKind::SubmissionFailed, armed.source, result);
        if (found != impl_->chains.end() && submitted(result)) found->second.last_real_present = Clock::now();
        return result;
    };
    if (!armed.image) return ordinary();
    // prepare_chain uses the armed description, then the pending request is
    // cleared again before returning. Only one present consumes one request.
    impl_->armed = armed;
    if (found == impl_->chains.end() || !impl_->prepare_chain(*this, found->second)) {
        impl_->clear_armed();
        impl_->event(FrameGenerationPresentEventKind::GenerationSkipped, armed.source, VK_NOT_READY,
            presentation_status().reason);
        return ordinary();
    }
    impl_->clear_armed();
    auto& chain = found->second;
    if (index >= chain.work.size()) {
        impl_->capture.abandon_ui_inputs(armed.source);
        chain.broken = true;
        impl_->event(FrameGenerationPresentEventKind::SubmissionFailed, armed.source,
            VK_ERROR_INITIALIZATION_FAILED, "Native acquired image index is outside the swapchain");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const bool capture_requested = impl_->capture.wants_source(armed.source);
    const bool capturing = capture_requested && impl_->capture.begin_pair(getDevice(),
        getPhysicalDevice(), chain.bundle.extent, chain.bundle.colorFormat, armed.source);
    if (capture_requested && !capturing)
        impl_->event(FrameGenerationPresentEventKind::NativeCaptureFailed, armed.source, VK_NOT_READY,
            impl_->capture.last_error());
    auto& generated_work = chain.work[index];
    auto result = impl_->begin_work(getDevice(), generated_work);
    if (result != VK_SUCCESS) {
        chain.broken = true;
        impl_->event(FrameGenerationPresentEventKind::GenerationSkipped, armed.source, result,
            "Generated presentation command buffer is unavailable");
        return ordinary();
    }
    const auto command = generated_work.command;
    const auto real_image = chain.bundle.colors[index];
    transition(command, real_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    transition(command, chain.real_backup, chain.backup_initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        chain.backup_initialized ? VK_ACCESS_TRANSFER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const auto region = copy_region(chain.bundle.extent);
    vkCmdCopyImage(command, real_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain.real_backup,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition(command, chain.real_backup, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    transition(command, real_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    transition(command, armed.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (armed.format == chain.bundle.colorFormat) {
        vkCmdCopyImage(command, armed.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            real_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = blit.dstOffsets[1] = {
            static_cast<int32_t>(armed.extent.width), static_cast<int32_t>(armed.extent.height), 1};
        vkCmdBlitImage(command, armed.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            real_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
    transition(command, armed.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (capturing) {
        transition(command, real_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        impl_->capture.record_generated(command, real_image);
    }
    transition(command, real_image, capturing ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, capturing ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
        0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    result = impl_->submit_work(*this, generated_work, finishedDrawing);
    if (result != VK_SUCCESS) {
        chain.broken = true;
        impl_->event(FrameGenerationPresentEventKind::SubmissionFailed, armed.source, result,
            "Generated image copy submission failed");
        return result;
    }
    chain.backup_initialized = true;
    const auto half_interval = std::chrono::nanoseconds(armed.interval_ns / 2);
    if (chain.last_real_present != Clock::time_point{}) impl_->pace(chain.last_real_present + half_interval);
    result = Base::present(handle, index, generated_work.present_ready);
    impl_->event(submitted(result) ? FrameGenerationPresentEventKind::GeneratedSubmitted :
        FrameGenerationPresentEventKind::SubmissionFailed, armed.source, result);
    if (!submitted(result)) { chain.broken = true; return result; }
    bool suboptimal = result == VK_SUBOPTIMAL_KHR;
    const auto generated_present = Clock::now();

    ImageSyncData sync;
    result = Base::acquire(handle, &sync);
    if (!submitted(result) || sync.imageIndex >= chain.work.size()) {
        chain.broken = true;
        impl_->event(FrameGenerationPresentEventKind::SubmissionFailed, armed.source, result,
            "Real-frame reacquire after generated presentation failed");
        return submitted(result) ? VK_ERROR_INITIALIZATION_FAILED : result;
    }
    suboptimal |= result == VK_SUBOPTIMAL_KHR;
    auto& real_work = chain.work[sync.imageIndex];
    result = impl_->begin_work(getDevice(), real_work);
    if (result != VK_SUCCESS) {
        chain.broken = true;
        impl_->event(FrameGenerationPresentEventKind::SubmissionFailed, armed.source, result,
            "Real-frame restore command buffer is unavailable");
        return result;
    }
    // No dependency on the acquired image contents: the preserved real frame
    // replaces every pixel. Undefined is valid for a newly created swap image.
    transition(real_work.command, chain.bundle.colors[sync.imageIndex], VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyImage(real_work.command, chain.real_backup, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        chain.bundle.colors[sync.imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    if (capturing) {
        transition(real_work.command, chain.bundle.colors[sync.imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        impl_->capture.record_real(real_work.command, chain.bundle.colors[sync.imageIndex]);
    }
    transition(real_work.command, chain.bundle.colors[sync.imageIndex],
        capturing ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, capturing ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT, 0,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    result = impl_->submit_work(*this, real_work, sync.imageReadySemaphore);
    if (result != VK_SUCCESS) {
        chain.broken = true;
        impl_->event(FrameGenerationPresentEventKind::SubmissionFailed, armed.source, result,
            "Real-frame restore submission failed");
        return result;
    }
    impl_->pace(generated_present + half_interval);
    result = Base::present(handle, sync.imageIndex, real_work.present_ready);
    impl_->event(submitted(result) ? FrameGenerationPresentEventKind::RealSubmitted :
        FrameGenerationPresentEventKind::SubmissionFailed, armed.source, result);
    if (submitted(result)) {
        chain.last_real_present = Clock::now();
        if (capturing) {
            const auto ns = [](Clock::time_point value) {
                return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    value.time_since_epoch()).count());
            };
            const bool saved = impl_->capture.finish_pair(real_work.complete,
                ns(generated_present), ns(chain.last_real_present));
            impl_->event(saved ? FrameGenerationPresentEventKind::NativeCaptureSaved :
                FrameGenerationPresentEventKind::NativeCaptureFailed, armed.source,
                saved ? VK_SUCCESS : VK_NOT_READY,
                saved ? "Native generated/real diagnostic pair saved" : impl_->capture.last_error());
        }
    }
    else chain.broken = true;
    return result == VK_SUCCESS && suboptimal ? VK_SUBOPTIMAL_KHR : result;
}

VkResult FrameGenerationPlatform::recreate(SwapChainPtr handle) {
    // The application may already have queued a packet with reset=false. The
    // driver's next native callback must see this before using SDK history.
    impl_->history.invalidate();
    drain_generated();
    impl_->capture.release_after_drain();
    if (const auto found = impl_->chains.find(handle); found != impl_->chains.end())
        impl_->release_chain(getDevice(), found->second);
    const auto result = Base::recreate(handle);
    if (const auto found = impl_->chains.find(handle); found != impl_->chains.end()) {
        found->second.bundle = Base::getSwapChainBundle(handle);
        found->second.broken = false;
        found->second.last_real_present = {};
    }
    impl_->event(FrameGenerationPresentEventKind::HistoryInvalidated, 0, result,
        "Native swapchain recreation invalidated generated presentation history");
    return result;
}

void FrameGenerationPlatform::destroy(SwapChainPtr handle) {
    drain_generated();
    impl_->capture.release_after_drain();
    if (const auto found = impl_->chains.find(handle); found != impl_->chains.end()) {
        impl_->release_chain(getDevice(), found->second);
        impl_->chains.erase(found);
    }
    Base::destroy(handle);
}

void FrameGenerationPlatform::terminate() {
    drain_generated();
    impl_->capture.release_after_drain();
    for (auto& entry : impl_->chains) {
        impl_->release_chain(getDevice(), entry.second);
    }
    impl_->chains.clear();
    Base::terminate();
}

} // namespace heaven
