#include "fsr31_vulkan.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(VkResult result, const char* message) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
struct Image { VkImage image{}; VkDeviceMemory memory{}; VkFormat format{}; };
struct Fixture {
    static constexpr uint32_t width = 256, height = 192;
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t queue_family{};
    VkCommandPool pool{};
    VkCommandBuffer commands{};
    VkFence fence{};
    VkBuffer buffer{};
    VkDeviceMemory buffer_memory{};
    void* mapped{};
    std::vector<Image> images;
    std::unique_ptr<heaven::Fsr31Vulkan> provider;
    bool production_formats = false;
    static constexpr VkDeviceSize color_bytes = width * height * 4 * sizeof(float);
    static constexpr VkDeviceSize depth_bytes = width * height * sizeof(float);
    static constexpr VkDeviceSize motion_bytes = width * height * 2 * sizeof(float);
    static constexpr VkDeviceSize depth_offset = color_bytes;
    static constexpr VkDeviceSize motion_offset = depth_offset + depth_bytes;
    static constexpr VkDeviceSize output_offset = motion_offset + motion_bytes;
    static constexpr VkDeviceSize total_bytes = output_offset + color_bytes;

    ~Fixture() {
        if (device) vkDeviceWaitIdle(device);
        provider.reset();
        for (auto image : images) { vkDestroyImage(device, image.image, nullptr); vkFreeMemory(device, image.memory, nullptr); }
        if (mapped) vkUnmapMemory(device, buffer_memory);
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (buffer_memory) vkFreeMemory(device, buffer_memory, nullptr);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags &&
                !(properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
                    VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD))) return i;
        throw std::runtime_error("No suitable Vulkan memory type");
    }
    void initialize(bool production = false) {
        production_formats = production;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "HeavenBench real FSR 3.1 component test";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        check(vkCreateInstance(&create, nullptr, &instance), "create instance");
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.vendorID == 0x1002 && properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical = candidate; std::cout << "device=" << properties.deviceName << '\n'; break;
            }
        }
        if (!physical) throw std::runtime_error("This fixture requires the discrete AMD Radeon; no fallback selected");
        VkPhysicalDeviceFeatures supported{}; vkGetPhysicalDeviceFeatures(physical, &supported);
        if (!supported.shaderStorageImageExtendedFormats || !supported.shaderStorageImageReadWithoutFormat || !supported.shaderStorageImageWriteWithoutFormat)
            throw std::runtime_error("Required image features unavailable");
        VkPhysicalDeviceFeatures enabled{};
        enabled.shaderStorageImageExtendedFormats = VK_TRUE;
        enabled.shaderStorageImageReadWithoutFormat = VK_TRUE;
        enabled.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        bool found = false;
        for (uint32_t i = 0; i < count; ++i) if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { queue_family = i; found = true; break; }
        if (!found) throw std::runtime_error("No compute queue");
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = queue_family; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue_info; device_info.pEnabledFeatures = &enabled;
        check(vkCreateDevice(physical, &device_info, nullptr, &device), "create device");
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool_info.queueFamilyIndex = queue_family;
        check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "create command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &allocation, &commands), "allocate command buffer");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device, &fence_info, nullptr, &fence), "create fence");
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = total_bytes; buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(device, &buffer_info, nullptr, &buffer), "create staging buffer");
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory_info.allocationSize = requirements.size;
        memory_info.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &memory_info, nullptr, &buffer_memory), "allocate staging memory");
        check(vkBindBufferMemory(device, buffer, buffer_memory, 0), "bind staging memory");
        check(vkMapMemory(device, buffer_memory, 0, total_bytes, 0, &mapped), "map staging");
        auto color_format = production ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32G32B32A32_SFLOAT;
        auto motion_format = production ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_R32G32_SFLOAT;
        for (auto format : {color_format, VK_FORMAT_R32_SFLOAT, motion_format, color_format}) {
            Image image{}; image.format = format;
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width, height, 1};
            info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            check(vkCreateImage(device, &info, nullptr, &image.image), "create image");
            vkGetImageMemoryRequirements(device, image.image, &requirements);
            memory_info.allocationSize = requirements.size;
            memory_info.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(device, &memory_info, nullptr, &image.memory), "allocate image memory");
            check(vkBindImageMemory(device, image.image, image.memory, 0), "bind image memory");
            images.push_back(image);
        }
        create_provider();
    }
    void create_provider() {
        heaven::Fsr31VulkanConfig config;
        config.physical_device = physical; config.device = device; config.get_device_proc_addr = vkGetDeviceProcAddr;
        config.max_render_size = {width, height}; config.display_size = {width, height}; config.color_format = images[0].format;
        config.inverted_depth = production_formats; config.infinite_depth = production_formats;
        config.storage_image_extended_formats_enabled = true;
        config.storage_image_read_without_format_enabled = true;
        config.storage_image_write_without_format_enabled = true;
        auto invalid = config; invalid.storage_image_read_without_format_enabled = false;
        if (heaven::Fsr31Vulkan::create(invalid)) throw std::runtime_error("Missing-feature negative control accepted");
        auto result = heaven::Fsr31Vulkan::create(config);
        if (!result) throw std::runtime_error(result.error());
        provider = std::move(*result);
        std::cout << "provider_gpu_bytes=" << provider->gpu_memory_bytes() << '\n';
    }
    void barrier(Image image, VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src, VkAccessFlags dst,
                 VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src; b.dstAccessMask = dst; b.oldLayout = old_layout; b.newLayout = new_layout;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    heaven::Fsr31VulkanImage input(size_t index) {
        return {images[index].image, images[index].format, {width, height},
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_LAYOUT_GENERAL};
    }
    double frame(uint64_t id, int x_position, bool first, bool reset, bool expected_eligible) {
        auto* color = static_cast<float*>(mapped);
        auto* depth = reinterpret_cast<float*>(static_cast<char*>(mapped) + depth_offset);
        auto* motion = reinterpret_cast<float*>(static_cast<char*>(mapped) + motion_offset);
        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
            size_t i = y * width + x;
            bool foreground = x >= static_cast<uint32_t>(x_position) && x < static_cast<uint32_t>(x_position + 32) && y >= 64 && y < 128;
            color[i * 4] = foreground ? 0.9f : 0.05f;
            color[i * 4 + 1] = color[i * 4 + 2] = color[i * 4]; color[i * 4 + 3] = 1.0f;
            depth[i] = foreground ? 0.4f : 0.9f;
            motion[i * 2] = foreground && !reset ? -8.0f : 0.0f; motion[i * 2 + 1] = 0.0f;
        }
        if (production_formats) {
            auto* bytes = static_cast<uint8_t*>(mapped);
            auto* halves = reinterpret_cast<uint16_t*>(static_cast<char*>(mapped) + motion_offset);
            for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
                const size_t i = y * width + x;
                const bool foreground = x >= static_cast<uint32_t>(x_position) && x < static_cast<uint32_t>(x_position + 32) && y >= 64 && y < 128;
                bytes[i*4] = bytes[i*4+1] = bytes[i*4+2] = foreground ? 230 : 13;
                bytes[i*4+3] = 255;
                depth[i] = 1.0f - depth[i];
                halves[i*2] = foreground && !reset ? 0xc800 : 0; // exact binary16 -8
                halves[i*2+1] = 0;
            }
        }
        check(vkResetCommandBuffer(commands, 0), "reset commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(commands, &begin), "begin commands");
        for (size_t i = 0; i < 3; ++i) {
            barrier(images[i], first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                first ? 0 : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{}; copy.bufferOffset = i == 0 ? 0 : i == 1 ? depth_offset : motion_offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(commands, buffer, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        if (first) barrier(images[3], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        heaven::Fsr31VulkanFrame f;
        f.commands = commands; f.hudless_color = input(0); f.depth = input(1); f.motion_vectors = input(2); f.generated_output = input(3);
        f.frame_id = id; f.reset = reset; f.frame_time_ms = 1000.0f / 60.0f;
        auto invalid = f; invalid.motion_vectors.extent.width = 1;
        if (provider->record(invalid)) throw std::runtime_error("Mismatched input negative control accepted");
        invalid = f; invalid.camera_right = invalid.camera_up;
        if (provider->record(invalid)) throw std::runtime_error("Nonorthogonal camera negative control accepted");
        invalid = f; invalid.generated_output = invalid.hudless_color;
        if (provider->record(invalid)) throw std::runtime_error("Aliased output negative control accepted");
        auto recorded = provider->record(f);
        if (!recorded) throw std::runtime_error(recorded.error());
        if (*recorded != expected_eligible) throw std::runtime_error("Unexpected history eligibility");
        barrier(images[3], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{}; copy.bufferOffset = output_offset; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(commands, images[3].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        barrier(images[3], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        check(vkEndCommandBuffer(commands), "end commands");
        check(vkResetFences(device, 1, &fence), "reset fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &commands;
        check(vkQueueSubmit(queue, 1, &submit, fence), "submit commands");
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull), "wait for component fixture");
        const auto* output = reinterpret_cast<const float*>(static_cast<const char*>(mapped) + output_offset);
        double sum = 0.0, moment = 0.0, peak = 0.0;
        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
            const size_t pixel = (y * width + x) * 4;
            float value = production_formats ? reinterpret_cast<const uint8_t*>(output)[pixel] / 255.0f : output[pixel];
            if (!std::isfinite(value) || value < -0.001f || value > 1.1f) throw std::runtime_error("Nonfinite/out-of-range generated pixel");
            peak = std::max(peak, static_cast<double>(value));
            double weight = std::max(0.0f, value - 0.15f); sum += weight; moment += weight * x;
        }
        if (sum < 500.0 || peak < 0.7) throw std::runtime_error("Generated output lacks the expected foreground");
        const double centroid = moment / sum;
        std::cout << "production_formats=" << production_formats << " frame=" << id << " eligible=" << *recorded << " centroid=" << centroid << " sum=" << sum << " peak=" << peak << '\n';
        return centroid;
    }
};
}

int main() {
    try {
        for (bool production : {false, true}) {
        Fixture fixture; fixture.initialize(production);
        double first = fixture.frame(0, 64, true, true, false);
        double interpolated = fixture.frame(1, 72, false, false, true);
        if (std::abs(first - 79.5) > 0.5 || std::abs(interpolated - 83.5) > 1.5)
            throw std::runtime_error("Moving rectangle does not land at the independently expected halfway position");
        fixture.frame(9, 96, false, false, false);
        fixture.provider->invalidate_history();
        fixture.frame(10, 104, false, false, false);
        check(vkDeviceWaitIdle(fixture.device), "drain before provider recreation");
        fixture.provider.reset(); fixture.create_provider();
        fixture.frame(11, 112, false, true, false);
        }
        std::cout << "PASS: actual FSR interpolation, input rejection, discontinuity/reset and drained recreation\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
