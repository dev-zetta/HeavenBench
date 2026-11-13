// Headless compute diagnostics, separate from Filament rendering and its score.
#include "gpu_lab_shaders.h"
#include "int8_contract.h"
#include <json.hpp>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace heaven::gpu_lab {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
template<typename T> requires std::is_aggregate_v<T>
T structure(VkStructureType type) {
    T result{};
    result.sType = type;
    return result;
}
constexpr std::uint32_t matrix_size = 256;
constexpr std::uint32_t matrix_batches = 4;
constexpr std::uint32_t dispatches_per_sample = 32;
constexpr std::size_t elements = std::size_t{matrix_size} * matrix_size * matrix_batches;
constexpr VkDeviceSize output_bytes = elements * sizeof(float);
enum class Workload { fp16, int8 };
enum class Kernel { reference, dp4a, cooperative };
// sig_atomic_t is specifically required for the asynchronous signal handler.
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int signal) { interrupted = signal; }

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
void check(VkResult result, std::string_view operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct Options {
    bool list = false;
    bool quick = false;
    bool validation = false;
    std::uint32_t vendor = 0;
    std::uint32_t device = 0;
    unsigned seconds = 60;
    std::string kernel = "both";
    Workload workload = Workload::fp16;
    bool zero_store_control = false;
    std::filesystem::path output;
};

std::uint32_t number(std::string_view text, int base) {
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
    require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "Invalid numeric argument");
    return result;
}
Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help") {
            std::cout << "HeavenBench GPU lab: --list | --gpu VENDOR:DEVICE [--workload fp16|int8] [--kernel both|all|reference|dp4a|cooperative] [--seconds 60..600] [--quick-check] [--validation] [--output FILE]\n"
                         "Default FP16-input/FP32-output; INT8 uses signed inputs and exact INT32 outputs. Both selects reference/cooperative; all also selects INT8 DP4a. No scene FPS or benchmark score.\n"
                         "Correctness control: --workload int8 --kernel reference --quick-check --negative-control zero-store (must fail).\n";
            std::exit(0);
        } else if (argument == "--list") options.list = true;
        else if (argument == "--quick-check") options.quick = true;
        else if (argument == "--validation") options.validation = true;
        else {
            require(i + 1 < argc, "Missing argument value");
            const std::string_view value = argv[++i];
            if (argument == "--gpu") {
                const auto colon = value.find(':');
                require(colon != std::string_view::npos, "Use hexadecimal vendor:device selector");
                options.vendor = number(value.substr(0, colon), 16);
                options.device = number(value.substr(colon + 1), 16);
                require(options.vendor && options.device && options.vendor <= 65535 && options.device <= 65535, "Invalid PCI vendor/device");
            } else if (argument == "--seconds") {
                options.seconds = number(value, 10);
                require(options.seconds >= 60 && options.seconds <= 600, "Measurement requires 60..600 seconds per kernel");
            } else if (argument == "--kernel") {
                require(value == "both" || value == "all" || value == "reference" || value == "cooperative" || value == "dp4a", "Unknown compute kernel");
                options.kernel = value;
            } else if (argument == "--workload") {
                require(value == "fp16" || value == "int8", "Unknown arithmetic workload");
                options.workload = value == "int8" ? Workload::int8 : Workload::fp16;
            } else if (argument == "--negative-control") {
                require(value == "zero-store", "Unknown correctness control");
                options.zero_store_control = true;
            } else if (argument == "--output") options.output = value;
            else throw std::runtime_error("Unknown option: " + std::string(argument));
        }
    }
    require(options.list || options.vendor, "Explicit --gpu vendor:device is required; use --list first");
    require(options.kernel != "dp4a" || options.workload == Workload::int8, "DP4a requires --workload int8");
    require(!options.zero_store_control || (options.workload == Workload::int8 && options.quick && options.kernel == "reference" && !options.list),
            "Zero-store control requires --workload int8 --kernel reference --quick-check");
    require(options.output.empty() || !std::filesystem::exists(options.output), "Output already exists; keep the previous capture");
    return options;
}

struct Adapter {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceDriverProperties driver = structure<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
    VkPhysicalDeviceSubgroupProperties subgroup = structure<VkPhysicalDeviceSubgroupProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES);
    VkPhysicalDeviceIDProperties identity = structure<VkPhysicalDeviceIDProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
    VkPhysicalDeviceVulkan13Properties properties13 = structure<VkPhysicalDeviceVulkan13Properties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES);
    VkPhysicalDeviceVulkan11Features features11 = structure<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    VkPhysicalDeviceVulkan12Features features12 = structure<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    VkPhysicalDeviceVulkan13Features features13 = structure<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperative = structure<VkPhysicalDeviceCooperativeMatrixFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR);
    std::vector<VkCooperativeMatrixPropertiesKHR> shapes;
    std::vector<VkQueueFamilyProperties> queues;
    std::vector<std::string> extensions;

    bool has(std::string_view extension) const {
        return std::ranges::find(extensions, extension) != extensions.end();
    }
    bool dp4a_available() const {
        return features13.shaderIntegerDotProduct && properties13.integerDotProduct4x8BitPackedSignedAccelerated;
    }
    std::string matrix_reason(Workload workload) const {
        if (!cooperative.cooperativeMatrix) return "cooperativeMatrix feature unavailable";
        if (!features12.vulkanMemoryModel) return "vulkanMemoryModel feature unavailable";
        if (!features13.maintenance4) return "maintenance4 feature unavailable";
        if (!(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || !subgroup.subgroupSize)
            return "Compute-stage subgroup unavailable";
        if (workload == Workload::int8 && (!features12.shaderInt8 || !features12.storageBuffer8BitAccess))
            return "Signed INT8 cooperative shader requires shaderInt8 and storageBuffer8BitAccess";
        if (!shape(workload)) return workload == Workload::int8 ?
            "No nonsaturating subgroup signed INT8/INT32 shape dividing workload" : "No subgroup FP16/FP32 shape dividing workload";
        return {};
    }
    bool matrix_available(Workload workload) const { return matrix_reason(workload).empty(); }
    std::optional<VkCooperativeMatrixPropertiesKHR> shape(Workload workload) const {
        const auto input = workload == Workload::int8 ? VK_COMPONENT_TYPE_SINT8_KHR : VK_COMPONENT_TYPE_FLOAT16_KHR;
        const auto output = workload == Workload::int8 ? VK_COMPONENT_TYPE_SINT32_KHR : VK_COMPONENT_TYPE_FLOAT32_KHR;
        for (const auto& item : shapes) {
            if (item.scope == VK_SCOPE_SUBGROUP_KHR && !item.saturatingAccumulation &&
                item.AType == input && item.BType == input && item.CType == output && item.ResultType == output &&
                item.MSize && item.NSize && item.KSize && matrix_size % item.MSize == 0 &&
                matrix_size % item.NSize == 0 && matrix_size % item.KSize == 0) return item;
        }
        return std::nullopt;
    }
    Json describe() const {
        Json matrices = Json::array();
        for (const auto& item : shapes) matrices.push_back({{"m", item.MSize}, {"n", item.NSize}, {"k", item.KSize},
            {"a_type", item.AType}, {"b_type", item.BType}, {"accumulator_type", item.CType},
            {"result_type", item.ResultType}, {"scope", item.scope}, {"saturating", bool(item.saturatingAccumulation)}});
        return {{"name", properties.deviceName}, {"vendor_id", properties.vendorID}, {"device_id", properties.deviceID},
            {"api_version", properties.apiVersion}, {"driver_version_raw", properties.driverVersion},
            {"device_uuid", std::vector<std::uint8_t>(std::begin(identity.deviceUUID), std::end(identity.deviceUUID))},
            {"driver_uuid", std::vector<std::uint8_t>(std::begin(identity.driverUUID), std::end(identity.driverUUID))},
            {"driver", driver.driverName}, {"driver_info", driver.driverInfo}, {"subgroup_size", subgroup.subgroupSize},
            {"capabilities", {{"cooperative_matrix", bool(cooperative.cooperativeMatrix)},
                {"fp16", bool(features12.shaderFloat16)}, {"storage_buffer_16bit", bool(features11.storageBuffer16BitAccess)},
                {"vulkan_memory_model", bool(features12.vulkanMemoryModel)}, {"maintenance4", bool(features13.maintenance4)},
                {"integer_dot_product", bool(features13.shaderIntegerDotProduct)},
                {"packed_signed_int8_dot_accelerated", bool(properties13.integerDotProduct4x8BitPackedSignedAccelerated)},
                {"shader_int8", bool(features12.shaderInt8)}, {"storage_buffer_8bit", bool(features12.storageBuffer8BitAccess)},
                {"variable_rate_shading_extension", has(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)},
                {"ray_query_extension", has(VK_KHR_RAY_QUERY_EXTENSION_NAME)},
                {"mesh_shader_extension", has(VK_EXT_MESH_SHADER_EXTENSION_NAME)}}}, {"matrix_shapes", matrices}};
    }
};

Adapter inspect(VkInstance instance, VkPhysicalDevice physical) {
    Adapter adapter;
    adapter.physical = physical;
    adapter.driver.pNext = &adapter.subgroup;
    adapter.subgroup.pNext = &adapter.identity;
    adapter.identity.pNext = &adapter.properties13;
    auto properties = structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    properties.pNext = &adapter.driver;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    adapter.properties = properties.properties;
    std::uint32_t count = 0;
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "Count extensions");
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()), "Enumerate extensions");
    for (const auto& extension : extensions) adapter.extensions.emplace_back(extension.extensionName);
    adapter.features11.pNext = &adapter.features12;
    adapter.features12.pNext = &adapter.features13;
    if (adapter.has(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) adapter.features13.pNext = &adapter.cooperative;
    auto features = structure<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    features.pNext = &adapter.features11;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (adapter.cooperative.cooperativeMatrix) {
        const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
        require(query != nullptr, "Cooperative matrix enumeration entry point unavailable");
        check(query(physical, &count, nullptr), "Count matrix shapes");
        require(count < 4096, "Unreasonable matrix property count");
        adapter.shapes.resize(count);
        for (auto& item : adapter.shapes) item.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
        check(query(physical, &count, adapter.shapes.data()), "Enumerate matrix shapes");
        adapter.shapes.resize(count);
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    adapter.queues.resize(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, adapter.queues.data());
    // Returned value objects must never retain pointers into this stack object.
    adapter.driver.pNext = nullptr;
    adapter.subgroup.pNext = nullptr;
    adapter.identity.pNext = nullptr;
    adapter.features11.pNext = adapter.features12.pNext = adapter.features13.pNext = nullptr;
    return adapter;
}

class Session {
public:
    Session() { pipelines_.reserve(3); shaders_.reserve(3); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() {
        close();
    }
    void close() noexcept {
        if (device_) {
            if (vkDeviceWaitIdle(device_) != VK_SUCCESS) ++errors_;
            if (mapped_) vkUnmapMemory(device_, staging_memory_);
            for (const auto pipeline : pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
            for (const auto shader : shaders_) vkDestroyShaderModule(device_, shader, nullptr);
            if (query_) vkDestroyQueryPool(device_, query_, nullptr);
            if (fence_) vkDestroyFence(device_, fence_, nullptr);
            if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
            if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
            if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
            if (buffer_) vkDestroyBuffer(device_, buffer_, nullptr);
            if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
            if (memory_) vkFreeMemory(device_, memory_, nullptr);
            if (staging_memory_) vkFreeMemory(device_, staging_memory_, nullptr);
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (messenger_) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(instance_, messenger_, nullptr);
            messenger_ = VK_NULL_HANDLE;
        }
        if (instance_) vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    void instance(bool validation) {
        validation_requested_ = validation;
        auto application = structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        application.pApplicationName = "HeavenBench GPU lab";
        application.apiVersion = VK_API_VERSION_1_3;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        auto create = structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        create.pApplicationInfo = &application;
        create.enabledLayerCount = validation ? 1 : 0;
        create.ppEnabledLayerNames = &layer;
        create.enabledExtensionCount = validation ? 1 : 0;
        create.ppEnabledExtensionNames = &extension;
        check(vkCreateInstance(&create, nullptr, &instance_), "Create Vulkan 1.3 instance");
        if (validation) {
            auto debug = structure<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback = debug_callback;
            debug.pUserData = this;
            const auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
            require(make != nullptr, "Validation debug messenger unavailable");
            check(make(instance_, &debug, nullptr, &messenger_), "Create validation messenger");
        }
    }

    std::vector<Adapter> adapters() const {
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "Count adapters");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "Enumerate adapters");
        std::vector<Adapter> result;
        result.reserve(devices.size());
        for (const auto physical : devices) result.push_back(inspect(instance_, physical));
        return result;
    }

    void initialize(const Adapter& adapter, Workload workload) {
        workload_ = workload;
        input_bytes_ = elements * (workload == Workload::int8 ? sizeof(std::int8_t) : sizeof(std::uint16_t));
        buffer_bytes_ = input_bytes_ * 2 + output_bytes;
        require(adapter.properties.apiVersion >= VK_API_VERSION_1_3, "GPU lab requires Vulkan 1.3");
        require(workload == Workload::int8 || (adapter.features11.storageBuffer16BitAccess && adapter.features12.shaderFloat16), "FP16 inputs/storage unsupported");
        require(workload != Workload::int8 || std::endian::native == std::endian::little, "INT8 packed storage currently requires a little-endian host");
        require(adapter.properties.limits.maxComputeWorkGroupInvocations >= 256 &&
                adapter.properties.limits.maxComputeWorkGroupSize[0] >= 16 && adapter.properties.limits.maxComputeWorkGroupSize[1] >= 16,
                "Reference tile exceeds compute limits");
        const auto family = std::ranges::find_if(adapter.queues, [](const auto& item) {
            return item.queueCount && (item.queueFlags & VK_QUEUE_COMPUTE_BIT) && item.timestampValidBits;
        });
        require(family != adapter.queues.end(), "No compute queue with GPU timestamp support");
        family_ = static_cast<std::uint32_t>(family - adapter.queues.begin());
        timestamp_bits_ = family->timestampValidBits;
        timestamp_period_ = adapter.properties.limits.timestampPeriod;
        require(timestamp_bits_ <= 64 && std::isfinite(timestamp_period_) && timestamp_period_ > 0, "Invalid timestamp precision");
        const float priority = 1;
        auto queue_info = structure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue_info.queueFamilyIndex = family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        auto enabled11 = structure<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
        enabled11.storageBuffer16BitAccess = workload == Workload::fp16;
        auto enabled12 = structure<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        enabled12.shaderFloat16 = workload == Workload::fp16;
        auto enabled_matrix = structure<VkPhysicalDeviceCooperativeMatrixFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR);
        enabled11.pNext = &enabled12;
        const bool matrix = adapter.matrix_available(workload);
        auto enabled13 = structure<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        enabled12.pNext = &enabled13;
        enabled13.shaderIntegerDotProduct = workload == Workload::int8 && adapter.dp4a_available();
        if (matrix) {
            enabled_matrix.cooperativeMatrix = VK_TRUE;
            enabled12.vulkanMemoryModel = VK_TRUE;
            enabled13.maintenance4 = VK_TRUE;
            enabled13.pNext = &enabled_matrix;
            enabled12.shaderInt8 = workload == Workload::int8;
            enabled12.storageBuffer8BitAccess = workload == Workload::int8;
        }
        enabled_features_ = {{"storage_buffer_16bit", bool(enabled11.storageBuffer16BitAccess)},
            {"shader_float16", bool(enabled12.shaderFloat16)}, {"storage_buffer_8bit", bool(enabled12.storageBuffer8BitAccess)},
            {"shader_int8", bool(enabled12.shaderInt8)}, {"integer_dot_product", bool(enabled13.shaderIntegerDotProduct)},
            {"cooperative_matrix", matrix}, {"vulkan_memory_model", bool(enabled12.vulkanMemoryModel)},
            {"maintenance4", bool(enabled13.maintenance4)}};
        const char* extension = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
        auto create = structure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        create.pNext = &enabled11;
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue_info;
        create.enabledExtensionCount = matrix ? 1 : 0;
        create.ppEnabledExtensionNames = &extension;
        check(vkCreateDevice(adapter.physical, &create, nullptr, &device_), "Create explicitly enabled diagnostic device");
        vkGetDeviceQueue(device_, family_, 0, &queue_);
        vkGetPhysicalDeviceMemoryProperties(adapter.physical, &memory_properties_);
        allocate(buffer_, memory_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        allocate(staging_, staging_memory_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkMapMemory(device_, staging_memory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "Map staging memory");
        auto pool = structure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family_;
        check(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "Create command pool");
        auto commands = structure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        commands.commandPool = pool_; commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commands.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &commands, &command_), "Allocate command buffer");
        auto fence = structure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        check(vkCreateFence(device_, &fence, nullptr, &fence_), "Create fence");
        auto queries = structure<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        queries.queryType = VK_QUERY_TYPE_TIMESTAMP; queries.queryCount = 2;
        check(vkCreateQueryPool(device_, &queries, nullptr, &query_), "Create timestamp pool");
        descriptors();
        inputs();
    }

    VkPipeline pipeline(std::span<const std::uint32_t> spirv, const Adapter& adapter, bool matrix) {
        auto shader_info = structure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        shader_info.codeSize = spirv.size_bytes(); shader_info.pCode = spirv.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        check(vkCreateShaderModule(device_, &shader_info, nullptr, &shader), "Create validated shader module");
        shaders_.push_back(shader);
        auto stage = structure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = shader; stage.pName = "main";
        std::array<std::uint32_t, 4> values{};
        const std::array<VkSpecializationMapEntry, 4> entries{{{0, 0, 4}, {1, 4, 4}, {2, 8, 4}, {3, 12, 4}}};
        const VkSpecializationInfo specialization{4, entries.data(), sizeof(values), values.data()};
        if (matrix) {
            const auto shape = adapter.shape(workload_);
            if (!shape) throw std::runtime_error("No matching cooperative matrix shape available");
            require(adapter.subgroup.subgroupSize &&
                (adapter.subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT), "No usable subgroup cooperative shape");
            values = {shape->MSize, shape->NSize, shape->KSize, adapter.subgroup.subgroupSize};
            stage.pSpecializationInfo = &specialization;
        }
        auto create = structure<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        create.stage = stage; create.layout = pipeline_layout_;
        VkPipeline result = VK_NULL_HANDLE;
        check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &result), "Create compute pipeline");
        pipelines_.push_back(result);
        return result;
    }

    double dispatch(VkPipeline pipeline, const Adapter& adapter, bool matrix, std::uint32_t repeats, bool readback) {
        const auto host_start = Clock::now();
        begin();
        barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdResetQueryPool(command_, query_, 0, 2);
        vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1, &set_, 0, nullptr);
        const auto tile = matrix ? adapter.shape(workload_) : std::nullopt;
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_, 0);
        for (std::uint32_t i = 0; i < repeats; ++i) {
            vkCmdDispatch(command_, matrix_size / (tile ? tile->NSize : 16), matrix_size / (tile ? tile->MSize : 16), matrix_batches);
            barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        }
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_, 1);
        if (readback) {
            barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            const VkBufferCopy copy{input_bytes_ * 2, input_bytes_ * 2, output_bytes};
            vkCmdCopyBuffer(command_, buffer_, staging_, 1, &copy);
            auto host = structure<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        }
        submit();
        std::array<std::uint64_t, 2> times{};
        check(vkGetQueryPoolResults(device_, query_, 0, 2, sizeof(times), times.data(), sizeof(times[0]), VK_QUERY_RESULT_64_BIT), "Read completed GPU timestamps");
        const std::uint64_t mask = timestamp_bits_ == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << timestamp_bits_) - 1;
        const auto ticks = (times[1] - times[0]) & mask;
        require(ticks > 0, "Zero GPU timestamp interval");
        // Host elapsed time is an upper bound only, never a GPU/CPU clock fit.
        // Reject intervals long enough to hide a complete timestamp wrap.
        require(seconds_since(host_start) < std::ldexp(timestamp_period_ * 1e-9, static_cast<int>(timestamp_bits_)),
                "Timestamp wrap coverage unavailable for this batch");
        return static_cast<double>(ticks) * timestamp_period_ / 1e6;
    }

    void poison_output() {
        poison_staging();
        begin();
        barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkBufferCopy copy{input_bytes_ * 2, input_bytes_ * 2, output_bytes};
        vkCmdCopyBuffer(command_, staging_, buffer_, 1, &copy);
        submit();
    }

    void verify() const {
        if (workload_ == Workload::int8) {
            const auto* actual = reinterpret_cast<const std::int32_t*>(static_cast<const std::byte*>(mapped_) + input_bytes_ * 2);
            verify_int8(std::span(actual, elements), expected_int8_);
            return;
        }
        const auto* actual = reinterpret_cast<const float*>(static_cast<const std::byte*>(mapped_) + input_bytes_ * 2);
        for (std::size_t i = 0; i < expected_.size(); ++i) {
            if (actual[i] != expected_[i]) throw std::runtime_error("GPU output disagrees with exact integer CPU oracle at element " + std::to_string(i));
        }
    }
    Json validation() const { return {{"errors", errors_.load()}, {"warnings", warnings_.load()}, {"enabled", validation_requested_}}; }
    Json timing() const { return {{"timestamp_valid_bits", timestamp_bits_}, {"timestamp_period_ns", timestamp_period_}, {"queue_family", family_}, {"gpu_clock", "frame-independent device timestamp pairs; no CPU/GPU clock alignment"}}; }
    const Json& enabled_features() const { return enabled_features_; }
    VkDeviceSize input_bytes() const { return input_bytes_; }
    VkDeviceSize buffer_bytes() const { return buffer_bytes_; }

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT /* message_type */, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        auto& session = *static_cast<Session*>(user);
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++session.errors_;
        else ++session.warnings_;
        std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
        return VK_FALSE;
    }
    void allocate(VkBuffer& buffer, VkDeviceMemory& memory, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
        auto create = structure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        create.size = buffer_bytes_; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &create, nullptr, &buffer), "Create bounded buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer, &requirements);
        std::optional<std::uint32_t> index;
        for (std::uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) && (memory_properties_.memoryTypes[i].propertyFlags & properties) == properties) { index = i; break; }
        }
        if (!index) throw std::runtime_error("Required device-local or coherent staging memory unavailable");
        const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, *index};
        check(vkAllocateMemory(device_, &allocation, nullptr, &memory), "Allocate buffer memory");
        check(vkBindBufferMemory(device_, buffer, memory, 0), "Bind buffer memory");
    }
    void descriptors() {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        auto layout = structure<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layout.bindingCount = 3; layout.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &set_layout_), "Create descriptor layout");
        auto pipeline_layout = structure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        pipeline_layout.setLayoutCount = 1; pipeline_layout.pSetLayouts = &set_layout_;
        check(vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &pipeline_layout_), "Create pipeline layout");
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
        auto pool = structure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
        check(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_), "Create descriptor pool");
        const VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptor_pool_, 1, &set_layout_};
        check(vkAllocateDescriptorSets(device_, &allocation, &set_), "Allocate descriptor set");
        const std::array<VkDescriptorBufferInfo, 3> infos{{{buffer_, 0, input_bytes_}, {buffer_, input_bytes_, input_bytes_}, {buffer_, input_bytes_ * 2, output_bytes}}};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set_; writes[i].dstBinding = i;
            writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, 3, writes.data(), 0, nullptr);
    }
    static int numerator(std::uint32_t index, std::uint32_t salt) {
        auto value = index + salt;
        value = (value ^ (value >> 16)) * 0x7feb352du;
        value = (value ^ (value >> 15)) * 0x846ca68bu;
        return static_cast<int>((value ^ (value >> 16)) & 15u) - 8;
    }
    void inputs() {
        if (workload_ == Workload::int8) {
            std::vector<std::int32_t> integers(elements * 2);
            for (std::size_t i = 0; i < integers.size(); ++i) integers[i] = int8_input(static_cast<std::uint32_t>(i));
            std::vector<std::uint32_t> packed(integers.size() / 4);
            for (std::size_t i = 0; i < packed.size(); ++i)
                packed[i] = pack_int8(std::span<const std::int32_t, 4>(integers.data() + i * 4, 4));
            expected_int8_ = int8_oracle(integers, matrix_size, matrix_batches);
            std::memcpy(mapped_, packed.data(), packed.size() * sizeof(packed[0]));
            upload_inputs();
            return;
        }
        const std::array<std::uint16_t, 9> halves{0, 0x2c00, 0x3000, 0x3200, 0x3400, 0x3500, 0x3600, 0x3700, 0x3800};
        std::vector<std::uint16_t> data(elements * 2);
        std::vector<int> integers(elements * 2);
        for (std::size_t i = 0; i < data.size(); ++i) {
            const int value = numerator(static_cast<std::uint32_t>(i), 0x12345u);
            integers[i] = value;
            data[i] = static_cast<std::uint16_t>(halves[static_cast<std::size_t>(std::abs(value))] | (value < 0 ? 0x8000u : 0u));
        }
        expected_.resize(elements);
        for (std::uint32_t batch = 0; batch < matrix_batches; ++batch) {
            const auto base = std::size_t{batch} * matrix_size * matrix_size;
            for (std::size_t row = 0; row < matrix_size; ++row) for (std::size_t column = 0; column < matrix_size; ++column) {
                int sum = 0;
                for (std::size_t k = 0; k < matrix_size; ++k) sum += integers[base + row * matrix_size + k] * integers[elements + base + k * matrix_size + column];
                expected_[base + row * matrix_size + column] = static_cast<float>(sum) / 256.0f;
            }
        }
        std::memcpy(mapped_, data.data(), data.size() * sizeof(data[0]));
        upload_inputs();
    }
    void poison_staging() {
        auto* output = static_cast<std::byte*>(mapped_) + input_bytes_ * 2;
        if (workload_ == Workload::int8)
            std::fill_n(reinterpret_cast<std::int32_t*>(output), elements, std::numeric_limits<std::int32_t>::min());
        else std::fill_n(reinterpret_cast<float*>(output), elements, std::numeric_limits<float>::quiet_NaN());
    }
    void upload_inputs() {
        poison_staging();
        begin();
        const VkBufferCopy copy{0, 0, buffer_bytes_};
        vkCmdCopyBuffer(command_, staging_, buffer_, 1, &copy);
        submit();
    }
    void begin() {
        check(vkResetCommandBuffer(command_, 0), "Reset completed command buffer");
        const VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
        check(vkBeginCommandBuffer(command_, &begin_info), "Begin command buffer");
    }
    void barrier(VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage, VkAccessFlags source, VkAccessFlags destination) {
        auto dependency = structure<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
        dependency.srcAccessMask = source; dependency.dstAccessMask = destination;
        dependency.srcQueueFamilyIndex = dependency.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dependency.buffer = buffer_; dependency.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command_, source_stage, destination_stage, 0, 0, nullptr, 1, &dependency, 0, nullptr);
    }
    void submit() {
        check(vkEndCommandBuffer(command_), "End command buffer");
        check(vkResetFences(device_, 1, &fence_), "Reset completed fence");
        auto submit_info = structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit_info.commandBufferCount = 1; submit_info.pCommandBuffers = &command_;
        check(vkQueueSubmit(queue_, 1, &submit_info, fence_), "Submit bounded compute work");
        check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 10'000'000'000ull), "Complete compute work (10 second bound)");
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_properties_{};
    std::uint32_t family_ = 0, timestamp_bits_ = 0;
    double timestamp_period_ = 0;
    VkBuffer buffer_ = VK_NULL_HANDLE, staging_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE, staging_memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool query_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    std::vector<VkPipeline> pipelines_;
    std::vector<VkShaderModule> shaders_;
    std::vector<float> expected_;
    std::vector<std::int32_t> expected_int8_;
    Workload workload_ = Workload::fp16;
    VkDeviceSize input_bytes_ = 0, buffer_bytes_ = 0;
    Json enabled_features_;
    std::atomic<unsigned> errors_{0}, warnings_{0};
    bool validation_requested_ = false;
};

std::string_view kernel_name(Workload workload, Kernel kernel) {
    if (workload == Workload::fp16) return kernel == Kernel::cooperative ? "cooperative-fp16-fp32" : "shared-tiled-simd-fp32";
    if (kernel == Kernel::cooperative) return "cooperative-int8-int32";
    return kernel == Kernel::dp4a ? "packed-dot4-int8-int32" : "shared-tiled-int8-int32";
}

Json measure(Session& session, const Adapter& adapter, const Options& options, Kernel kernel) {
    const bool matrix = kernel == Kernel::cooperative;
    std::span<const std::uint32_t> spirv = matrix ? std::span<const std::uint32_t>(cooperative_spirv) : std::span<const std::uint32_t>(reference_spirv);
    if (options.workload == Workload::int8) {
        if (options.zero_store_control) spirv = int8_zero_spirv;
        else if (matrix) spirv = int8_cooperative_spirv;
        else if (kernel == Kernel::dp4a) spirv = int8_dp4a_spirv;
        else spirv = int8_reference_spirv;
    }
    const auto pipeline = session.pipeline(spirv, adapter, matrix);
    session.poison_output();
    static_cast<void>(session.dispatch(pipeline, adapter, matrix, 1, true));
    session.verify();
    const auto warmup = Clock::now();
    while (!options.quick && seconds_since(warmup) < 2 && !interrupted) static_cast<void>(session.dispatch(pipeline, adapter, matrix, dispatches_per_sample, false));
    require(!interrupted, "Cancelled during warmup");
    const double warmup_seconds = seconds_since(warmup);
    const auto start = Clock::now();
    std::vector<double> samples;
    do {
        require(samples.size() < 1'000'000, "Timing sample bound exceeded");
        samples.push_back(session.dispatch(pipeline, adapter, matrix, dispatches_per_sample, false));
    } while (!options.quick && seconds_since(start) < options.seconds && !interrupted);
    const double wall_seconds = seconds_since(start);
    session.poison_output();
    static_cast<void>(session.dispatch(pipeline, adapter, matrix, 1, true));
    session.verify();
    require(!interrupted, "Cancelled; incomplete measurement is not a performance result");
    double total = 0;
    for (const auto duration : samples) total += duration;
    auto sorted = samples;
    std::ranges::sort(sorted);
    const auto percentile = [&](double fraction) { return sorted[static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size()))) - 1] / dispatches_per_sample; };
    const double operations_per_dispatch = 2.0 * matrix_size * matrix_size * matrix_size * matrix_batches;
    Json result{{"kernel", kernel_name(options.workload, kernel)}, {"status", "passed"},
        {"correct_output_elements_before_and_after", elements}, {"exact_integer_cpu_oracle", true},
        {"wall_seconds", wall_seconds}, {"warmup_seconds", warmup_seconds},
        {"measurement_window_complete", !options.quick && wall_seconds >= options.seconds},
        {"dispatches_per_sample", dispatches_per_sample}, {"gpu_ms_per_dispatch_mean", total / static_cast<double>(samples.size()) / dispatches_per_sample},
        {"gpu_ms_per_dispatch_p50", percentile(0.5)}, {"gpu_ms_per_dispatch_p95", percentile(0.95)},
        {"gpu_batch_ms", samples}};
    result[options.workload == Workload::int8 ? "integer_gops" : "arithmetic_gflops"] =
        operations_per_dispatch * dispatches_per_sample * static_cast<double>(samples.size()) / (total * 1e6);
    result["enabled_features"] = session.enabled_features();
    if (matrix) {
        const auto shape = adapter.shape(options.workload);
        if (!shape) throw std::runtime_error("Enabled cooperative shape vanished");
        result["enabled_matrix_shape"] = {{"m", shape->MSize}, {"n", shape->NSize}, {"k", shape->KSize},
            {"a_type", shape->AType}, {"b_type", shape->BType}, {"accumulator_type", shape->CType},
            {"result_type", shape->ResultType}, {"scope", shape->scope}, {"saturating", bool(shape->saturatingAccumulation)}};
    }
    return result;
}

int run(const Options& options) {
    Session session;
    session.instance(options.validation);
    const auto adapters = session.adapters();
    Json report{{"schema", 1}, {"application_version", HEAVEN_GPU_LAB_VERSION}, {"benchmark", "gpu-matrix-lab"},
        {"implementation_sha256", implementation_sha256}, {"compiler", __clang_version__}, {"build_type", HEAVEN_GPU_LAB_BUILD_TYPE},
        {"scored", false}, {"scene_fps", nullptr}, {"quick_check", options.quick},
        {"scope", "Separate compute implementations, not a renderer effect, XeSS, theoretical hardware peak or scene performance. GPU intervals include dispatch/barrier overhead; uploads/readback are excluded. Desktop GPU contention is not isolated."}};
    if (options.list) {
        report["adapters"] = Json::array();
        for (const auto& adapter : adapters) report["adapters"].push_back(adapter.describe());
    } else {
        const Adapter* selected = nullptr;
        for (const auto& adapter : adapters) if (adapter.properties.vendorID == options.vendor && adapter.properties.deviceID == options.device) {
            require(selected == nullptr, "Ambiguous GPU selector: multiple adapters share this PCI ID"); selected = &adapter;
        }
        require(selected != nullptr, "Requested GPU is unavailable; no fallback to another adapter");
        report["adapter"] = selected->describe();
        session.initialize(*selected, options.workload);
        report["timing"] = session.timing();
        report["workload"] = {{"matrix_size", matrix_size}, {"batches", matrix_batches},
            {"input", options.workload == Workload::int8 ? "INT8" : "FP16"},
            {"accumulation_output", options.workload == Workload::int8 ? "INT32" : "FP32"},
            {"device_local_bytes", session.buffer_bytes()}, {"staging_bytes", session.buffer_bytes()}};
        if (options.workload == Workload::int8) {
            report["workload"]["signedness"] = "signed";
            report["workload"]["input_layout"] = "A row-major, B column-major; four little-endian signed INT8 values per UINT32";
            report["workload"]["input_range"] = {-128, 127};
            report["workload"]["maximum_accumulator_magnitude"] = matrix_size * 128 * 128;
            report["workload"]["operations_per_dispatch"] = std::uint64_t{2} * matrix_size * matrix_size * matrix_size * matrix_batches;
            report["workload"]["operation_counting"] = "One integer multiply plus one add counts as two integer operations; never FLOPs";
            report["workload"]["cpu_oracle"] = "Independent INT64 sum, exact INT32 output; full coverage before and after timing";
        }
        report["results"] = Json::array();
        for (const auto kernel : {Kernel::reference, Kernel::dp4a, Kernel::cooperative}) {
            if (options.kernel == "reference" && kernel != Kernel::reference) continue;
            if (options.kernel == "cooperative" && kernel != Kernel::cooperative) continue;
            if (options.kernel == "dp4a" && kernel != Kernel::dp4a) continue;
            if (kernel == Kernel::dp4a && (options.workload != Workload::int8 || options.kernel == "both")) continue;
            std::string reason;
            if (kernel == Kernel::cooperative) reason = selected->matrix_reason(options.workload);
            if (kernel == Kernel::dp4a && !selected->dp4a_available()) reason =
                "Requires shaderIntegerDotProduct and integerDotProduct4x8BitPackedSignedAccelerated; unsigned acceleration does not qualify";
            if (!reason.empty()) {
                report["results"].push_back({{"kernel", kernel_name(options.workload, kernel)}, {"status", "unavailable"}, {"reason", reason}});
                continue;
            }
            const auto name = kernel_name(options.workload, kernel);
            std::fprintf(stderr, "GPU lab: %.*s on %s\n", static_cast<int>(name.size()), name.data(), selected->properties.deviceName);
            report["results"].push_back(measure(session, *selected, options, kernel));
        }
        session.close();
        report["validation"] = session.validation();
        require(report["validation"]["errors"] == 0, "Vulkan validation errors; capture refused");
    }
    const auto text = report.dump(2) + '\n';
    if (options.output.empty()) std::cout << text;
    else {
        const auto temporary = std::filesystem::path(options.output.string() + ".partial");
        require(!std::filesystem::exists(temporary), "Partial export already exists");
        {
            std::ofstream stream(temporary, std::ios::binary);
            stream.exceptions(std::ios::failbit | std::ios::badbit);
            stream << text;
            stream.close();
        }
        // Atomic publication without replacing a capture created while running.
        std::filesystem::create_hard_link(temporary, options.output);
        std::filesystem::remove(temporary);
        std::fprintf(stderr, "Saved %s\n", options.output.c_str());
    }
    return 0;
}
} // namespace
} // namespace heaven::gpu_lab

int main(int argc, char** argv) try {
    std::signal(SIGINT, heaven::gpu_lab::interrupt);
    std::signal(SIGTERM, heaven::gpu_lab::interrupt);
    return heaven::gpu_lab::run(heaven::gpu_lab::parse(argc, argv));
} catch (const std::exception& error) {
    std::fprintf(stderr, "GPU lab: %s\n", error.what());
    return heaven::gpu_lab::interrupted ? 130 : 1;
} catch (...) {
    std::fprintf(stderr, "GPU lab: unknown failure\n");
    return 1;
}
