#include "frame_generation_capture.h"
#include "test_fake_handle.h"

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
using heaven::test::fake_handle;
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
VKAPI_ATTR void VKAPI_CALL physical_memory([[maybe_unused]] VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    ++api_calls; *out = {}; out->memoryTypeCount = 1;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL create_buffer([[maybe_unused]] VkDevice device, const VkBufferCreateInfo* info, [[maybe_unused]] const VkAllocationCallbacks* allocator, VkBuffer* out) {
    ++api_calls; *out = fake_handle<VkBuffer>(next_handle++); buffers.emplace(*out, Buffer{info->size}); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL buffer_requirements([[maybe_unused]] VkDevice device, VkBuffer buffer, VkMemoryRequirements* out) {
    ++api_calls; *out = {buffers.at(buffer).size + requirement_padding, 1, 1};
}
VKAPI_ATTR VkResult VKAPI_CALL allocate_memory([[maybe_unused]] VkDevice device, const VkMemoryAllocateInfo* info, [[maybe_unused]] const VkAllocationCallbacks* allocator, VkDeviceMemory* out) {
    ++api_calls; ++allocation_calls;
    if (allocation_calls == fail_allocation) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = fake_handle<VkDeviceMemory>(next_handle++);
    // Only small byte fixtures are mapped; large-limit cases never allocate a
    // real large host array. Size accounting follows the fake native request.
    memories.emplace(*out, Memory{info->allocationSize, std::vector<uint8_t>(std::min<VkDeviceSize>(info->allocationSize, 4096))});
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL bind_buffer([[maybe_unused]] VkDevice device, VkBuffer buffer, VkDeviceMemory memory, [[maybe_unused]] VkDeviceSize offset) {
    ++api_calls; buffers.at(buffer).memory = memory; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_buffer([[maybe_unused]] VkDevice device, VkBuffer buffer, [[maybe_unused]] const VkAllocationCallbacks* allocator) {
    ++api_calls; require(buffers.erase(buffer) == 1, "Destroyed unknown or duplicate capture buffer");
}
VKAPI_ATTR void VKAPI_CALL free_memory([[maybe_unused]] VkDevice device, VkDeviceMemory memory, [[maybe_unused]] const VkAllocationCallbacks* allocator) {
    ++api_calls; require(memories.erase(memory) == 1, "Freed unknown or duplicate capture memory");
}
VKAPI_ATTR void VKAPI_CALL copy_image([[maybe_unused]] VkCommandBuffer commandBuffer, VkImage image, VkImageLayout layout, VkBuffer buffer, uint32_t count, const VkBufferImageCopy* regions) {
    ++api_calls; ++copies;
    require(layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && count == 1, "Invalid capture copy contract");
    const size_t bytes = size_t(regions[0].imageExtent.width) * regions[0].imageExtent.height * 4;
    require(images.at(image).size() == bytes, "Capture image extent mismatch");
    memories.at(buffers.at(buffer).memory).bytes = images.at(image);
}
VKAPI_ATTR void VKAPI_CALL barrier([[maybe_unused]] VkCommandBuffer commandBuffer, [[maybe_unused]] VkPipelineStageFlags srcStageMask, [[maybe_unused]] VkPipelineStageFlags dstStageMask, [[maybe_unused]] VkDependencyFlags dependencyFlags, [[maybe_unused]] uint32_t memoryBarrierCount, [[maybe_unused]] const VkMemoryBarrier* memoryBarriers, [[maybe_unused]] uint32_t bufferMemoryBarrierCount, [[maybe_unused]] const VkBufferMemoryBarrier* bufferMemoryBarriers, [[maybe_unused]] uint32_t imageMemoryBarrierCount, [[maybe_unused]] const VkImageMemoryBarrier* imageMemoryBarriers) { ++api_calls; }
VKAPI_ATTR VkResult VKAPI_CALL wait([[maybe_unused]] VkDevice device, [[maybe_unused]] uint32_t fenceCount, [[maybe_unused]] const VkFence* fences, [[maybe_unused]] VkBool32 waitAll, [[maybe_unused]] uint64_t timeout) {
    ++api_calls; ++waits; return fail_wait ? VK_TIMEOUT : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL map([[maybe_unused]] VkDevice device, VkDeviceMemory memory, [[maybe_unused]] VkDeviceSize offset, [[maybe_unused]] VkDeviceSize size, [[maybe_unused]] VkMemoryMapFlags flags, void** out) {
    ++api_calls; *out = memories.at(memory).bytes.data(); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL unmap([[maybe_unused]] VkDevice device, [[maybe_unused]] VkDeviceMemory memory) { ++api_calls; }
VKAPI_ATTR VkResult VKAPI_CALL invalidate([[maybe_unused]] VkDevice device, [[maybe_unused]] uint32_t rangeCount, [[maybe_unused]] const VkMappedMemoryRange* ranges) {
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
void options(const fs::path& destination, const std::string& start = "1", bool ui = true,
        const std::string& pair_limit = "") {
    environment("HEAVEN_FG_CAPTURE_DIR", destination.string());
    environment("HEAVEN_FG_CAPTURE_START_SOURCE", start);
    environment("HEAVEN_FG_CAPTURE_UI_INPUTS", ui ? "1" : "");
    environment("HEAVEN_FG_CAPTURE_PAIRS", pair_limit);
}
constexpr VkExtent2D size{4,4};
const auto device = fake_handle<VkDevice>(1);
const auto physical = fake_handle<VkPhysicalDevice>(2);
const auto commands = fake_handle<VkCommandBuffer>(3);
const auto fence = fake_handle<VkFence>(4);
const auto before_image = fake_handle<VkImage>(5), ui_image = fake_handle<VkImage>(6);
const auto generated_image = fake_handle<VkImage>(7), real_image = fake_handle<VkImage>(8);
bool prepare(heaven::FrameGenerationCapture& capture, uint64_t source, VkExtent2D extent = size) {
    return capture.prepare_ui_inputs(device, physical, extent,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, source);
}
void record(heaven::FrameGenerationCapture& capture, uint64_t source) {
    require(prepare(capture, source), "UI input preparation failed");
    require(capture.record_ui_inputs(commands, before_image, ui_image), "UI input copies failed");
    require(capture.commit_ui_inputs(source), "UI source commit failed");
}
void pair(heaven::FrameGenerationCapture& capture, uint64_t source, bool ui = true) {
    if (ui) record(capture, source);
    require(capture.begin_pair(device, physical, size, VK_FORMAT_B8G8R8A8_UNORM, source), "Matching pair rejected");
    capture.record_generated(commands, generated_image); capture.record_real(commands, real_image);
    require(capture.finish_pair(fence, 10, 20), "Pair completion failed");
}
void verify_pair_metadata(const fs::path& directory, unsigned expected_limit, bool ui) {
    unsigned receipts = 0, images_count = 0;
    std::vector<bool> indices(expected_limit);
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto& path = entry.path();
        if (path.extension() == ".json") {
            const auto data = contents(path);
            const auto pair_begin = path.filename().string().find("-pair") + 5;
            const auto pair_index = std::stoul(path.filename().string().substr(pair_begin));
            require(pair_index < expected_limit && !indices[pair_index], "Pair index duplicated or exceeds configured limit");
            indices[pair_index] = true;
            require(data.find("\"configured_pair_limit\": " + std::to_string(expected_limit) + ",") != std::string::npos,
                "Configured pair limit missing from receipt");
            require(data.find("\"pair_index\": " + std::to_string(pair_index) + ",") != std::string::npos,
                "Receipt pair index does not match filename");
            require((data.find("ui_inputs_source_frame") != std::string::npos) == ui,
                "Configured capture changed UI-input mode");
            ++receipts;
        } else {
            require(path.extension() == ".ppm" || (ui && path.extension() == ".pam"),
                "Configured capture produced a partial or unexpected file");
            ++images_count;
        }
    }
    require(receipts == expected_limit && images_count == expected_limit * (ui ? 4u : 2u),
        "Configured capture file count differs");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) { std::fprintf(stderr, "Pass a writable temporary test directory\n"); return 2; }
        const fs::path destination = fs::path(argv[1]) / std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        require(!fs::exists(destination), "Refusing to overwrite a capture-contract directory");
        install_fakes();
        environment("HEAVEN_FG_CAPTURE_DIR", ""); environment("HEAVEN_FG_CAPTURE_UI_INPUTS", "1");
        environment("HEAVEN_FG_CAPTURE_START_SOURCE", "bad");
        environment("HEAVEN_FG_CAPTURE_PAIRS", "invalid");
        {
            heaven::FrameGenerationCapture capture;
            require(!capture.enabled() && !capture.wants_source(1) && !prepare(capture, 1), "Capture active without directory");
        }
        for (const auto* invalid : {"0", "-1", "+1", " 1", "1 ", "1x", "1000001", "18446744073709551616"}) {
            options(destination, invalid);
            heaven::FrameGenerationCapture capture;
            require(!capture.enabled() && !capture.last_error().empty(), "Invalid source gate accepted or silently ignored");
        }
        for (const auto* invalid : {"0", "33", "4294967296", "18446744073709551616", "-1", "+1", " 1", "1 ",
                "1x", "01", "032", "004", "00", "bad"}) {
            options(destination, "1", true, invalid);
            heaven::FrameGenerationCapture capture;
            require(!capture.enabled() && !capture.wants_source(1) && !prepare(capture, 1) &&
                capture.last_error().find("HEAVEN_FG_CAPTURE_PAIRS") != std::string::npos,
                "Invalid pair limit accepted or silently ignored");
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
            const auto pixel_offset = static_cast<std::ptrdiff_t>(pixel) * 4;
            std::copy(pre.begin(),pre.end(),images[before_image].begin()+pixel_offset);
            std::copy(ui.begin(),ui.end(),images[ui_image].begin()+pixel_offset);
            std::copy(gen.begin(),gen.end(),images[generated_image].begin()+pixel_offset);
            std::copy(real.begin(),real.end(),images[real_image].begin()+pixel_offset);
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
            const auto& path = entry.path();
            if (path.filename() == "failed-fence" || path.filename() == "failed-export") continue;
            const auto data = contents(path);
            if (path.extension() == ".json") {
                ++receipts;
                require(data.find("\"ui_inputs_source_frame\"") != std::string::npos, "Input source metadata missing");
                require(data.find("source1-") == std::string::npos, "Abandoned source exported");
            } else if (path.extension() == ".ppm") {
                ++ppms; require(data.size() == ppm_header.size()+48 && data.starts_with(ppm_header), "Invalid native PPM");
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
        for (const auto& entry : fs::directory_iterator(destination)) {
            if (entry.path().extension() != ".json") continue;
            const auto data = contents(entry.path());
            require(data.find("\"configured_pair_limit\": 4,") != std::string::npos,
                "Default pair limit changed or receipt omitted it");
            const auto name = entry.path().filename().string();
            const auto pair_index = std::stoul(name.substr(name.find("-pair") + 5));
            require(data.find("\"pair_index\": " + std::to_string(pair_index) + ",") != std::string::npos,
                "Default receipt pair index differs");
        }
        // The option is captured once per diagnostic instance. All pairs reuse
        // two/four buffers; an explicit drain frees storage, not the completed
        // pair budget. No Vulkan work may happen after that budget is exhausted.
        for (const auto limit : {1u, 32u}) for (const bool ui : {false, true}) {
            const auto configured_destination = destination / ("limit-" + std::to_string(limit) + (ui ? "-ui" : "-plain"));
            options(configured_destination, "1", ui, std::to_string(limit));
            const auto allocations_before = allocation_calls;
            const auto waits_before = waits;
            {
                heaven::FrameGenerationCapture capture;
                environment("HEAVEN_FG_CAPTURE_PAIRS", "invalid-after-construction");
                for (uint64_t source = 1; source <= limit; ++source) {
                    require(capture.enabled() && capture.wants_source(source), "Configured capture stopped before its pair limit");
                    pair(capture, source, ui);
                    if (source == 16) {
                        capture.release_after_drain();
                        no_live_resources();
                        require(capture.enabled() && capture.wants_source(17), "Drain disabled unfinished configured capture");
                    }
                }
                const auto calls_at_limit = api_calls;
                require(!capture.enabled() && !capture.wants_source(limit + 1) && !prepare(capture, limit + 1) &&
                    !capture.begin_pair(device, physical, size, VK_FORMAT_B8G8R8A8_UNORM, limit + 1),
                    "Configured capture exceeded its pair limit");
                require(api_calls == calls_at_limit && capture.last_error().empty(), "Exhausted capture called Vulkan or failed provider");
                capture.release_after_drain();
                require(!capture.enabled() && !capture.wants_source(limit + 1), "Drain reset completed capture budget");
            }
            no_live_resources();
            require(waits - waits_before == limit, "Configured capture fence count differs");
            require(allocation_calls - allocations_before == (ui ? 4u : 2u) * (limit == 32 ? 2u : 1u),
                "Configured pairs did not reuse bounded staging storage");
            verify_pair_metadata(configured_destination, limit, ui);
        }
        std::printf("Capture contract PASS: disabled/gated paths, canonical1..32/default4 options, allocation failures/cap, staged source isolation, fence/export failures, reused buffers/drain budgets, RGBA/BGRA/PAM bytes; no GPU execution\n");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Capture contract FAIL: %s\n",error.what()); return 1;
    } catch (...) {
        std::fprintf(stderr, "Capture contract FAIL: unknown exception\n"); return 1;
    }
}
