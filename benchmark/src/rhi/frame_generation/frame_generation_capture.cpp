#include "frame_generation_capture.h"
#include "frame_generation_capture_memory_type.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace heaven {
namespace {
using namespace bluevk;
constexpr VkDeviceSize capture_limit = 128ull * 1024 * 1024;

void require(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool native_format_supported(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB ||
        format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
}
uint32_t parse_pair_limit(const char* text) {
    const std::string_view value(text);
    constexpr auto reason = "HEAVEN_FG_CAPTURE_PAIRS must be canonical decimal digits in 1..32 (no leading zeros)";
    if (value.empty() || value.front() == '0') throw std::runtime_error(reason);
    uint32_t parsed = 0;
    for (const char digit : value) {
        if (digit < '0' || digit > '9' || parsed > 32 / 10)
            throw std::runtime_error(reason);
        parsed = parsed * 10 + static_cast<uint32_t>(digit - '0');
        if (parsed > 32) throw std::runtime_error(reason);
    }
    return parsed;
}
uint64_t parse_start_source(const char* text) {
    const std::string_view value(text);
    uint64_t parsed = 0;
    if (value.empty())
        throw std::runtime_error("HEAVEN_FG_CAPTURE_START_SOURCE must be decimal digits in 1..1000000");
    for (const char digit : value) {
        if (digit < '0' || digit > '9' || parsed > 1000000 / 10)
            throw std::runtime_error("HEAVEN_FG_CAPTURE_START_SOURCE must be decimal digits in 1..1000000");
        parsed = parsed * 10 + static_cast<uint64_t>(digit - '0');
        if (parsed > 1000000)
            throw std::runtime_error("HEAVEN_FG_CAPTURE_START_SOURCE must be decimal digits in 1..1000000");
    }
    if (parsed == 0)
        throw std::runtime_error("HEAVEN_FG_CAPTURE_START_SOURCE must be decimal digits in 1..1000000");
    return parsed;
}
} // namespace

struct FrameGenerationCapture::Impl {
    struct Buffer {
        VkBuffer handle{};
        VkDeviceMemory memory{};
        VkDeviceSize allocation_bytes = 0;
        uint32_t memory_type_index = UINT32_MAX;
        VkMemoryPropertyFlags property_flags = 0;
        uint32_t memory_type_bits = 0;
        bool coherent = false;
    };
    std::filesystem::path directory;
    std::string prefix;
    std::string error;
    bool error_storage_failed = false;
    VkDevice device{};
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::array<Buffer, 4> buffers{};
    VkDeviceSize allocated_bytes = 0;
    uint64_t start_source = 1;
    uint64_t source = 0;
    uint64_t ui_source = 0;
    uint64_t pair_ui_source = 0;
    uint32_t pairs = 0;
    uint32_t pair_limit = 4;
    std::array<bool, 2> native_recorded{};
    bool ui_requested = false;
    bool ui_recorded = false;
    bool ui_committed = false;
    bool pending = false;
    bool disabled = false;

    void clear_ui_stage() noexcept {
        ui_source = 0;
        ui_recorded = false;
        ui_committed = false;
    }
    void clear_pair() noexcept {
        source = 0;
        pair_ui_source = 0;
        native_recorded = {};
        pending = false;
    }
    void fail(const char* reason) noexcept {
        try { error = reason; error_storage_failed = false; }
        catch (...) {
            error_storage_failed = true;
            std::fprintf(stderr, "Native capture error could not be retained: %s\n", reason);
        }
        disabled = true;
        clear_ui_stage();
        clear_pair();
    }
    void release() {
        if (device) for (auto& buffer : buffers) {
            if (buffer.handle) vkDestroyBuffer(device, buffer.handle, nullptr);
            if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
            buffer = {};
        }
        device = VK_NULL_HANDLE;
        extent = {};
        format = VK_FORMAT_UNDEFINED;
        allocated_bytes = 0;
        clear_ui_stage();
        clear_pair();
    }

    void allocate(VkPhysicalDevice physical) {
        const size_t count = ui_requested ? 4 : 2;
        // Validate before multiplying four bytes per pixel, including malformed
        // uint32 extents. Count actual Vulkan allocation sizes against the cap.
        const VkDeviceSize row_bytes = VkDeviceSize(extent.width) * 4;
        if (row_bytes == 0 || extent.height == 0 ||
                row_bytes > (capture_limit / count) / extent.height)
            throw std::runtime_error("Native capture exceeds the 128 MiB diagnostic allocation limit");
        const VkDeviceSize bytes = row_bytes * extent.height;
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        for (size_t index = 0; index < count; ++index) {
            auto& buffer = buffers[index];
            VkBufferCreateInfo create{}; create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            create.size = bytes;
            create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            require(vkCreateBuffer(device, &create, nullptr, &buffer.handle), "Create capture buffer");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
            if (requirements.size > capture_limit - allocated_bytes)
                throw std::runtime_error("Native capture exceeds the 128 MiB diagnostic allocation limit");
            const auto selected = select_frame_generation_readback_memory_type(
                memory, requirements.memoryTypeBits);
            if (selected == UINT32_MAX) throw std::runtime_error("No host-visible capture memory");
            buffer.memory_type_index = selected;
            buffer.property_flags = memory.memoryTypes[selected].propertyFlags;
            buffer.memory_type_bits = requirements.memoryTypeBits;
            buffer.allocation_bytes = requirements.size;
            buffer.coherent = (buffer.property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            VkMemoryAllocateInfo allocation{}; allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = selected;
            require(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory), "Allocate capture memory");
            allocated_bytes += requirements.size;
            require(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0), "Bind capture memory");
        }
    }

    void ensure_storage(VkDevice requested_device, VkPhysicalDevice physical, VkExtent2D size) {
        if (!requested_device || !physical)
            throw std::runtime_error("Native capture requires a valid Vulkan device");
        if (device && (device != requested_device || extent.width != size.width || extent.height != size.height))
            throw std::runtime_error("Capture device or extent changed before resource drain");
        if (!device) {
            device = requested_device;
            extent = size;
            allocate(physical);
            std::filesystem::create_directories(directory);
        }
    }

    void record(VkCommandBuffer commands, VkImage image, size_t index) {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            buffers[index].handle, 1, &copy);
        VkMemoryBarrier host{}; host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &host, 0, nullptr, 0, nullptr);
    }

    void write_image(size_t index, VkFormat image_format, bool preserve_alpha,
            const std::filesystem::path& destination) {
        struct Mapping {
            VkDevice device;
            VkDeviceMemory memory;
            void* address = nullptr;
            Mapping(VkDevice device_in, VkDeviceMemory memory_in) : device(device_in), memory(memory_in) {
                require(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &address), "Map native capture");
            }
            ~Mapping() { vkUnmapMemory(device, memory); }
            Mapping(const Mapping&) = delete;
            Mapping& operator=(const Mapping&) = delete;
        } mapping(device, buffers[index].memory);
        {
            if (!buffers[index].coherent) {
                VkMappedMemoryRange range{}; range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = buffers[index].memory;
                range.size = VK_WHOLE_SIZE;
                require(vkInvalidateMappedMemoryRanges(device, 1, &range), "Invalidate native capture");
            }
            const auto temporary = destination.string() + ".tmp";
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.exceptions(std::ios::badbit | std::ios::failbit);
            const auto* pixels = static_cast<const uint8_t*>(mapping.address);
            if (preserve_alpha) {
                // Input validation fixes this buffer to RGBA8_UNORM. PAM keeps
                // the actual premultiplied RGB and alpha without conversion.
                stream << "P7\nWIDTH " << extent.width << "\nHEIGHT " << extent.height
                    << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
                stream.write(reinterpret_cast<const char*>(pixels),
                    static_cast<std::streamsize>(VkDeviceSize(extent.width) * extent.height * 4));
            } else {
                stream << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
                std::vector<char> row(size_t(extent.width) * 3);
                const bool bgra = image_format == VK_FORMAT_B8G8R8A8_UNORM || image_format == VK_FORMAT_B8G8R8A8_SRGB;
                for (uint32_t y = 0; y < extent.height; ++y) {
                    for (uint32_t x = 0; x < extent.width; ++x) {
                        const auto* pixel = pixels + (size_t(y) * extent.width + x) * 4;
                        row[size_t(x) * 3] = static_cast<char>(pixel[bgra ? 2 : 0]);
                        row[size_t(x) * 3 + 1] = static_cast<char>(pixel[1]);
                        row[size_t(x) * 3 + 2] = static_cast<char>(pixel[bgra ? 0 : 2]);
                    }
                    stream.write(row.data(), static_cast<std::streamsize>(row.size()));
                }
            }
            stream.close();
            std::filesystem::rename(temporary, destination);
        }
    }
};

FrameGenerationCapture::FrameGenerationCapture() {
    if (const auto* path = std::getenv("HEAVEN_FG_CAPTURE_DIR"); path && *path) {
        impl_ = std::make_unique<Impl>();
        impl_->directory = path;
        impl_->prefix = "native-" + std::to_string(now_ns());
        const auto* ui = std::getenv("HEAVEN_FG_CAPTURE_UI_INPUTS");
        impl_->ui_requested = ui && std::string_view(ui) == "1";
        if (const auto* limit = std::getenv("HEAVEN_FG_CAPTURE_PAIRS")) {
            try { impl_->pair_limit = parse_pair_limit(limit); }
            catch (const std::exception& error) { impl_->fail(error.what()); }
        }
        if (const auto* start = std::getenv("HEAVEN_FG_CAPTURE_START_SOURCE")) {
            try { impl_->start_source = parse_start_source(start); }
            catch (const std::exception& error) {
                impl_->fail(error.what());
            }
        }
    }
}
FrameGenerationCapture::~FrameGenerationCapture() { if (impl_) impl_->release(); }
bool FrameGenerationCapture::enabled() const noexcept {
    return impl_ && !impl_->directory.empty() && !impl_->disabled && impl_->pairs < impl_->pair_limit;
}
bool FrameGenerationCapture::wants_source(uint64_t source_frame) const noexcept {
    return enabled() && source_frame >= impl_->start_source;
}
bool FrameGenerationCapture::ui_inputs_enabled() const noexcept {
    return enabled() && impl_->ui_requested;
}
bool FrameGenerationCapture::prepare_ui_inputs(VkDevice device, VkPhysicalDevice physical,
        VkExtent2D size, VkFormat generated_format, VkFormat ui_format, uint64_t source_frame) {
    if (!ui_inputs_enabled() || !wants_source(source_frame)) return false;
    auto& p = *impl_;
    try {
        if (generated_format != VK_FORMAT_R8G8B8A8_UNORM || ui_format != VK_FORMAT_R8G8B8A8_UNORM)
            throw std::runtime_error("Native UI diagnostic inputs require RGBA8_UNORM images");
        if (p.pending)
            throw std::runtime_error("Native UI capture preparation overlaps an unfinished pair");
        p.clear_ui_stage();
        p.ensure_storage(device, physical, size);
        p.ui_source = source_frame;
        return true;
    } catch (const std::exception& error) {
        p.fail(error.what());
        return false;
    }
}
bool FrameGenerationCapture::record_ui_inputs(VkCommandBuffer commands,
        VkImage generated_before_ui, VkImage ui_layer) {
    if (!ui_inputs_enabled()) return false;
    auto& p = *impl_;
    if (!p.ui_source || p.ui_recorded || !commands || !generated_before_ui || !ui_layer) {
        p.fail("Native UI input copies require one prepared source and valid images/commands");
        return false;
    }
    // Abandoned input commands may have executed without a later native pair.
    // Order any previous diagnostic transfer writes before reusing the buffers;
    // source validity is committed separately from command recording.
    VkMemoryBarrier reuse{}; reuse.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    reuse.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &reuse, 0, nullptr, 0, nullptr);
    p.record(commands, generated_before_ui, 2);
    p.record(commands, ui_layer, 3);
    p.ui_recorded = true;
    return true;
}
bool FrameGenerationCapture::commit_ui_inputs(uint64_t source_frame) noexcept {
    if (!ui_inputs_enabled()) return false;
    auto& p = *impl_;
    if (source_frame == 0 || p.ui_source != source_frame || !p.ui_recorded || p.ui_committed) {
        p.fail("Native UI capture commit has no matching recorded source");
        return false;
    }
    p.ui_committed = true;
    return true;
}
void FrameGenerationCapture::abandon_ui_inputs(uint64_t source_frame) noexcept {
    if (!impl_ || !impl_->ui_requested) return;
    auto& p = *impl_;
    if (source_frame == 0 || p.ui_source == source_frame) p.clear_ui_stage();
    if (p.pending && (source_frame == 0 || p.pair_ui_source == source_frame)) p.clear_pair();
}
bool FrameGenerationCapture::begin_pair(VkDevice device, VkPhysicalDevice physical,
        VkExtent2D size, VkFormat format, uint64_t source_frame) {
    if (!wants_source(source_frame)) return false;
    auto& p = *impl_;
    try {
        if (!native_format_supported(format))
            throw std::runtime_error("Native diagnostic capture requires an SDR RGBA8/BGRA8 swapchain");
        if (p.pending)
            throw std::runtime_error("Native capture overlaps an unfinished pair");
        if (p.ui_requested && (!p.ui_committed || !p.ui_recorded || p.ui_source != source_frame))
            throw std::runtime_error("Native pair has no matching committed UI inputs");
        if (p.format != VK_FORMAT_UNDEFINED && p.format != format)
            throw std::runtime_error("Native capture format changed before resource drain");
        p.ensure_storage(device, physical, size);
        p.format = format;
        p.source = source_frame;
        p.pair_ui_source = p.ui_requested ? p.ui_source : 0;
        p.clear_ui_stage();
        p.native_recorded = {};
        p.pending = true;
        return true;
    } catch (const std::exception& error) {
        p.fail(error.what());
        return false;
    }
}
void FrameGenerationCapture::record_generated(VkCommandBuffer commands, VkImage image) {
    if (!impl_ || !impl_->pending) return;
    if (!commands || !image || impl_->native_recorded[0]) {
        impl_->fail("Native generated readback requires valid images/commands and one recording");
        return;
    }
    impl_->record(commands, image, 0);
    impl_->native_recorded[0] = true;
}
void FrameGenerationCapture::record_real(VkCommandBuffer commands, VkImage image) {
    if (!impl_ || !impl_->pending) return;
    if (!commands || !image || impl_->native_recorded[1]) {
        impl_->fail("Native real readback requires valid images/commands and one recording");
        return;
    }
    impl_->record(commands, image, 1);
    impl_->native_recorded[1] = true;
}
bool FrameGenerationCapture::finish_pair(VkFence real_consumer, uint64_t generated_present_ns,
        uint64_t real_present_ns) {
    if (!impl_) return false;
    auto& p = *impl_;
    if (!p.pending) return false;
    try {
        if (!real_consumer || !p.native_recorded[0] || !p.native_recorded[1] ||
                (p.ui_requested && p.pair_ui_source != p.source))
            throw std::runtime_error("Native capture lacks a complete source-matched consumer/readback set");
        const auto wait_begin = now_ns();
        require(vkWaitForFences(p.device, 1, &real_consumer, VK_TRUE, UINT64_MAX), "Wait native readback consumer");
        const auto completed = now_ns();
        const auto stem = p.prefix + "-pair" + std::to_string(p.pairs) + "-source" + std::to_string(p.source);
        const auto generated = stem + "-generated.ppm", real = stem + "-real.ppm";
        p.write_image(0, p.format, false, p.directory / generated);
        p.write_image(1, p.format, false, p.directory / real);
        std::string before_ui, ui_layer;
        if (p.ui_requested) {
            before_ui = stem + "-generated-before-ui.ppm";
            ui_layer = stem + "-ui-layer.pam";
            p.write_image(2, VK_FORMAT_R8G8B8A8_UNORM, false, p.directory / before_ui);
            p.write_image(3, VK_FORMAT_R8G8B8A8_UNORM, true, p.directory / ui_layer);
        }
        const auto images_export_completed = now_ns();
        // Publish the receipt last. Partial file output cannot look like a
        // successful source-matched pair after an export failure.
        const auto metadata = p.directory / (stem + ".json");
        const auto temporary = metadata.string() + ".tmp";
        std::ofstream stream(temporary, std::ios::trunc);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream << "{\n  \"source_frame\": " << p.source
            << ",\n  \"configured_pair_limit\": " << p.pair_limit << ",\n  \"pair_index\": " << p.pairs
            << ",\n  \"width\": " << p.extent.width
            << ",\n  \"height\": " << p.extent.height << ",\n  \"vk_format\": " << p.format
            << ",\n  \"generated_present_return_cpu_monotonic_ns\": " << generated_present_ns
            << ",\n  \"real_present_return_cpu_monotonic_ns\": " << real_present_ns
            << ",\n  \"readback_wait_begin_cpu_monotonic_ns\": " << wait_begin
            << ",\n  \"readback_fence_completed_cpu_monotonic_ns\": " << completed
            << ",\n  \"images_export_completed_cpu_monotonic_ns\": " << images_export_completed
            << ",\n  \"readback_wait_duration_ns\": " << completed - wait_begin
            << ",\n  \"images_export_duration_ns\": " << images_export_completed - completed
            << ",\n  \"capture_allocated_bytes\": " << p.allocated_bytes
            << ",\n  \"diagnostic_gpu_wait_enabled\": true,\n  \"display_scanout_verified\": false,\n"
            << R"(  "generated": ")" << generated << "\",\n  \"real\": \"" << real << '"';
        stream << ",\n  \"readback_buffers\": [";
        const size_t buffer_count = p.ui_requested ? 4 : 2;
        for (size_t index = 0; index < buffer_count; ++index) {
            const auto& buffer = p.buffers[index];
            if (index) stream << ',';
            stream << "{\"memory_type_index\":" << buffer.memory_type_index
                << ",\"property_flags\":" << buffer.property_flags
                << ",\"memory_type_bits\":" << buffer.memory_type_bits
                << ",\"allocation_bytes\":" << buffer.allocation_bytes << '}';
        }
        stream << ']';
        if (p.ui_requested) {
            stream << ",\n  \"ui_inputs_source_frame\": " << p.pair_ui_source
                << ",\n  \"generated_before_ui_vk_format\": " << VK_FORMAT_R8G8B8A8_UNORM
                << ",\n  \"ui_layer_vk_format\": " << VK_FORMAT_R8G8B8A8_UNORM
                << ",\n  \"generated_before_ui\": \"" << before_ui
                << "\",\n  \"ui_layer\": \"" << ui_layer << '"';
        }
        stream << "\n}\n";
        stream.close();
        std::filesystem::rename(temporary, metadata);
        ++p.pairs;
        p.clear_pair();
        return true;
    } catch (const std::exception& error) {
        p.fail(error.what());
        return false;
    }
}
std::string FrameGenerationCapture::last_error() const {
    if (!impl_) return {};
    if (impl_->error_storage_failed) return "Native capture failed; error text allocation failed";
    return impl_->error;
}
void FrameGenerationCapture::release_after_drain() { if (impl_) impl_->release(); }

} // namespace heaven
