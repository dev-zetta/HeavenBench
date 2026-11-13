// Host-only contract for the actual VulkanReadPixels.cpp. No loader, instance, device, or GPU is
// created. Only its context/texture inputs and BlueVK entry points are replaced. The production
// worker, Request/resource_ptr retention, recording, submission, reshaping and cleanup run unchanged.
#include <backend/PixelBufferDescriptor.h>
#include <bluevk/BlueVK.h>
#include <utils/Panic.h>

#include "vulkan/memory/ResourcePointer.h"
#include "vulkan/utils/Conversion.h"
#include "vulkan/utils/Image.h"
#include "vulkan/VulkanConstants.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// These classes normally require the whole driver to construct. Keep their small input contract
// here, while using the real resource_ptr implementation and the complete readback source below.
#define TNT_FILAMENT_BACKEND_VULKANCONTEXT_H
#define TNT_FILAMENT_BACKEND_VULKANTEXTURE_H
#define TNT_FILAMENT_BACKEND_VULKANCOMMANDS_H
#define TNT_FILAMENT_BACKEND_VULKANHANDLES_H

namespace readback_test {
namespace {
void require(bool value, char const* message) {
    if (!value) throw std::runtime_error(message);
}
template<typename T>
T handle(std::uintptr_t value) {
    // Opaque fake Vulkan identity tokens are compared only and never dereferenced.
    // NOLINTNEXTLINE(performance-no-int-to-ptr) -- models native handle identity without allocating a GPU object.
    return reinterpret_cast<T>(value);
}
constexpr auto waitLimit = std::chrono::seconds(5);
struct Operation {
    enum class Kind { COPY, HOST_BARRIER } kind;
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
};
struct Buffer { VkDeviceSize size = 0; VkDeviceMemory memory = VK_NULL_HANDLE; };
struct Memory {
    // Device writes and host visibility are deliberately distinct. Completing a fence never copies
    // deviceBytes into hostBytes. Only the recorded HOST_READ dependency can do that.
    std::vector<uint8_t> deviceBytes, hostBytes;
};
struct Commands {
    bool recording = false, pending = false;
    std::vector<Operation> operations;
};
struct Fence { VkCommandBuffer commands = VK_NULL_HANDLE; bool submitted = false, complete = false; };
struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::uintptr_t next = 100;
    std::map<VkCommandPool, bool> pools;
    std::map<VkBuffer, Buffer> buffers;
    std::map<VkDeviceMemory, Memory> memories;
    std::map<VkCommandBuffer, Commands> commands;
    std::map<VkFence, Fence> fences;
    std::map<VkImage, std::vector<uint8_t>> images;
    std::string failOperation;
    VkResult failure = VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult waitResult = VK_SUCCESS, mapResult = VK_SUCCESS;
    bool deviceLost = false, hostCached = true;
    bool omitHostBarrier = false;
    unsigned submits = 0, waits = 0, maps = 0, unmaps = 0, hostBarriers = 0;
    std::thread::id backendThread = std::this_thread::get_id();
    VkQueue expectedQueue = handle<VkQueue>(3);
    uint32_t expectedQueueFamily = 7;
    template<typename T> T allocate() { return handle<T>(next++); }
    VkResult result(char const* name) const { return failOperation == name ? failure : VK_SUCCESS; }
    void backend() const noexcept {
        if (std::this_thread::get_id() != backendThread) {
            std::fputs("Native allocation/cleanup left backend thread\n", stderr);
            std::abort();
        }
    }
    void complete() {
        std::scoped_lock lock(mutex);
        for (auto& [_, fence] : fences) {
            if (!fence.submitted || fence.complete) continue;
            auto& command = commands.at(fence.commands);
            for (const auto& operation : command.operations) {
                auto& memory = memories.at(buffers.at(operation.buffer).memory);
                if (operation.kind == Operation::Kind::COPY) {
                    const auto& image = images.at(operation.image);
                    require(image.size() <= memory.deviceBytes.size(), "Copy exceeds native allocation");
                    std::copy(image.begin(), image.end(), memory.deviceBytes.begin());
                } else if (!omitHostBarrier) {
                    memory.hostBytes = memory.deviceBytes;
                }
            }
            command.pending = false;
            fence.complete = true;
        }
        changed.notify_all();
    }
    void waitUntilWorker(unsigned expected) {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, waitLimit, [&] { return waits >= expected; }), "Readback worker did not reach its fence");
    }
    void empty() const {
        require(pools.empty() && buffers.empty() && memories.empty() && commands.empty() && fences.empty(),
                "Readback native resources leaked");
    }
};
State* state = nullptr;
std::atomic_uint destroyedTextures{0};
class StateBinding {
public:
    explicit StateBinding(State& current) noexcept : previous_(std::exchange(state, &current)) {}
    ~StateBinding() { state = previous_; }
    StateBinding(const StateBinding&) = delete;
    StateBinding& operator=(const StateBinding&) = delete;
    StateBinding(StateBinding&&) = delete;
    StateBinding& operator=(StateBinding&&) = delete;
private:
    State* previous_;
};
} // namespace
} // namespace readback_test

namespace filament::backend {
struct VulkanContext {
    uint32_t selectMemoryType(uint32_t /*type_bits*/, VkMemoryPropertyFlags flags) const {
        if (!readback_test::state->hostCached && (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) return VK_MAX_MEMORY_TYPES;
        return 0;
    }
};
struct VulkanTexture {
    uint32_t width = 4, height = 4;
    uint8_t samples = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage image = readback_test::handle<VkImage>(10);
    unsigned references = 0;
    std::map<std::pair<uint32_t, uint32_t>, VulkanLayout> layouts;
    void inc() noexcept { readback_test::state->backend(); ++references; }
    void dec() noexcept { readback_test::state->backend(); if (--references == 0) delete this; }
    ~VulkanTexture() { ++readback_test::destroyedTextures; }
    bool getIsProtected() const { return false; }
    VkFormat getVkFormat() const { return format; }
    VkImage getVkImage() const { return image; }
    VulkanLayout getLayout(uint32_t layer, uint32_t level) const {
        const auto found = layouts.find({layer, level});
        return found == layouts.end() ? VulkanLayout::UNDEFINED : found->second;
    }
    bool transitionLayout(VkCommandBuffer /*command_buffer*/, VkImageSubresourceRange const& range, VulkanLayout layout) {
        const bool changed = getLayout(range.baseArrayLayer, range.baseMipLevel) != layout;
        layouts[{range.baseArrayLayer, range.baseMipLevel}] = layout;
        return changed;
    }
};
struct VulkanAttachment { fvkmemory::resource_ptr<VulkanTexture> texture; uint8_t level = 0, layer = 0; };
struct VulkanRenderTarget {
    VulkanAttachment attachment;
    VulkanAttachment getDepthStencil() const { return attachment; }
    VulkanAttachment getColor(unsigned /*attachment_index*/) const { return attachment; }
    void inc() {}
    void dec() {}
};
namespace fvkutils {
VkImageAspectFlags getImageAspect(VkFormat /*format*/) { return VK_IMAGE_ASPECT_COLOR_BIT; }
uint32_t getComponentCount(VkFormat /*format*/) { return 4; }
PixelDataType getComponentType(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: return PixelDataType::UBYTE;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return PixelDataType::HALF;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return PixelDataType::FLOAT;
        default: throw std::runtime_error("Unexpected fake texture format");
    }
}
} // namespace fvkutils
} // namespace filament::backend

// NOLINTNEXTLINE(bugprone-suspicious-include) -- retains the actual shared worker moved upstream.
#include <vulkan/utils/TaskHandler.cpp>

// NOLINTNEXTLINE(bugprone-suspicious-include) -- exercises the actual implementation with only its native inputs replaced.
#include <vulkan/VulkanReadPixels.cpp>

namespace readback_test {
namespace {
using namespace bluevk;
using namespace filament::backend;
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice /*device*/, VkCommandPoolCreateInfo const* info, VkAllocationCallbacks const* /*allocator*/, VkCommandPool* output) {
    state->backend(); require(info->queueFamilyIndex == state->expectedQueueFamily, "Readback command pool uses wrong queue family");
    const auto result = state->result("vkCreateCommandPool");
    *output = state->allocate<VkCommandPool>();
    if (result == VK_SUCCESS) state->pools.emplace(*output, true);
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice /*device*/, VkCommandPool pool, VkAllocationCallbacks const* /*allocator*/) {
    state->backend(); require(state->commands.empty(), "Command pool destroyed with live command buffers");
    require(state->pools.erase(pool) == 1, "Unknown or failed command pool destroyed");
}
VKAPI_ATTR VkResult VKAPI_CALL createBuffer(VkDevice /*device*/, VkBufferCreateInfo const* info, VkAllocationCallbacks const* /*allocator*/, VkBuffer* output) {
    state->backend(); std::scoped_lock lock(state->mutex); const auto result = state->result("vkCreateBuffer"); *output = state->allocate<VkBuffer>();
    if (result == VK_SUCCESS) state->buffers.emplace(*output, Buffer{info->size});
    return result;
}
VKAPI_ATTR void VKAPI_CALL requirements(VkDevice /*device*/, VkBuffer buffer, VkMemoryRequirements* output) {
    state->backend(); std::scoped_lock lock(state->mutex); *output = {state->buffers.at(buffer).size + 64, 16, 1};
}
VKAPI_ATTR VkResult VKAPI_CALL allocateMemory(VkDevice /*device*/, VkMemoryAllocateInfo const* info, VkAllocationCallbacks const* /*allocator*/, VkDeviceMemory* output) {
    state->backend(); std::scoped_lock lock(state->mutex); const auto result = state->result("vkAllocateMemory"); *output = state->allocate<VkDeviceMemory>();
    if (result == VK_SUCCESS) state->memories.emplace(*output, Memory{
            std::vector<uint8_t>(info->allocationSize), std::vector<uint8_t>(info->allocationSize)});
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL bindMemory(VkDevice /*device*/, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    state->backend(); std::scoped_lock lock(state->mutex); require(offset == 0, "Unexpected binding offset"); const auto result = state->result("vkBindBufferMemory");
    if (result == VK_SUCCESS) state->buffers.at(buffer).memory = memory;
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroyBuffer(VkDevice /*device*/, VkBuffer buffer, VkAllocationCallbacks const* /*allocator*/) {
    state->backend(); std::scoped_lock lock(state->mutex);
    for (const auto& [_, command] : state->commands) if (command.pending && !state->deviceLost)
        for (const auto& operation : command.operations) require(operation.buffer != buffer, "In-flight staging buffer destroyed");
    require(state->buffers.erase(buffer) == 1, "Unknown or failed buffer destroyed");
}
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice /*device*/, VkDeviceMemory memory, VkAllocationCallbacks const* /*allocator*/) {
    state->backend(); std::scoped_lock lock(state->mutex);
    for (const auto& [_, buffer] : state->buffers) require(buffer.memory != memory, "Bound staging memory freed before buffer");
    require(state->memories.erase(memory) == 1, "Unknown or failed memory freed");
}
VKAPI_ATTR VkResult VKAPI_CALL allocateCommands(VkDevice /*device*/, VkCommandBufferAllocateInfo const* info, VkCommandBuffer* output) {
    state->backend(); std::scoped_lock lock(state->mutex); require(state->pools.contains(info->commandPool) && info->commandBufferCount == 1, "Invalid readback command allocation");
    const auto result = state->result("vkAllocateCommandBuffers"); *output = state->allocate<VkCommandBuffer>();
    if (result == VK_SUCCESS) state->commands.emplace(*output, Commands{});
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer commands, VkCommandBufferBeginInfo const* /*begin_info*/) {
    state->backend(); std::scoped_lock lock(state->mutex); const auto result = state->result("vkBeginCommandBuffer");
    if (result == VK_SUCCESS) state->commands.at(commands).recording = true;
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL end(VkCommandBuffer commands) {
    state->backend(); std::scoped_lock lock(state->mutex); auto& command = state->commands.at(commands); require(command.recording, "Unbegun readback commands ended");
    command.recording = false; return state->result("vkEndCommandBuffer");
}
VKAPI_ATTR void VKAPI_CALL freeCommands(VkDevice /*device*/, VkCommandPool /*pool*/, uint32_t count, VkCommandBuffer const* commands) {
    state->backend(); require(count == 1, "Unexpected command free count"); std::scoped_lock lock(state->mutex);
    require(!state->commands.at(*commands).pending || state->deviceLost, "In-flight readback commands freed");
    require(state->commands.erase(*commands) == 1, "Unknown or failed commands freed");
}
VKAPI_ATTR void VKAPI_CALL copy(VkCommandBuffer commands, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t count, VkBufferImageCopy const* regions) {
    state->backend(); std::scoped_lock lock(state->mutex); auto& command = state->commands.at(commands);
    require(command.recording && layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && count == 1, "Invalid readback copy");
    require(regions->bufferOffset == 0 && regions->imageExtent.depth == 1, "Unexpected copy region");
    command.operations.push_back({Operation::Kind::COPY, image, buffer});
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer commands, VkPipelineStageFlags source, VkPipelineStageFlags destination,
        VkDependencyFlags /*dependency_flags*/, uint32_t memoryCount, VkMemoryBarrier const* /*memory_barriers*/, uint32_t bufferCount, VkBufferMemoryBarrier const* buffers,
        uint32_t imageCount, VkImageMemoryBarrier const* /*image_barriers*/) {
    state->backend(); std::scoped_lock lock(state->mutex); auto& command = state->commands.at(commands); require(command.recording, "Barrier outside recording");
    // The production already-TRANSFER_SRC path also emits an image-read ordering dependency.
    if (!bufferCount) { require(memoryCount == 1 && imageCount == 0, "Unexpected source-image barrier"); return; }
    require(bufferCount == 1 && memoryCount == 0 && imageCount == 0, "Readback host dependency has wrong resource scope");
    require(source == VK_PIPELINE_STAGE_TRANSFER_BIT && destination == VK_PIPELINE_STAGE_HOST_BIT,
            "Readback host dependency has wrong stage scopes");
    require(buffers->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT && buffers->dstAccessMask == VK_ACCESS_HOST_READ_BIT &&
            buffers->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && buffers->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
            buffers->offset == 0 && (buffers->size == VK_WHOLE_SIZE || buffers->size >= state->buffers.at(buffers->buffer).size),
            "Readback host dependency has wrong access/ownership/range scopes");
    require(!command.operations.empty() && command.operations.back().kind == Operation::Kind::COPY &&
            command.operations.back().buffer == buffers->buffer, "Host dependency precedes or targets a different copy");
    ++state->hostBarriers; command.operations.push_back({Operation::Kind::HOST_BARRIER, {}, buffers->buffer});
}
VKAPI_ATTR VkResult VKAPI_CALL createFence(VkDevice /*device*/, VkFenceCreateInfo const* /*create_info*/, VkAllocationCallbacks const* /*allocator*/, VkFence* output) {
    state->backend(); std::scoped_lock lock(state->mutex); const auto result = state->result("vkCreateFence"); *output = state->allocate<VkFence>();
    if (result == VK_SUCCESS) state->fences.emplace(*output, Fence{});
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroyFence(VkDevice /*device*/, VkFence fence, VkAllocationCallbacks const* /*allocator*/) {
    state->backend(); std::scoped_lock lock(state->mutex); const auto& item = state->fences.at(fence);
    require(!item.submitted || item.complete || state->deviceLost, "Uncompleted readback fence destroyed");
    require(state->fences.erase(fence) == 1, "Unknown or failed fence destroyed");
}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue, uint32_t count, VkSubmitInfo const* info, VkFence fence) {
    state->backend(); require(queue == state->expectedQueue && count == 1 && info->commandBufferCount == 1 &&
            info->waitSemaphoreCount == 0 && info->signalSemaphoreCount == 0, "Readback changed same-queue submission contract");
    ++state->submits; const auto result = state->result("vkQueueSubmit");
    if (result == VK_SUCCESS) {
        std::scoped_lock lock(state->mutex); auto& command = state->commands.at(*info->pCommandBuffers);
        require(!command.recording && !command.pending, "Invalid readback submit state"); command.pending = true;
        state->fences.at(fence) = {*info->pCommandBuffers, true, false};
    } else if (result == VK_ERROR_DEVICE_LOST) state->deviceLost = true;
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice /*device*/, uint32_t count, VkFence const* fences, VkBool32 all, uint64_t timeout) {
    require(std::this_thread::get_id() != state->backendThread, "Readback waited on backend thread");
    require(count == 1 && all && timeout == UINT64_MAX, "Readback wait contract changed");
    std::unique_lock lock(state->mutex); ++state->waits; state->changed.notify_all();
    if (state->waitResult != VK_SUCCESS) {
        if (state->waitResult == VK_ERROR_DEVICE_LOST) state->deviceLost = true;
        return state->waitResult;
    }
    require(state->fences.at(*fences).submitted, "Wait scheduled for an unsubmitted fence");
    require(state->changed.wait_for(lock, waitLimit, [&] { return state->fences.at(*fences).complete; }), "Fake GPU was never completed");
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL map(VkDevice /*device*/, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize /*size*/, VkMemoryMapFlags /*flags*/, void** output) {
    std::scoped_lock lock(state->mutex); ++state->maps; require(offset == 0, "Unexpected map offset");
    if (state->mapResult == VK_SUCCESS) *output = state->memories.at(memory).hostBytes.data();
    return state->mapResult;
}
VKAPI_ATTR void VKAPI_CALL unmap(VkDevice /*device*/, VkDeviceMemory /*memory*/) { std::scoped_lock lock(state->mutex); ++state->unmaps; }
void install() {
    vkCreateCommandPool = createPool; vkDestroyCommandPool = destroyPool;
    vkCreateBuffer = createBuffer; vkGetBufferMemoryRequirements = requirements;
    vkAllocateMemory = allocateMemory; vkBindBufferMemory = bindMemory;
    vkDestroyBuffer = destroyBuffer; vkFreeMemory = freeMemory;
    vkAllocateCommandBuffers = allocateCommands; vkBeginCommandBuffer = begin; vkEndCommandBuffer = end;
    vkFreeCommandBuffers = freeCommands; vkCmdCopyImageToBuffer = copy; vkCmdPipelineBarrier = barrier;
    vkCreateFence = createFence; vkDestroyFence = destroyFence; vkQueueSubmit = submit;
    vkWaitForFences = wait; vkMapMemory = map; vkUnmapMemory = unmap;
}
struct Fixture {
    State local;
    StateBinding binding{local};
    VulkanContext context;
    std::atomic_uint callbacks{0};
    std::vector<uint8_t> pixels = std::vector<uint8_t>(64, 0xcc);
    VulkanReadPixels reader;
    Fixture() : reader(handle<VkDevice>(1), context, local.expectedQueue, local.expectedQueueFamily,
            [this](PixelBufferDescriptor&& pbd) { ++callbacks; pbd = {}; }) {
    }
    auto texture(VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, uint8_t level = 0, uint16_t layer = 0,
            VulkanLayout layout = VulkanLayout::COLOR_ATTACHMENT) {
        auto texture = std::make_unique<VulkanTexture>();
        texture->format = format; texture->layouts[{layer, level}] = layout;
        texture->width <<= level;
        texture->height <<= level;
        auto& image = local.images[texture->image];
        if (format == VK_FORMAT_R8G8B8A8_UNORM) image = std::vector<uint8_t>(64, 0x59);
        else if (format == VK_FORMAT_R16G16B16A16_SFLOAT) {
            const std::array<filament::math::half, 64> values = [] {
                std::array<filament::math::half, 64> values{};
                std::fill(values.begin(), values.end(), filament::math::half(0.5f)); return values;
            }();
            image.resize(sizeof(values)); std::memcpy(image.data(), values.data(), sizeof(values));
        } else {
            std::array<float, 64> values{}; std::fill(values.begin(), values.end(), 0.5f);
            image.resize(sizeof(values)); std::memcpy(image.data(), values.data(), sizeof(values));
        }
        return fvkmemory::resource_ptr<VulkanTexture>::cast(texture.release());
    }
    void run(fvkmemory::resource_ptr<VulkanTexture> texture, uint8_t level = 0, uint16_t layer = 0) {
        reader.run(std::move(texture), level, layer, 0, 0, 4, 4,
                PixelBufferDescriptor(pixels.data(), pixels.size(), PixelDataFormat::RGBA, PixelDataType::UBYTE));
    }
    void drain() { reader.runUntilComplete(); reader.terminate(); local.empty(); }
};
void success(bool cached, VulkanLayout layout, bool negative, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM) {
    Fixture fixture; fixture.local.hostCached = cached; fixture.local.omitHostBarrier = negative;
    auto texture = fixture.texture(format, 1, 2, layout);
    auto* borrowed = texture.get();
    const auto destroyed = destroyedTextures.load();
    fixture.run(texture, 1, 2);
    require(borrowed->getLayout(2, 1) == layout, "Nonzero layer/mip layout was not restored");
    texture = {};
    require(destroyedTextures.load() == destroyed && fixture.callbacks == 0,
            "Readback did not retain source or complete asynchronously");
    fixture.local.waitUntilWorker(1);
    fixture.local.complete(); fixture.drain();
    require(destroyedTextures.load() == destroyed + 1 && fixture.callbacks == 1 && fixture.local.waits == 1 &&
            fixture.local.maps == 1 && fixture.local.unmaps == 1 && fixture.local.hostBarriers == 1,
            "Readback completion/retention/callback accounting differs");
    const uint8_t expected = format == VK_FORMAT_R8G8B8A8_UNORM ? 0x59 : 0x7f;
    require(std::all_of(fixture.pixels.begin(), fixture.pixels.end(), [expected](uint8_t byte) { return byte == expected; }),
            "Completed copy was not made visible to host reads");
}
void concurrent() {
    Fixture fixture; const auto destroyed = destroyedTextures.load();
    for (unsigned i = 0; i < 3; ++i) fixture.run(fixture.texture());
    require(fixture.callbacks == 0 && destroyedTextures.load() == destroyed, "Pending request source released early");
    fixture.local.waitUntilWorker(1); fixture.local.complete(); fixture.drain();
    require(fixture.callbacks == 3 && fixture.local.waits == 3 && fixture.local.maps == 3 &&
            destroyedTextures.load() == destroyed + 3, "Concurrent readback request accounting differs");
}
void failure(char const* operation, VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY) {
    Fixture fixture; fixture.local.failOperation = operation; fixture.local.failure = result;
    fixture.run(fixture.texture()); fixture.drain();
    require(fixture.callbacks == 1 && fixture.local.waits == 0 && fixture.local.maps == 0 && fixture.local.unmaps == 0,
            "Pre-submit failure scheduled a wait/map or lost its descriptor callback");
    require(std::all_of(fixture.pixels.begin(), fixture.pixels.end(), [](uint8_t byte) { return byte == 0xcc; }),
            "Failed readback published pixels");
}
void mapFailure() {
    Fixture fixture; fixture.local.mapResult = VK_ERROR_MEMORY_MAP_FAILED;
    fixture.run(fixture.texture()); fixture.local.waitUntilWorker(1); fixture.local.complete(); fixture.drain();
    require(fixture.callbacks == 1 && fixture.local.maps == 1 && fixture.local.unmaps == 0,
            "Failed mapping was consumed/unmapped or lost callback");
    require(std::all_of(fixture.pixels.begin(), fixture.pixels.end(), [](uint8_t byte) { return byte == 0xcc; }),
            "Failed map consumed an uninitialized pointer");
}
void waitFailure(VkResult result) {
    Fixture fixture; fixture.local.waitResult = result;
    fixture.run(fixture.texture()); fixture.reader.runUntilComplete(); fixture.reader.terminate(); fixture.local.empty();
    require(result == VK_ERROR_DEVICE_LOST && fixture.callbacks == 1 && fixture.local.maps == 0,
            "Unproven fence completion continued or device-lost pixels were mapped");
}
} // namespace
} // namespace readback_test

int main(int argc, char** argv) {
    using namespace readback_test;
    try {
        install();
        if (argc == 2) {
            const std::string mode = argv[1];
            if (mode == "--negative-host-visibility") success(true, filament::backend::VulkanLayout::COLOR_ATTACHMENT, true);
            else if (mode == "--fatal-wait-oom") waitFailure(VK_ERROR_OUT_OF_HOST_MEMORY);
            else if (mode == "--fatal-wait-timeout") waitFailure(VK_TIMEOUT);
            else if (mode == "--fatal-wait-unknown") waitFailure(VK_ERROR_UNKNOWN);
            else if (mode == "--fatal-submit-unknown") failure("vkQueueSubmit", VK_ERROR_UNKNOWN);
            else throw std::runtime_error("Unknown contract mode");
            std::fprintf(stderr, "Expected negative/death contract unexpectedly returned\n"); return 3;
        }
        require(argc == 1, "Unexpected contract arguments");
        for (bool cached : {true, false}) for (auto layout : {
                filament::backend::VulkanLayout::COLOR_ATTACHMENT, filament::backend::VulkanLayout::TRANSFER_SRC})
            for (auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT})
                success(cached, layout, false, format);
        concurrent();
        for (auto operation : {"vkCreateCommandPool", "vkCreateBuffer", "vkAllocateMemory", "vkBindBufferMemory",
                "vkAllocateCommandBuffers", "vkBeginCommandBuffer", "vkEndCommandBuffer", "vkCreateFence", "vkQueueSubmit"})
            failure(operation);
        failure("vkQueueSubmit", VK_ERROR_OUT_OF_DEVICE_MEMORY);
        failure("vkQueueSubmit", VK_ERROR_DEVICE_LOST);
        mapFailure(); waitFailure(VK_ERROR_DEVICE_LOST);
        std::puts("VulkanReadPixels host contract PASS: independent completion/host visibility, asynchronous source retention, mip/layer restore, cached/bounce paths, Vulkan allocation/submit/map failures and device loss; no GPU execution");
        return 0;
    } catch (std::exception const& error) {
        std::fprintf(stderr, "VulkanReadPixels host contract FAIL: %s\n", error.what()); return 1;
    }
}
