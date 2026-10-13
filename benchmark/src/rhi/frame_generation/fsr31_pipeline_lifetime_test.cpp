// Host-only fault injection into the actual transformed SDK backend. No Vulkan
// device, loader entry point, shader dispatch or queue submission is executed.
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include HEAVEN_FSR31_TEST_BACKEND_COPY

namespace {
std::unordered_map<uintptr_t, std::string> live;
uintptr_t next_handle = 100;
unsigned allocation_call = 0, fail_call = 0;
bool pipeline_handle_on_failure = false;
bool mapped = false;
std::unordered_set<uintptr_t> mapped_memory;
unsigned scheduled_jobs = 0;

template<class T> uintptr_t identity(T handle) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(handle);
    else return static_cast<uintptr_t>(handle);
}
template<class T> T handle(uintptr_t id) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(id);
    else return static_cast<T>(id);
}
template<class T> VkResult allocate(T* output, const char* kind) {
    if (++allocation_call == fail_call) { *output = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    *output = handle<T>(++next_handle);
    live.emplace(identity(*output), kind);
    return VK_SUCCESS;
}
template<class T> void retire(T object, const char* kind) {
    const auto found = live.find(identity(object));
    if (found == live.end() || found->second != kind)
        throw std::runtime_error(std::string("Invalid/double/null retirement: ") + kind);
    live.erase(found);
}
VKAPI_ATTR VkResult VKAPI_CALL sampler(VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*, VkSampler* out) { return allocate(out, "sampler"); }
VKAPI_ATTR void VKAPI_CALL destroy_sampler(VkDevice, VkSampler object, const VkAllocationCallbacks*) { retire(object, "sampler"); }
VKAPI_ATTR VkResult VKAPI_CALL descriptor_layout(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* out) { return allocate(out, "descriptor layout"); }
VKAPI_ATTR void VKAPI_CALL destroy_descriptor_layout(VkDevice, VkDescriptorSetLayout object, const VkAllocationCallbacks*) { retire(object, "descriptor layout"); }
VKAPI_ATTR VkResult VKAPI_CALL descriptor(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet* out) { return allocate(out, "descriptor"); }
VKAPI_ATTR VkResult VKAPI_CALL free_descriptor(VkDevice, VkDescriptorPool, uint32_t count, const VkDescriptorSet* objects) {
    for (uint32_t i = 0; i != count; ++i) retire(objects[i], "descriptor");
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL pipeline_layout(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* out) { return allocate(out, "pipeline layout"); }
VKAPI_ATTR void VKAPI_CALL destroy_pipeline_layout(VkDevice, VkPipelineLayout object, const VkAllocationCallbacks*) { retire(object, "pipeline layout"); }
VKAPI_ATTR VkResult VKAPI_CALL module(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule* out) { return allocate(out, "module"); }
VKAPI_ATTR void VKAPI_CALL destroy_module(VkDevice, VkShaderModule object, const VkAllocationCallbacks*) { retire(object, "module"); }
VKAPI_ATTR VkResult VKAPI_CALL pipeline(VkDevice, VkPipelineCache, uint32_t count, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* out) {
    if (count != 1) throw std::runtime_error("Unexpected compute pipeline count");
    const auto result = allocate(out, "pipeline");
    if (result != VK_SUCCESS && pipeline_handle_on_failure) {
        *out = handle<VkPipeline>(++next_handle);
        live.emplace(identity(*out), "pipeline");
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroy_pipeline(VkDevice, VkPipeline object, const VkAllocationCallbacks*) { retire(object, "pipeline"); }
FfxErrorCode capabilities(FfxInterface*, FfxDeviceCapabilities* out) { *out = {}; return FFX_OK; }
FfxErrorCode blob(FfxEffect, FfxPass, FfxBindStage, uint32_t, FfxShaderBlob* out) {
    static const uint32_t dummy = 0;
    const FfxShaderBlob value{reinterpret_cast<const uint8_t*>(&dummy), sizeof(dummy)};
    std::memcpy(out, &value, sizeof(value));
    return FFX_OK;
}

unsigned exercise(unsigned failure, bool failed_pipeline_handle = false) {
    live.clear(); allocation_call = 0; fail_call = failure;
    pipeline_handle_on_failure = failed_pipeline_handle;
    BackendContext_VK backend{};
    BackendContext_VK::EffectContext effect{};
    BackendContext_VK::PipelineLayout layouts[FFX_MAX_PASS_COUNT]{};
    backend.pEffectContexts = &effect;
    backend.pPipelineLayouts = layouts;
    backend.device = handle<VkDevice>(1);
    backend.descriptorPool = handle<VkDescriptorPool>(2);
    auto& vk = backend.vkFunctionTable;
    vk.vkCreateSampler = sampler; vk.vkDestroySampler = destroy_sampler;
    vk.vkCreateDescriptorSetLayout = descriptor_layout; vk.vkDestroyDescriptorSetLayout = destroy_descriptor_layout;
    vk.vkAllocateDescriptorSets = descriptor; vk.vkFreeDescriptorSets = free_descriptor;
    vk.vkCreatePipelineLayout = pipeline_layout; vk.vkDestroyPipelineLayout = destroy_pipeline_layout;
    vk.vkCreateShaderModule = module; vk.vkDestroyShaderModule = destroy_module;
    vk.vkCreateComputePipelines = pipeline; vk.vkDestroyPipeline = destroy_pipeline;
    FfxInterface interface{};
    interface.scratchBuffer = &backend;
    interface.fpGetPermutationBlobByIndex = blob;
    interface.fpGetDeviceCapabilities = capabilities;
    FfxPipelineDescription description{};
    FfxSamplerDescription samples[2]{};
    for (auto& sample : samples) {
        sample.filter = FFX_FILTER_TYPE_MINMAGMIP_POINT;
        sample.addressModeU = sample.addressModeV = sample.addressModeW = FFX_ADDRESS_MODE_CLAMP;
    }
    description.samplers = samples;
    description.samplerCount = 2;
    FfxPipelineState output{};
    const auto result = CreatePipelineVK(&interface, FFX_EFFECT_FRAMEINTERPOLATION, 0, 0, &description, 0, &output);
    const auto calls = allocation_call;
    if (failure) {
        if (result == FFX_OK || !live.empty() || effect.nextPipelineLayout || output.pipeline || output.rootSignature)
            throw std::runtime_error("Failed allocation leaked or committed output at call " + std::to_string(failure));
        // Context destruction must safely see the now-empty pipeline record.
        DestroyPipelineVK(&interface, &output, 0);
    } else {
        if (result != FFX_OK || live.empty() || effect.nextPipelineLayout != 1 || !output.pipeline || !output.rootSignature)
            throw std::runtime_error("Successful pipeline was not transferred to its owner");
        for (const auto& [id, kind] : live)
            if (kind == "module") throw std::runtime_error("Successful pipeline leaked temporary shader module");
        DestroyPipelineVK(&interface, &output, 0);
    }
    if (!live.empty()) throw std::runtime_error("Retirement left live handles");
    return calls;
}

VKAPI_ATTR VkResult VKAPI_CALL pool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* out) { return allocate(out, "pool"); }
VKAPI_ATTR void VKAPI_CALL destroy_pool(VkDevice, VkDescriptorPool object, const VkAllocationCallbacks*) { retire(object, "pool"); }
VKAPI_ATTR VkResult VKAPI_CALL buffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer* out) { return allocate(out, "buffer"); }
VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer object, const VkAllocationCallbacks*) { retire(object, "buffer"); }
VKAPI_ATTR VkResult VKAPI_CALL memory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* out) { return allocate(out, "memory"); }
VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory object, const VkAllocationCallbacks*) {
    if (mapped_memory.count(identity(object))) throw std::runtime_error("Freed mapped memory without unmapping");
    retire(object, "memory");
}
VKAPI_ATTR void VKAPI_CALL requirements(VkDevice, VkBuffer, VkMemoryRequirements* out) { *out = {4096, 256, 3}; }
VKAPI_ATTR VkResult VKAPI_CALL map(VkDevice, VkDeviceMemory memory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** out) {
    if (++allocation_call == fail_call) return VK_ERROR_MEMORY_MAP_FAILED;
    static char bytes[4096];
    mapped_memory.insert(identity(memory)); mapped = true; *out = bytes; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL unmap(VkDevice, VkDeviceMemory memory) {
    if (!mapped_memory.erase(identity(memory))) throw std::runtime_error("Unmapped memory twice");
    mapped = !mapped_memory.empty();
}
VKAPI_ATTR VkResult VKAPI_CALL bind(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
    return ++allocation_call == fail_call ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL proc(VkDevice, const char* name) {
#define HOST_VK(symbol, fake) if (std::strcmp(name, #symbol) == 0) return reinterpret_cast<PFN_vkVoidFunction>(fake)
    HOST_VK(vkCreateDescriptorPool, pool); HOST_VK(vkDestroyDescriptorPool, destroy_pool);
    HOST_VK(vkCreateBuffer, buffer); HOST_VK(vkDestroyBuffer, destroy_buffer);
    HOST_VK(vkGetBufferMemoryRequirements, requirements);
    HOST_VK(vkAllocateMemory, memory); HOST_VK(vkFreeMemory, free_memory);
    HOST_VK(vkMapMemory, map); HOST_VK(vkUnmapMemory, unmap);
    HOST_VK(vkBindBufferMemory, bind);
#undef HOST_VK
    return nullptr;
}
}

// The SDK uses these directly while probing scratch/global allocation. These
// executable-owned symbols return synthetic capabilities, never call a loader.
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
    *out = {}; out->limits.minUniformBufferOffsetAlignment = 256;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    *out = {}; out->memoryTypeCount = 2;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    out->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*, uint32_t* count, VkExtensionProperties*) { *count = 0; return VK_SUCCESS; }
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkBuffer* out) { return buffer(device, info, allocator, out); }

namespace {
unsigned exercise_backend(unsigned failure) {
    live.clear(); allocation_call = 0; fail_call = failure; mapped = false; mapped_memory.clear();
    const auto physical = handle<VkPhysicalDevice>(1);
    const auto bytes = ffxGetScratchMemorySizeVK(physical, 1);
    struct alignas(32) Block { char bytes[32]; };
    std::vector<Block> scratch((bytes + sizeof(Block) - 1) / sizeof(Block));
    auto* backend = reinterpret_cast<BackendContext_VK*>(scratch.data());
    backend->maxEffectContexts = 1;
    VkDeviceContext device{handle<VkDevice>(1), physical, proc};
    FfxInterface interface{};
    interface.device = reinterpret_cast<FfxDevice>(&device);
    interface.scratchBuffer = scratch.data(); interface.scratchBufferSize = scratch.size() * sizeof(Block);
    uint32_t effect = UINT32_MAX;
    const auto result = CreateBackendContextVK(&interface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &effect);
    const auto calls = allocation_call;
    if (failure) {
        if (result == FFX_OK || !live.empty() || mapped || backend->refCount || backend->device ||
                backend->uniformBuffer || backend->uniformBufferMemory || backend->descriptorPool || effect != UINT32_MAX)
            throw std::runtime_error("Global setup leaked or committed at call " + std::to_string(failure));
        // A failed attempt must be reusable without inheriting stale handles.
        fail_call = 0;
        if (CreateBackendContextVK(&interface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &effect) != FFX_OK)
            throw std::runtime_error("Global setup retry failed");
    } else if (result != FFX_OK || backend->refCount != 1 || !mapped || live.size() != 3 || effect != 0) {
        throw std::runtime_error("Successful global setup did not retain exactly its pool/buffer/memory");
    }
    DestroyBackendContextVK(&interface, effect);
    if (!live.empty() || mapped) throw std::runtime_error("Global teardown leaked handles or mapping");
    return calls;
}

FfxErrorCode schedule(FfxInterface*, const FfxGpuJobDescription*) { ++scheduled_jobs; return FFX_OK; }
void exercise_upload(unsigned failure) {
    live.clear(); mapped_memory.clear(); mapped = false; allocation_call = 0; fail_call = 0; scheduled_jobs = 0;
    const auto physical = handle<VkPhysicalDevice>(1);
    const auto bytes = ffxGetScratchMemorySizeVK(physical, 1);
    struct alignas(32) Block { char bytes[32]; };
    std::vector<Block> scratch((bytes + sizeof(Block) - 1) / sizeof(Block));
    auto* backend = reinterpret_cast<BackendContext_VK*>(scratch.data());
    backend->maxEffectContexts = 1;
    VkDeviceContext device{handle<VkDevice>(1), physical, proc};
    FfxInterface interface{};
    interface.device = reinterpret_cast<FfxDevice>(&device);
    interface.scratchBuffer = scratch.data(); interface.scratchBufferSize = scratch.size() * sizeof(Block);
    interface.fpCreateResource = CreateResourceVK; interface.fpScheduleGpuJob = schedule;
    uint32_t effect = UINT32_MAX;
    if (CreateBackendContextVK(&interface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &effect) != FFX_OK)
        throw std::runtime_error("Upload fixture backend setup failed");
    allocation_call = 0; fail_call = failure;
    FfxCreateResourceDescription description{};
    description.heapType = FFX_HEAP_TYPE_DEFAULT;
    description.resourceDescription = {FFX_RESOURCE_TYPE_BUFFER, FFX_SURFACE_FORMAT_UNKNOWN, 4, 4, 1, 1,
        FFX_RESOURCE_FLAGS_NONE, FFX_RESOURCE_USAGE_UAV};
    description.initialState = FFX_RESOURCE_STATE_UNORDERED_ACCESS;
    description.initData = FfxResourceInitData::FfxResourceInitValue(4, 0);
    FfxResourceInternal resource{};
    const auto result = CreateResourceVK(&interface, &description, effect, &resource);
    if (failure ? (result == FFX_OK || scheduled_jobs != 0) : (result != FFX_OK || scheduled_jobs != 1))
        throw std::runtime_error("Recursive upload failure was not propagated before copy scheduling");
    // The failed recursive allocation already has a context-owned resource
    // slot. Exercise the same copy-resource/primary-resource unwind as FI.
    DestroyResourceVK(&interface, {resource.internalIndex + 1}, effect);
    DestroyResourceVK(&interface, resource, effect);
    if (live.size() != 3 || mapped_memory.size() != 1)
        throw std::runtime_error("Partial upload resources did not unwind");
    DestroyBackendContextVK(&interface, effect);
    if (!live.empty() || mapped) throw std::runtime_error("Upload fixture teardown leaked");
}
}

int main() {
    try {
        const auto calls = exercise(0);
        for (unsigned failure = 1; failure <= calls; ++failure) exercise(failure);
        exercise(calls, true);
        const auto globals = exercise_backend(0);
        for (unsigned failure = 1; failure <= globals; ++failure) exercise_backend(failure);
        exercise_upload(0);
        for (unsigned failure = 4; failure <= 7; ++failure) exercise_upload(failure);
        std::cout << "PASS: " << calls << " pipeline allocation failures, partial failed-pipeline handle, " << globals
                  << " global setup failures, 4 recursive upload failures, retry, success ownership and retirement; no GPU\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
