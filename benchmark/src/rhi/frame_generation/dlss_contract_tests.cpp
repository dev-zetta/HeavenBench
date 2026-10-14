// GPU-independent contracts for the optional DLSS adapter. Build this together
// with dlss_provider.cpp and dlss_runtime_loader.cpp, defining
// HEAVEN_DLSS_CONTRACT_TEST_MAIN, for a standalone host acceptance executable.
#include "dlss_input_validation.h"
#include "dlss_device_creation.h"

#include <cstddef>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <numbers>
#include <type_traits>

namespace heaven {
namespace {

template <class T> T handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) {
        if (!value) return nullptr;
        static std::array<std::max_align_t, 8> identities{};
        return reinterpret_cast<T>(&identities.at(value));
    }
    else return static_cast<T>(value);
}

DlssImage test_image(uint32_t width, uint32_t height, VkFormat format) {
    DlssImage image;
    // Opaque non-null values exercise metadata validation only. No test may
    // send these to a native Vulkan or Streamline function.
    image.image = handle<VkImage>(1);
    image.view = handle<VkImageView>(2);
    image.memory = handle<VkDeviceMemory>(3);
    image.width = width;
    image.height = height;
    image.format = format;
    image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    return image;
}

DlssPresentInputs valid_inputs() {
    DlssPresentInputs input;
    input.token.native_token = handle<void*>(4);
    input.token.application_frame = 7;
    input.token.stream_frame = 7;
    input.depth = test_image(1280, 720, VK_FORMAT_R32_SFLOAT);
    input.motion = test_image(1280, 720, VK_FORMAT_R16G16_SFLOAT);
    input.hudless_color = test_image(1920, 1080, VK_FORMAT_R16G16B16A16_SFLOAT);
    input.ui_color_alpha = test_image(1920, 1080, VK_FORMAT_R8G8B8A8_UNORM);
    input.backbuffer_format = VK_FORMAT_B8G8R8A8_UNORM;
    input.backbuffer_width = 1920;
    input.backbuffer_height = 1080;
    input.backbuffer_count = 3;
    input.backbuffer_has_ui = true;
    input.temporal_coverage_complete = true;
    input.color_space_matches_backbuffer = true;
    input.inputs_valid_until_present = true;
    input.all_input_writes_on_presenting_queue = true;
    constexpr std::array<float, 16> identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    input.camera.view_to_clip = identity;
    input.camera.clip_to_view = identity;
    input.camera.clip_to_previous_clip = identity;
    input.camera.previous_clip_to_clip = identity;
    return input;
}

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "[dlss_contract] %s\n", message);
    return condition;
}

struct DeviceCreationHarness {
    VkInstance instance = handle<VkInstance>(1);
    VkPhysicalDevice selected = handle<VkPhysicalDevice>(2);
    VkPhysicalDevice other = handle<VkPhysicalDevice>(3);
    VkDevice device = handle<VkDevice>(4);
    VkDeviceCreateInfo info{};
    VkResult probe_result = VK_SUCCESS;
    VkResult fetch_result = VK_SUCCESS;
    VkResult create_result = VK_SUCCESS;
    uint32_t reported_count = 2;
    unsigned incomplete_probes = 0, incomplete_fetches = 0;
    unsigned probes = 0, fetches = 0, creates = 0;
    bool include_selected = true, parent_registered = false, arguments_match = true;
};
DeviceCreationHarness* creation_harness = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL enumerate_for_creation(VkInstance instance,
        uint32_t* count, VkPhysicalDevice* devices) {
    auto& h = *creation_harness;
    h.arguments_match &= instance == h.instance && count != nullptr;
    if (!devices) {
        ++h.probes;
        *count = h.reported_count;
        if (h.incomplete_probes) { --h.incomplete_probes; return VK_INCOMPLETE; }
        return h.probe_result;
    }
    ++h.fetches;
    h.arguments_match &= *count == 2;
    devices[0] = h.other;
    devices[1] = h.include_selected ? h.selected : h.other;
    h.parent_registered = h.include_selected;
    *count = 2;
    if (h.incomplete_fetches) { --h.incomplete_fetches; return VK_INCOMPLETE; }
    return h.fetch_result;
}

VKAPI_ATTR VkResult VKAPI_CALL create_after_enumeration(VkPhysicalDevice physical,
        const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* device) {
    auto& h = *creation_harness;
    ++h.creates;
    h.arguments_match &= physical == h.selected && info == &h.info && allocator == nullptr;
    // This is the exact parent-map dependency in pinned wrapper.cpp:992/1003.
    // Count-only enumeration does not register the selected physical device.
    if (!h.parent_registered) return VK_ERROR_INITIALIZATION_FAILED;
    if (h.create_result == VK_SUCCESS) *device = h.device;
    return h.create_result;
}

bool run_dlss_device_creation_tests() {
    const auto invoke = [](DeviceCreationHarness& h, VkDevice& device) {
        creation_harness = &h;
        return dlss_device_creation_detail::create_device(h.instance, h.selected, h.info,
            enumerate_for_creation, create_after_enumeration, &device);
    };
    bool success = true;
    {
        DeviceCreationHarness h;
        VkDevice device = VK_NULL_HANDLE;
        success &= check(invoke(h, device) == VK_SUCCESS && device == h.device &&
            h.probes == 1 && h.fetches == 1 && h.creates == 1 && h.arguments_match,
            "creation proxy did not establish the selected physical-device parent before create");
    }
    for (const bool incomplete_probe : {false, true}) {
        DeviceCreationHarness h;
        if (incomplete_probe) h.incomplete_probes = 1; else h.incomplete_fetches = 1;
        VkDevice device = VK_NULL_HANDLE;
        success &= check(invoke(h, device) == VK_SUCCESS && device == h.device &&
            h.probes == 2 && h.fetches == (incomplete_probe ? 1u : 2u) &&
            h.creates == 1 && h.arguments_match, "incomplete enumeration did not retry before creation");
    }
    for (const bool incomplete_probe : {false, true}) {
        DeviceCreationHarness h;
        if (incomplete_probe) h.incomplete_probes = 8; else h.incomplete_fetches = 8;
        VkDevice device = h.device;
        success &= check(invoke(h, device) == VK_INCOMPLETE && device == VK_NULL_HANDLE &&
            h.probes == 4 && h.fetches == (incomplete_probe ? 0u : 4u) &&
            h.creates == 0, "unstable enumeration escaped the bounded retry limit");
    }
    for (const bool probe_failure : {false, true}) {
        DeviceCreationHarness h;
        if (probe_failure) h.probe_result = VK_ERROR_INITIALIZATION_FAILED;
        else h.fetch_result = VK_ERROR_DEVICE_LOST;
        const auto expected = probe_failure ? h.probe_result : h.fetch_result;
        VkDevice device = h.device;
        success &= check(invoke(h, device) == expected && device == VK_NULL_HANDLE &&
            h.probes == 1 && h.fetches == (probe_failure ? 0u : 1u) && h.creates == 0,
            "enumeration failure still invoked create or lost the original error");
    }
    {
        DeviceCreationHarness h;
        h.include_selected = false;
        VkDevice device = h.device;
        success &= check(invoke(h, device) == VK_ERROR_INITIALIZATION_FAILED &&
            device == VK_NULL_HANDLE && h.creates == 0,
            "selected physical device absent from SDK enumeration was still created");
    }
    for (const uint32_t count : {0u, 4097u}) {
        DeviceCreationHarness h;
        h.reported_count = count;
        VkDevice device = h.device;
        success &= check(invoke(h, device) == VK_ERROR_INITIALIZATION_FAILED &&
            device == VK_NULL_HANDLE && h.fetches == 0 && h.creates == 0,
            "invalid enumeration count allocated or invoked create");
    }
    {
        DeviceCreationHarness h;
        h.create_result = VK_ERROR_FEATURE_NOT_PRESENT;
        VkDevice device = h.device;
        success &= check(invoke(h, device) == h.create_result && device == VK_NULL_HANDLE &&
            h.creates == 1 && h.arguments_match, "creation proxy failure was not preserved");
    }
    creation_harness = nullptr;
    return success;
}

bool run_dlss_contract_tests() {
    const auto valid = valid_inputs();
    const auto accepts = [](const DlssPresentInputs& input) {
        return validate_dlss_present_inputs(input, 64, 1).empty();
    };
    bool success = run_dlss_device_creation_tests();
    success &= check(accepts(valid), "complete SDR temporal input was rejected");
    auto reject_change = [&](auto change, const char* label) {
        auto input = valid;
        change(input);
        success &= check(!accepts(input), label);
    };
    reject_change([](auto& in) { in.token = {}; }, "missing token accepted");
    reject_change([](auto& in) { in.temporal_coverage_complete = false; },
        "camera-only motion accepted as complete deformation history");
    reject_change([](auto& in) { in.inputs_valid_until_present = false; },
        "resources that expire before asynchronous presentation accepted");
    reject_change([](auto& in) { in.all_input_writes_on_presenting_queue = false; },
        "unsynchronized cross-queue resource reuse accepted");
    reject_change([](auto& in) { in.color_space_matches_backbuffer = false; },
        "mismatched HUDless/backbuffer color domain accepted");
    reject_change([](auto& in) { in.vsync = true; }, "unsupported Vulkan VSync accepted");
    reject_change([](auto& in) { in.hdr = true; }, "unqualified HDR accepted");
    reject_change([](auto& in) { in.frames_to_generate = 0; }, "zero generated frames accepted");
    reject_change([](auto& in) { in.frames_to_generate = 2; }, "unsupported GPU multiplier accepted");
    reject_change([](auto& in) { in.backbuffer_count = 0; }, "missing swapchain accepted");
    reject_change([](auto& in) { --in.backbuffer_width; }, "mismatched swapchain dimensions accepted");
    reject_change([](auto& in) { in.depth.memory = VK_NULL_HANDLE; }, "missing Vulkan allocation accepted");
    reject_change([](auto& in) { in.motion.layout = VK_IMAGE_LAYOUT_UNDEFINED; },
        "uninitialized motion image accepted");
    reject_change([](auto& in) { in.motion.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; },
        "unsampleable motion image accepted");
    reject_change([](auto& in) { in.depth.array_layers = 2; }, "layered depth image accepted");
    reject_change([](auto& in) { --in.motion.height; }, "mismatched depth/motion dimensions accepted");
    reject_change([](auto& in) { in.motion.format = VK_FORMAT_R16G16_UNORM; },
        "unsigned motion vectors accepted");
    reject_change([](auto& in) { in.ui_color_alpha = {}; }, "UI-bearing backbuffer without UI layer accepted");
    reject_change([](auto& in) { --in.ui_color_alpha.width; }, "mismatched UI dimensions accepted");
    reject_change([](auto& in) { in.camera.motion_scale[0] = 0; }, "zero motion scale accepted");
    reject_change([](auto& in) { in.camera.near_plane = in.camera.far_plane; },
        "invalid depth reconstruction accepted");
    reject_change([](auto& in) { in.camera.vertical_fov_radians = std::numbers::pi_v<float>; },
        "pi-radian field of view accepted");
    reject_change([](auto& in) {
        in.camera.vertical_fov_radians = std::nextafter(std::numbers::pi_v<float>,
            std::numeric_limits<float>::infinity());
    }, "field of view above pi accepted");
    auto near_pi = valid;
    near_pi.camera.vertical_fov_radians = std::nextafter(std::numbers::pi_v<float>, 0.0f);
    success &= check(accepts(near_pi), "field of view immediately below pi rejected");
    reject_change([](auto& in) { in.camera.clip_to_previous_clip = {}; },
        "uninitialized temporal transform accepted");
    reject_change([](auto& in) {
        in.camera.view_to_clip[7] = std::numeric_limits<float>::quiet_NaN();
    }, "non-finite projection accepted");
    auto hudless = valid;
    hudless.backbuffer_has_ui = false;
    hudless.ui_color_alpha = {};
    success &= check(accepts(hudless), "genuinely HUDless presentation incorrectly needs UI");
    auto full_resolution = valid;
    full_resolution.depth.width = full_resolution.motion.width = 1920;
    full_resolution.depth.height = full_resolution.motion.height = 1080;
    full_resolution.motion.format = VK_FORMAT_R32G32_SFLOAT;
    success &= check(accepts(full_resolution), "full-resolution signed floating motion was rejected");
    success &= check(!validate_dlss_present_inputs(valid, 1081, 1).empty(),
        "SDK minimum output dimension ignored");

    // These lifecycle checks are valid in every build. No bootstrap or native
    // handle is supplied, so an unavailable adapter must remain inactive.
    DlssProvider provider;
    DlssFrameToken token{handle<void*>(4), 7, 7, {}};
    success &= check(!provider.status().available && !provider.status().enabled,
        "new provider claims generation is active");
    success &= check(!provider.begin_frame(token) && !token.native_token,
        "uninitialized provider created a usable frame token");
    success &= check(!provider.bind_device({}) && !provider.prepare_present(valid),
        "uninitialized provider accepted native device or presentation");
    success &= check(provider.instance_hook(VK_NULL_HANDLE, "vkCreateInstance") == nullptr &&
        provider.device_hook(VK_NULL_HANDLE, "vkCreateDevice") == nullptr,
        "manual adapter intercepted native creation without owner requirements");
    success &= check(provider.enumerate_physical_devices_proxy(VK_NULL_HANDLE) == nullptr,
        "inactive creation path exposed an SDK physical-device enumeration proxy");
    success &= check(provider.suspend() && provider.suspend(), "inactive suspension was not idempotent");
    provider.shutdown_after_drain();
    provider.shutdown_after_drain();
    success &= check(!provider.status().initialized && !provider.status().enabled &&
        !provider.status().available && !provider.mark({}, DlssLatencyMarker::PresentStart),
        "shutdown retained active generation or accepted invalid markers");
    return success;
}

} // namespace
} // namespace heaven

#ifdef HEAVEN_DLSS_CONTRACT_TEST_MAIN
int main() {
    try {
        const bool passed = heaven::run_dlss_contract_tests();
        std::printf("DLSS host contracts: %s\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DLSS host contracts: exception: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DLSS host contracts: unknown exception\n");
        return 1;
    }
}
#endif
