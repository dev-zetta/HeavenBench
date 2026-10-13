#include "frame_generation_capture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace bluevk;
namespace fs = std::filesystem;
template<class T> T handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void environment(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    if (value.empty()) unsetenv(key); else setenv(key, value.c_str(), 1);
#endif
}
struct Memory { VkDeviceSize size; std::vector<uint8_t> bytes; };
struct Buffer { VkDeviceSize size; VkDeviceMemory memory{}; };
std::map<VkBuffer, Buffer> buffers;
std::map<VkDeviceMemory, Memory> memories;
std::map<VkImage, std::vector<uint8_t>> images;
uintptr_t next_handle = 100;
unsigned api_calls = 0, allocation_calls = 0, fail_allocation = 0, waits = 0, copies = 0;
VkDeviceSize requirement_padding = 0;
bool fail_wait = false;
VKAPI_ATTR void VKAPI_CALL physical_memory(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    ++api_calls; *out = {}; out->memoryTypeCount = 1;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice, const VkBufferCreateInfo* info,
        const VkAllocationCallbacks*, VkBuffer* out) {
    ++api_calls; *out = handle<VkBuffer>(next_handle++); buffers.emplace(*out, Buffer{info->size}); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL buffer_requirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* out) {
    ++api_calls; *out = {buffers.at(buffer).size + requirement_padding, 1, 1};
}
VKAPI_ATTR VkResult VKAPI_CALL allocate_memory(VkDevice, const VkMemoryAllocateInfo* info,
        const VkAllocationCallbacks*, VkDeviceMemory* out) {
    ++api_calls; ++allocation_calls;
    if (allocation_calls == fail_allocation) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = handle<VkDeviceMemory>(next_handle++);
    // Only small byte fixtures are mapped; large-limit cases never allocate a
    // real large host array. Size accounting follows the fake native request.
    memories.emplace(*out, Memory{info->allocationSize, std::vector<uint8_t>(std::min<VkDeviceSize>(info->allocationSize, 4096))});
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL bind_buffer(VkDevice, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize) {
    ++api_calls; buffers.at(buffer).memory = memory; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) {
    ++api_calls; require(buffers.erase(buffer) == 1, "Destroyed unknown or duplicate capture buffer");
}
VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    ++api_calls; require(memories.erase(memory) == 1, "Freed unknown or duplicate capture memory");
}
VKAPI_ATTR void VKAPI_CALL copy_image(VkCommandBuffer, VkImage image, VkImageLayout layout,
        VkBuffer buffer, uint32_t count, const VkBufferImageCopy* regions) {
    ++api_calls; ++copies;
    require(layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && count == 1, "Invalid capture copy contract");
    const size_t bytes = size_t(regions[0].imageExtent.width) * regions[0].imageExtent.height * 4;
    require(images.at(image).size() == bytes, "Capture image extent mismatch");
    memories.at(buffers.at(buffer).memory).bytes = images.at(image);
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
        VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
        uint32_t, const VkImageMemoryBarrier*) { ++api_calls; }
VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) {
    ++api_calls; ++waits; return fail_wait ? VK_TIMEOUT : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL map(VkDevice, VkDeviceMemory memory, VkDeviceSize, VkDeviceSize,
        VkMemoryMapFlags, void** out) {
    ++api_calls; *out = memories.at(memory).bytes.data(); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL unmap(VkDevice, VkDeviceMemory) { ++api_calls; }
VKAPI_ATTR VkResult VKAPI_CALL invalidate(VkDevice, uint32_t, const VkMappedMemoryRange*) {
    ++api_calls; return VK_SUCCESS;
}
void install_fakes() {
    vkGetPhysicalDeviceMemoryProperties = physical_memory; vkCreateBuffer = create_buffer;
    vkGetBufferMemoryRequirements = buffer_requirements; vkAllocateMemory = allocate_memory;
    vkBindBufferMemory = bind_buffer; vkDestroyBuffer = destroy_buffer; vkFreeMemory = free_memory;
    vkCmdCopyImageToBuffer = copy_image; vkCmdPipelineBarrier = barrier; vkWaitForFences = wait;
    vkMapMemory = map; vkUnmapMemory = unmap; vkInvalidateMappedMemoryRanges = invalidate;
}
std::string contents(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void no_live_resources() {
    require(buffers.empty() && memories.empty(), "Capture resources leaked after drain/destruction");
}
void options(const fs::path& destination, const std::string& start = "1", bool ui = true) {
    environment("HEAVEN_FG_CAPTURE_DIR", destination.string());
    environment("HEAVEN_FG_CAPTURE_START_SOURCE", start);
    environment("HEAVEN_FG_CAPTURE_UI_INPUTS", ui ? "1" : "");
}
constexpr VkExtent2D size{4,4};
const auto device = handle<VkDevice>(1);
const auto physical = handle<VkPhysicalDevice>(2);
const auto commands = handle<VkCommandBuffer>(3);
const auto fence = handle<VkFence>(4);
const auto before_image = handle<VkImage>(5), ui_image = handle<VkImage>(6);
const auto generated_image = handle<VkImage>(7), real_image = handle<VkImage>(8);
bool prepare(heaven::FrameGenerationCapture& capture, uint64_t source, VkExtent2D extent = size) {
    return capture.prepare_ui_inputs(device, physical, extent,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, source);
}
void record(heaven::FrameGenerationCapture& capture, uint64_t source) {
    require(prepare(capture, source), "UI input preparation failed");
    require(capture.record_ui_inputs(commands, before_image, ui_image), "UI input copies failed");
    require(capture.commit_ui_inputs(source), "UI source commit failed");
}
void pair(heaven::FrameGenerationCapture& capture, uint64_t source) {
    record(capture, source);
    require(capture.begin_pair(device, physical, size, VK_FORMAT_B8G8R8A8_UNORM, source), "Matching pair rejected");
    capture.record_generated(commands, generated_image); capture.record_real(commands, real_image);
    require(capture.finish_pair(fence, 10, 20), "Pair completion failed");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "Pass a writable temporary test directory\n"); return 2; }
    const fs::path destination = fs::path(argv[1]) / std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        require(!fs::exists(destination), "Refusing to overwrite a capture-contract directory");
        install_fakes();
        environment("HEAVEN_FG_CAPTURE_DIR", ""); environment("HEAVEN_FG_CAPTURE_UI_INPUTS", "1");
        environment("HEAVEN_FG_CAPTURE_START_SOURCE", "bad");
        {
            heaven::FrameGenerationCapture capture;
            require(!capture.enabled() && !capture.wants_source(1) && !prepare(capture, 1), "Capture active without directory");
        }
        for (const auto* invalid : {"0", "-1", "+1", " 1", "1 ", "1x", "1000001", "18446744073709551616"}) {
            options(destination, invalid);
            heaven::FrameGenerationCapture capture;
            require(!capture.enabled() && !capture.last_error().empty(), "Invalid source gate accepted or silently ignored");
        }
        options(destination, "90");
        {
            heaven::FrameGenerationCapture capture;
            require(capture.enabled() && !capture.wants_source(0) && !capture.wants_source(89) && capture.wants_source(90), "Source gate failed");
            require(!prepare(capture, 89), "Input captured before source gate");
        }
        require(api_calls == 0 && !fs::exists(destination), "Disabled/gated paths allocated, called Vulkan or wrote files");
        images[before_image] = std::vector<uint8_t>(64); images[ui_image] = std::vector<uint8_t>(64);
        images[generated_image] = std::vector<uint8_t>(64); images[real_image] = std::vector<uint8_t>(64);
        for (unsigned pixel = 0; pixel < 16; ++pixel) {
            const std::array<uint8_t,4> pre{11,73,199,255}, ui{9,35,57,96}, gen{41,93,217,255}, real{47,101,223,255};
            std::copy(pre.begin(),pre.end(),images[before_image].begin()+pixel*4);
            std::copy(ui.begin(),ui.end(),images[ui_image].begin()+pixel*4);
            std::copy(gen.begin(),gen.end(),images[generated_image].begin()+pixel*4);
            std::copy(real.begin(),real.end(),images[real_image].begin()+pixel*4);
        }
        options(destination);
        for (unsigned failure = 1; failure <= 4; ++failure) {
            fail_allocation = allocation_calls + failure;
            {
                heaven::FrameGenerationCapture capture;
                // Implementations may allocate input or pair storage lazily.
                const bool ready = prepare(capture, 1);
                if (ready) {
                    require(capture.record_ui_inputs(commands, before_image, ui_image), "Could not record staged input");
                    require(capture.commit_ui_inputs(1), "Could not commit staged input");
                    require(!capture.begin_pair(device, physical, size, VK_FORMAT_B8G8R8A8_UNORM, 1), "Injected allocation failure ignored");
                }
                require(!capture.last_error().empty(), "Allocation failure lost diagnostic reason");
                capture.release_after_drain();
            }
            no_live_resources(); fail_allocation = 0;
        }
        // Independent size accounting includes actual memory requirements,
        // rather than only the smaller logical image payload.
        requirement_padding = 128ull * 1024 * 1024;
        {
            heaven::FrameGenerationCapture capture;
            require(!prepare(capture, 1) && !capture.last_error().empty(), "Native allocation cap ignored");
        }
        requirement_padding = 0; no_live_resources();
        // A failed fence must publish no source receipt and must retain its
        // recorded storage until an explicit drain, even when capture disables.
        const auto fence_destination = destination / "failed-fence";
        options(fence_destination); fail_wait = true;
        {
            heaven::FrameGenerationCapture capture;
            record(capture, 1);
            require(capture.begin_pair(device, physical, size, VK_FORMAT_B8G8R8A8_UNORM, 1), "Fence setup failed");
            capture.record_generated(commands, generated_image); capture.record_real(commands, real_image);
            require(!capture.finish_pair(fence, 10, 20) && !capture.enabled() && !capture.last_error().empty(), "Fence failure accepted");
            require(fs::is_empty(fence_destination) && !buffers.empty(), "Failed fence published data or freed in-flight storage");
            capture.release_after_drain();
        }
        fail_wait = false; no_live_resources();
        const auto export_destination = destination / "failed-export";
        options(export_destination);
        {
            heaven::FrameGenerationCapture capture;
            record(capture, 1);
            require(capture.begin_pair(device, physical, size, VK_FORMAT_B8G8R8A8_UNORM, 1), "Export setup failed");
            capture.record_generated(commands, generated_image); capture.record_real(commands, real_image);
            require(fs::remove(export_destination), "Export failure fixture directory was not empty");
            std::ofstream(export_destination) << "fixture blocks directory output";
            require(!capture.finish_pair(fence, 10, 20) && !capture.enabled() && !capture.last_error().empty(), "Export failure accepted");
            require(fs::is_regular_file(export_destination), "Export failure fixture unexpectedly changed");
            capture.release_after_drain();
        }
        no_live_resources();
        const unsigned successful_waits_start = waits;
        options(destination);
        {
            heaven::FrameGenerationCapture capture;
            require(prepare(capture, 1), "Initial input setup failed");
            require(capture.record_ui_inputs(commands, before_image, ui_image), "Initial input record failed");
            require(!capture.commit_ui_inputs(2), "Mismatched source committed");
            require(!capture.enabled() && !capture.last_error().empty(), "Mismatched commit lost failure state");
            capture.release_after_drain();
        }
        no_live_resources();
        {
            heaven::FrameGenerationCapture capture;
            require(prepare(capture, 1), "Abandoned input setup failed");
            require(capture.record_ui_inputs(commands, before_image, ui_image), "Abandoned input record failed");
            capture.abandon_ui_inputs(1);
            for (uint64_t source = 2; source <= 5; ++source) pair(capture, source);
            require(!capture.enabled() && !capture.wants_source(6) && !prepare(capture, 6), "Four-pair capture bound failed");
            capture.release_after_drain();
        }
        no_live_resources();
        unsigned receipts = 0, ppms = 0, pams = 0;
        const std::string ppm_header = "P6\n4 4\n255\n";
        for (const auto& entry : fs::directory_iterator(destination)) {
            const auto path = entry.path();
            if (path.filename() == "failed-fence" || path.filename() == "failed-export") continue;
            const auto data = contents(path);
            if (path.extension() == ".json") {
                ++receipts;
                require(data.find("\"ui_inputs_source_frame\"") != std::string::npos, "Input source metadata missing");
                require(data.find("source1-") == std::string::npos, "Abandoned source exported");
            } else if (path.extension() == ".ppm") {
                ++ppms; require(data.size() == ppm_header.size()+48 && data.compare(0,ppm_header.size(),ppm_header)==0, "Invalid native PPM");
                const auto offset = ppm_header.size(); const bool pre = path.string().find("before-ui") != std::string::npos;
                const bool real = path.string().find("-real.ppm") != std::string::npos;
                const std::array<uint8_t,3> expected = pre ? std::array<uint8_t,3>{11,73,199} :
                    real ? std::array<uint8_t,3>{223,101,47} : std::array<uint8_t,3>{217,93,41};
                for (unsigned i=0;i<48;++i) require(uint8_t(data[offset+i])==expected[i%3], "RGBA/BGRA capture bytes differ");
            } else if (path.extension() == ".pam") {
                ++pams; const auto end = data.find("ENDHDR\n"); require(end != std::string::npos, "Invalid UI PAM header");
                const auto offset=end+7; require(data.size()==offset+64, "UI PAM alpha/payload lost");
                for(unsigned i=0;i<64;++i) require(uint8_t(data[offset+i])==std::array<uint8_t,4>{9,35,57,96}[i%4], "UI RGBA bytes differ");
            } else require(false,"Unexpected partial or extra capture file");
        }
        require(receipts == 4 && ppms == 12 && pams == 4 && waits - successful_waits_start == 4, "Capture file/fence counts differ");
        std::printf("Capture contract PASS: disabled/gated paths, options, allocation failures/cap, staged source isolation, fence/export failures, four fenced pairs, RGBA/BGRA/PAM bytes; no GPU execution\n");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Capture contract FAIL: %s\n",error.what()); return 1;
    }
}
