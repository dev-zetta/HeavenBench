#include "dlss_provider.h"
#include "dlss_input_validation.h"
#include "dlss_runtime_loader.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <system_error>
#include <type_traits>

#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_helpers_vk.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

static_assert(SL_VERSION_MAJOR == 2 && SL_VERSION_MINOR == 14 && SL_VERSION_PATCH == 1,
        "HeavenBench DLSS adapter requires the reviewed Streamline 2.14.1 headers");
static_assert(sl::MAX_FRAMES_IN_FLIGHT == 6,
        "HeavenBench token reservations require the pinned six-slot Streamline ring");
#endif

namespace heaven {
namespace {

constexpr std::array instance_hooks{"vkCreateWin32SurfaceKHR", "vkDestroySurfaceKHR"};
constexpr std::array device_hooks{"vkCreateSwapchainKHR", "vkDestroySwapchainKHR",
    "vkGetSwapchainImagesKHR", "vkAcquireNextImageKHR", "vkQueuePresentKHR", "vkDeviceWaitIdle"};

[[maybe_unused]] bool contains_hook(std::span<const char* const> hooks, const char* name) {
    return name && std::ranges::any_of(hooks,
        [name](const char* hook) { return std::strcmp(name, hook) == 0; });
}

#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
template <class T> void* native_pointer(T handle) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<void*>(handle);
    else return reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
}

sl::Resource make_resource(const DlssImage& image) {
    sl::Resource result{sl::ResourceType::eTex2d, native_pointer(image.image),
        native_pointer(image.memory), native_pointer(image.view), static_cast<uint32_t>(image.layout)};
    result.width = image.width;
    result.height = image.height;
    result.nativeFormat = static_cast<uint32_t>(image.format);
    result.mipLevels = image.mip_levels;
    result.arrayLayers = image.array_layers;
    result.flags = image.flags;
    result.usage = image.usage;
    return result;
}

sl::float4x4 make_matrix(const std::array<float, 16>& matrix) {
    sl::float4x4 result;
    for (uint32_t row = 0; row < 4; ++row) {
        result[row] = {matrix[row * 4], matrix[row * 4 + 1],
            matrix[row * 4 + 2], matrix[row * 4 + 3]};
    }
    return result;
}

sl::Constants make_constants(const DlssCameraConstants& camera) {
    sl::Constants result;
    result.cameraViewToClip = make_matrix(camera.view_to_clip);
    result.clipToCameraView = make_matrix(camera.clip_to_view);
    result.clipToPrevClip = make_matrix(camera.clip_to_previous_clip);
    result.prevClipToClip = make_matrix(camera.previous_clip_to_clip);
    const std::array<float, 16> identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    result.clipToLensClip = make_matrix(identity);
    result.jitterOffset = {camera.jitter_pixels[0], camera.jitter_pixels[1]};
    result.mvecScale = {camera.motion_scale[0], camera.motion_scale[1]};
    result.cameraPinholeOffset = {0.0f, 0.0f};
    result.cameraPos = {camera.position[0], camera.position[1], camera.position[2]};
    result.cameraUp = {camera.up[0], camera.up[1], camera.up[2]};
    result.cameraRight = {camera.right[0], camera.right[1], camera.right[2]};
    result.cameraFwd = {camera.forward[0], camera.forward[1], camera.forward[2]};
    result.cameraNear = camera.near_plane;
    result.cameraFar = camera.far_plane;
    result.cameraFOV = camera.vertical_fov_radians;
    result.cameraAspectRatio = camera.aspect_ratio;
    result.depthInverted = camera.depth_inverted ? sl::eTrue : sl::eFalse;
    result.cameraMotionIncluded = sl::eTrue;
    result.motionVectors3D = sl::eFalse;
    result.reset = camera.reset ? sl::eTrue : sl::eFalse;
    result.orthographicProjection = sl::eFalse;
    result.motionVectorsDilated = sl::eFalse;
    result.motionVectorsJittered = camera.motion_jittered ? sl::eTrue : sl::eFalse;
    return result;
}

void append_unique(std::vector<std::string>& into, uint32_t count, const char* const* strings) {
    for (uint32_t i = 0; i < count; ++i) {
        if (strings[i] && std::ranges::find(into, strings[i]) == into.end())
            into.emplace_back(strings[i]);
    }
}

std::vector<const char*> string_pointers(const std::vector<std::string>& strings) {
    std::vector<const char*> result;
    result.reserve(strings.size());
    for (const auto& string : strings) result.push_back(string.c_str());
    return result;
}

std::atomic<int32_t> asynchronous_api_error{VK_SUCCESS};
std::atomic<bool> streamline_owned{false};

void api_error_callback(const sl::APIError& error) {
    // The SDK invokes this on its presentation thread. Defer explanation and
    // recovery to the driver owner; no allocation, locking, logging or GPU wait.
    asynchronous_api_error.store(static_cast<int32_t>(error.vkRes), std::memory_order_relaxed);
}
#endif

} // namespace

struct DlssProvider::Impl {
    DlssTokenReservations token_reservations;
    DlssVulkanRequirements requirements;
    DlssProviderStatus status;
    uint32_t minimum_dimension = 0;
    uint32_t ping_message = 0;
    DlssFrameToken last_token;

#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slGetFeatureRequirements* get_requirements = nullptr;
    PFun_slGetFeatureVersion* get_version = nullptr;
    PFun_slIsFeatureSupported* is_supported = nullptr;
    PFun_slSetVulkanInfo* set_vulkan_info = nullptr;
    PFun_slGetFeatureFunction* get_feature_function = nullptr;
    PFun_slGetNewFrameToken* get_frame_token = nullptr;
    PFun_slSetConstants* set_constants = nullptr;
    PFun_slSetTagForFrame* set_tags = nullptr;
    PFun_slDLSSGSetOptions* set_options = nullptr;
    PFun_slDLSSGGetState* get_state = nullptr;
    PFun_slReflexSetOptions* set_reflex_options = nullptr;
    PFun_slReflexSleep* reflex_sleep = nullptr;
    PFun_slPCLSetMarker* pcl_marker = nullptr;
    PFun_slPCLSetOptions* set_pcl_options = nullptr;
    PFun_slPCLGetState* get_pcl_state = nullptr;
    PFN_vkGetInstanceProcAddr instance_resolver = nullptr;
    PFN_vkGetDeviceProcAddr device_resolver = nullptr;
    sl::ViewportHandle viewport{0};
    sl::DLSSGOptions options;
    std::atomic<int32_t> threaded_result{static_cast<int32_t>(sl::Result::eOk)};
    std::atomic<bool> token_index_exhausted{false};
    std::atomic<bool> frame_available{false};
    bool owns_streamline = false;
    DlssRuntimeLoader loader;

    bool accept(sl::Result result, const char* operation) {
        if (result == sl::Result::eOk) return true;
        status.enabled = false;
        status.available = false;
        frame_available.store(false, std::memory_order_relaxed);
        status.reason = std::string(operation) + ": " + sl::getResultAsStr(result);
        return false;
    }

    template <class T> bool feature_function(sl::Feature feature, const char* name, T*& function) {
        void* address = nullptr;
        if (!accept(get_feature_function(feature, name, address), name)) return false;
        function = reinterpret_cast<T*>(address);
        if (!function) {
            status.reason = std::string("Missing SDK feature entry point: ") + name;
            return false;
        }
        return true;
    }
#endif
};

DlssProvider::DlssProvider() : impl_(std::make_unique<Impl>()) {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE && defined(_WIN32)
    impl_->status.built = true;
    impl_->status.reason = "DLSS requires initialization before Vulkan device creation";
#else
#ifdef _WIN32
    impl_->status.reason = "DLSS support was not built; configure the optional Streamline 2.14.1 SDK";
#else
    impl_->status.reason = "Native DLSS frame generation is available only in the optional Windows build";
#endif
#endif
}

DlssProvider::~DlssProvider() {
    shutdown_after_drain();
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (impl_->owns_streamline) streamline_owned.store(false);
#endif
}

bool DlssProvider::bootstrap([[maybe_unused]] const DlssBootstrapOptions& options) {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (impl_->status.initialized || impl_->loader.loaded()) {
        impl_->status.reason = "Streamline bootstrap can run only once per provider";
        return false;
    }
    bool expected = false;
    if (!streamline_owned.compare_exchange_strong(expected, true)) {
        impl_->status.reason = "Another provider already owns process-global Streamline state";
        return false;
    }
    impl_->owns_streamline = true;
    if (!impl_->loader.load(options.plugin_directory, impl_->status.reason)) {
        streamline_owned.store(false);
        impl_->owns_streamline = false;
        return false;
    }
#define UGX_LOAD_SL(member, symbol) \
    impl_->member = reinterpret_cast<decltype(impl_->member)>(impl_->loader.find_export(#symbol)); \
    if (!impl_->member) { impl_->status.reason = "Missing Streamline export: " #symbol; return false; }
    UGX_LOAD_SL(init, slInit)
    UGX_LOAD_SL(shutdown, slShutdown)
    UGX_LOAD_SL(get_requirements, slGetFeatureRequirements)
    UGX_LOAD_SL(get_version, slGetFeatureVersion)
    UGX_LOAD_SL(is_supported, slIsFeatureSupported)
    UGX_LOAD_SL(set_vulkan_info, slSetVulkanInfo)
    UGX_LOAD_SL(get_feature_function, slGetFeatureFunction)
    UGX_LOAD_SL(get_frame_token, slGetNewFrameToken)
    UGX_LOAD_SL(set_constants, slSetConstants)
    UGX_LOAD_SL(set_tags, slSetTagForFrame)
    UGX_LOAD_SL(instance_resolver, vkGetInstanceProcAddr)
    UGX_LOAD_SL(device_resolver, vkGetDeviceProcAddr)
#undef UGX_LOAD_SL

    const std::array features{sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    const auto plugin_path = options.plugin_directory.wstring();
    const wchar_t* plugin_paths[]{plugin_path.c_str()};
    const auto log_path = options.log_directory.wstring();
    sl::Preferences preferences;
    preferences.featuresToLoad = features.data();
    preferences.numFeaturesToLoad = static_cast<uint32_t>(features.size());
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = log_path.empty() ? nullptr : log_path.c_str();
    preferences.applicationId = options.application_id;
    preferences.projectId = options.project_id.empty() ? nullptr : options.project_id.c_str();
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = options.engine_version.c_str();
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    // Reproducible opt-in SDK loading: no OTA downloads or downloaded plugins.
    preferences.flags = sl::PreferenceFlags::eUseManualHooking |
        sl::PreferenceFlags::eUseFrameBasedResourceTagging |
        sl::PreferenceFlags::eDisableCLStateTracking;
    if (!impl_->accept(impl_->init(preferences, sl::kSDKVersion), "slInit")) return false;
    impl_->status.initialized = true;
    sl::FeatureVersion version;
    if (!impl_->accept(impl_->get_version(sl::kFeatureDLSS_G, version), "slGetFeatureVersion")) return false;
    if (!(version.versionSL == sl::Version{2, 14, 1})) {
        impl_->status.reason = "Loaded Streamline version does not match pinned 2.14.1 headers";
        return false;
    }
    std::vector<std::string> feature_names12, feature_names13;
    for (auto feature : features) {
        sl::FeatureRequirements required;
        if (!impl_->accept(impl_->get_requirements(feature, required), "slGetFeatureRequirements")) return false;
        if (feature == sl::kFeatureDLSS_G &&
                !(required.flags & sl::FeatureRequirementFlags::eVulkanSupported)) {
            impl_->status.reason = "This DLSS-G SDK library does not support Vulkan";
            return false;
        }
        auto& into = impl_->requirements;
        append_unique(into.instance_extensions, required.vkNumInstanceExtensions, required.vkInstanceExtensions);
        append_unique(into.device_extensions, required.vkNumDeviceExtensions, required.vkDeviceExtensions);
        append_unique(feature_names12, required.vkNumFeatures12, required.vkFeatures12);
        append_unique(feature_names13, required.vkNumFeatures13, required.vkFeatures13);
        into.extra_graphics_queues += required.vkNumGraphicsQueuesRequired;
        into.extra_compute_queues += required.vkNumComputeQueuesRequired;
        into.extra_optical_flow_queues += required.vkNumOpticalFlowQueuesRequired;
    }
    auto names12 = string_pointers(feature_names12);
    auto names13 = string_pointers(feature_names13);
    impl_->requirements.features12 = sl::getVkPhysicalDeviceVulkan12Features(
        static_cast<uint32_t>(names12.size()), names12.data());
    impl_->requirements.features13 = sl::getVkPhysicalDeviceVulkan13Features(
        static_cast<uint32_t>(names13.size()), names13.data());
    impl_->status.reason = "Streamline initialized; native Vulkan device/hooks are not connected yet";
    asynchronous_api_error.store(VK_SUCCESS, std::memory_order_relaxed);
    return true;
#else
    return false;
#endif
}

const DlssVulkanRequirements& DlssProvider::requirements() const noexcept { return impl_->requirements; }
std::span<const char* const> DlssProvider::mandatory_instance_hooks() noexcept { return instance_hooks; }
std::span<const char* const> DlssProvider::mandatory_device_hooks() noexcept { return device_hooks; }

PFN_vkVoidFunction DlssProvider::instance_hook([[maybe_unused]] VkInstance instance, [[maybe_unused]] const char* name) const {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (impl_->status.initialized && contains_hook(instance_hooks, name))
        return impl_->instance_resolver(instance, name);
#endif
    return nullptr;
}

PFN_vkVoidFunction DlssProvider::device_hook([[maybe_unused]] VkDevice device, [[maybe_unused]] const char* name) const {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (impl_->status.initialized && contains_hook(device_hooks, name))
        return impl_->device_resolver(device, name);
#endif
    return nullptr;
}

bool DlssProvider::bind_device(const DlssVulkanDevice& device) {
    return bind_device_impl(device, false);
}

PFN_vkCreateInstance DlssProvider::create_instance_proxy() const {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (impl_->status.initialized)
        return reinterpret_cast<PFN_vkCreateInstance>(impl_->instance_resolver(VK_NULL_HANDLE, "vkCreateInstance"));
#endif
    return nullptr;
}

PFN_vkEnumeratePhysicalDevices DlssProvider::enumerate_physical_devices_proxy(
        [[maybe_unused]] VkInstance instance) const {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (impl_->status.initialized)
        return reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            impl_->instance_resolver(instance, "vkEnumeratePhysicalDevices"));
#endif
    return nullptr;
}

PFN_vkCreateDevice DlssProvider::create_device_proxy([[maybe_unused]] VkInstance instance) const {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (impl_->status.initialized)
        return reinterpret_cast<PFN_vkCreateDevice>(impl_->instance_resolver(instance, "vkCreateDevice"));
#endif
    return nullptr;
}

bool DlssProvider::adopt_interposed_device(VkInstance instance, VkPhysicalDevice physical,
        VkDevice device, bool hooks_installed) {
    DlssVulkanDevice info;
    info.instance = instance;
    info.physical_device = physical;
    info.device = device;
    info.hooks_installed = hooks_installed;
    return bind_device_impl(info, true);
}

bool DlssProvider::bind_device_impl([[maybe_unused]] const DlssVulkanDevice& device, [[maybe_unused]] bool automatic_queues) {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (!impl_->status.initialized) return false;
    if (!device.instance || !device.physical_device || !device.device || !device.hooks_installed) {
        impl_->status.reason = "Native Vulkan device or mandatory presentation/surface hooks are unavailable";
        return false;
    }
    for (const auto* name : instance_hooks) if (!instance_hook(device.instance, name)) {
        impl_->status.reason = std::string("Missing Vulkan instance proxy: ") + name; return false;
    }
    for (const auto* name : device_hooks) if (!device_hook(device.device, name)) {
        impl_->status.reason = std::string("Missing Vulkan device proxy: ") + name; return false;
    }
    sl::AdapterInfo adapter;
    adapter.vkPhysicalDevice = device.physical_device;
    for (auto feature : {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL}) {
        if (!impl_->accept(impl_->is_supported(feature, adapter), "slIsFeatureSupported")) return false;
    }
    sl::VulkanInfo info;
    info.device = device.device;
    info.instance = device.instance;
    info.physicalDevice = device.physical_device;
    info.graphicsQueueFamily = device.graphics_family;
    info.graphicsQueueIndex = device.graphics_queue_start;
    info.computeQueueFamily = device.compute_family;
    info.computeQueueIndex = device.compute_queue_start;
    info.opticalFlowQueueFamily = device.optical_flow_family;
    info.opticalFlowQueueIndex = device.optical_flow_queue_start;
    info.useNativeOpticalFlowMode = device.native_optical_flow;
    info.graphicsQueueCreateFlags = device.graphics_queue_flags;
    info.computeQueueCreateFlags = device.compute_queue_flags;
    info.opticalFlowQueueCreateFlags = device.optical_flow_queue_flags;
    if (!automatic_queues && !impl_->accept(impl_->set_vulkan_info(info), "slSetVulkanInfo")) return false;
#define UGX_SL_FEATURE(feature, member, name) \
    if (!impl_->feature_function(feature, #name, impl_->member)) return false;
    UGX_SL_FEATURE(sl::kFeatureDLSS_G, set_options, slDLSSGSetOptions)
    UGX_SL_FEATURE(sl::kFeatureDLSS_G, get_state, slDLSSGGetState)
    UGX_SL_FEATURE(sl::kFeatureReflex, set_reflex_options, slReflexSetOptions)
    UGX_SL_FEATURE(sl::kFeatureReflex, reflex_sleep, slReflexSleep)
    UGX_SL_FEATURE(sl::kFeaturePCL, pcl_marker, slPCLSetMarker)
    UGX_SL_FEATURE(sl::kFeaturePCL, set_pcl_options, slPCLSetOptions)
    UGX_SL_FEATURE(sl::kFeaturePCL, get_pcl_state, slPCLGetState)
#undef UGX_SL_FEATURE
    sl::ReflexOptions reflex;
    reflex.mode = sl::ReflexMode::eLowLatency;
    if (!impl_->accept(impl_->set_reflex_options(reflex), "slReflexSetOptions")) return false;
    sl::PCLOptions pcl;
    if (!impl_->accept(impl_->set_pcl_options(pcl), "slPCLSetOptions")) return false;
    sl::PCLState pcl_state;
    if (!impl_->accept(impl_->get_pcl_state(pcl_state), "slPCLGetState")) return false;
    impl_->ping_message = pcl_state.statsWindowMessage;
    impl_->options.mode = sl::DLSSGMode::eOff;
    impl_->options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    impl_->options.onErrorCallback = api_error_callback;
    if (!impl_->accept(impl_->set_options(impl_->viewport, impl_->options), "slDLSSGSetOptions")) return false;
    if (!collect_state()) return false;
    if (impl_->status.maximum_generated_frames == 0) {
        impl_->status.reason = "DLSS-G reports no supported frame-generation multiplier";
        return false;
    }
    impl_->status.available = true;
    impl_->frame_available.store(true, std::memory_order_release);
    impl_->status.reason = "Ready; awaiting complete temporal inputs and Reflex frame markers";
    return true;
#else
    return false;
#endif
}

bool DlssProvider::begin_frame(DlssFrameToken& token) {
    token = {};
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (!impl_->frame_available.load(std::memory_order_acquire)) return false;
    // Reserve before asking Streamline to rotate its mutable token ring. A
    // rejected attempt retains its prospective ticket and never reaches here.
    const auto reservation = impl_->token_reservations.reserve();
    if (!reservation) {
        if (impl_->token_reservations.exhausted()) {
            impl_->token_index_exhausted.store(true, std::memory_order_relaxed);
            impl_->threaded_result.store(static_cast<int32_t>(sl::Result::eErrorInvalidState), std::memory_order_relaxed);
            impl_->frame_available.store(false, std::memory_order_release);
        }
        return false;
    }
    sl::FrameToken* native = nullptr;
    const uint32_t index = static_cast<uint32_t>(reservation->logical_frame);
    auto result = impl_->get_frame_token(native, &index);
    if (result == sl::Result::eOk && native) result = impl_->reflex_sleep(*native);
    if (result == sl::Result::eOk && !native) result = sl::Result::eErrorInvalidState;
    if (result != sl::Result::eOk) {
        impl_->threaded_result.store(static_cast<int32_t>(result), std::memory_order_relaxed);
        // A failed acquisition cannot prove whether SDK rotation occurred.
        // Stop new acquisitions until the provider has drained and shut down.
        impl_->token_reservations.disable();
        impl_->frame_available.store(false, std::memory_order_release);
        return false;
    }
    token = {native, reservation->logical_frame, index, reservation};
    return true;
#else
    return false;
#endif
}

bool DlssProvider::mark([[maybe_unused]] const DlssFrameToken& token, [[maybe_unused]] DlssLatencyMarker marker) const {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (!token.native_token || !token.reservation || !impl_->pcl_marker) return false;
    if (static_cast<uint32_t>(*static_cast<sl::FrameToken*>(token.native_token)) != token.stream_frame) {
        impl_->threaded_result.store(static_cast<int32_t>(sl::Result::eErrorInvalidState), std::memory_order_relaxed);
        return false;
    }
    sl::PCLMarker native_marker;
    switch (marker) {
    case DlssLatencyMarker::SimulationStart: native_marker = sl::PCLMarker::eSimulationStart; break;
    case DlssLatencyMarker::SimulationEnd: native_marker = sl::PCLMarker::eSimulationEnd; break;
    case DlssLatencyMarker::RenderSubmitStart: native_marker = sl::PCLMarker::eRenderSubmitStart; break;
    case DlssLatencyMarker::RenderSubmitEnd: native_marker = sl::PCLMarker::eRenderSubmitEnd; break;
    case DlssLatencyMarker::PresentStart: native_marker = sl::PCLMarker::ePresentStart; break;
    case DlssLatencyMarker::PresentEnd: native_marker = sl::PCLMarker::ePresentEnd; break;
    case DlssLatencyMarker::TriggerFlash: native_marker = sl::PCLMarker::eTriggerFlash; break;
    case DlssLatencyMarker::LatencyPing: native_marker = sl::PCLMarker::ePCLatencyPing; break;
    default: return false;
    }
    const auto result = impl_->pcl_marker(native_marker, *static_cast<sl::FrameToken*>(token.native_token));
    if (result != sl::Result::eOk)
        impl_->threaded_result.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    return result == sl::Result::eOk;
#else
    return false;
#endif
}

uint32_t DlssProvider::latency_ping_message() const noexcept { return impl_->ping_message; }

bool DlssProvider::prepare_present([[maybe_unused]] const DlssPresentInputs& inputs) {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (!impl_->status.available) return false;
    if (const auto invalid = validate_dlss_present_inputs(inputs, impl_->minimum_dimension,
            impl_->status.maximum_generated_frames); !invalid.empty()) {
        suspend(&inputs.token);
        impl_->status.reason = invalid;
        return false;
    }
    const auto thread_result = static_cast<sl::Result>(
        impl_->threaded_result.exchange(static_cast<int32_t>(sl::Result::eOk)));
    if (!impl_->accept(thread_result, "Reflex frame/marker")) { suspend(&inputs.token); return false; }
    const auto& frame = *static_cast<sl::FrameToken*>(inputs.token.native_token);
    if (static_cast<uint32_t>(frame) != inputs.token.stream_frame) {
        suspend(&inputs.token);
        impl_->status.reason = "Streamline frame token expired or does not match the submitted frame";
        return false;
    }
    impl_->last_token = inputs.token;
    const auto constants = make_constants(inputs.camera);
    if (!impl_->accept(impl_->set_constants(constants, frame, impl_->viewport), "slSetConstants")) {
        suspend(&inputs.token); return false;
    }
    auto depth = make_resource(inputs.depth);
    auto motion = make_resource(inputs.motion);
    auto hudless = make_resource(inputs.hudless_color);
    auto ui = make_resource(inputs.ui_color_alpha);
    auto distortion = make_resource(inputs.bidirectional_distortion);
    const sl::Extent depth_extent{0, 0, inputs.depth.width, inputs.depth.height};
    const sl::Extent color_extent{0, 0, inputs.hudless_color.width, inputs.hudless_color.height};
    const sl::Extent distortion_extent{0, 0, inputs.bidirectional_distortion.width,
        inputs.bidirectional_distortion.height};
    const std::array tags{
        sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::eValidUntilPresent, &depth_extent},
        sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &depth_extent},
        sl::ResourceTag{&hudless, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &color_extent},
        sl::ResourceTag{inputs.backbuffer_has_ui ? &ui : nullptr,
            sl::kBufferTypeUIColorAndAlpha, sl::eValidUntilPresent, &color_extent},
        sl::ResourceTag{inputs.distortion_active ? &distortion : nullptr,
            sl::kBufferTypeBidirectionalDistortionField, sl::eValidUntilPresent, &distortion_extent}};
    if (!impl_->accept(impl_->set_tags(frame, impl_->viewport, tags.data(),
            static_cast<uint32_t>(tags.size()), nullptr), "slSetTagForFrame")) {
        suspend(&inputs.token); return false;
    }
    auto& options = impl_->options;
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = inputs.frames_to_generate;
    options.numBackBuffers = inputs.backbuffer_count;
    options.colorWidth = inputs.hudless_color.width;
    options.colorHeight = inputs.hudless_color.height;
    options.mvecDepthWidth = inputs.depth.width;
    options.mvecDepthHeight = inputs.depth.height;
    options.colorBufferFormat = static_cast<uint32_t>(inputs.backbuffer_format);
    options.mvecBufferFormat = static_cast<uint32_t>(inputs.motion.format);
    options.depthBufferFormat = static_cast<uint32_t>(inputs.depth.format);
    options.hudLessBufferFormat = static_cast<uint32_t>(inputs.hudless_color.format);
    options.uiBufferFormat = inputs.backbuffer_has_ui ? static_cast<uint32_t>(inputs.ui_color_alpha.format) : 0;
    options.flags = sl::DLSSGFlags{};
    if (inputs.depth.width != inputs.hudless_color.width || inputs.depth.height != inputs.hudless_color.height) {
        options.flags |= sl::DLSSGFlags::eDynamicResolutionEnabled;
        options.dynamicResWidth = inputs.depth.width;
        options.dynamicResHeight = inputs.depth.height;
    }
    if (!impl_->accept(impl_->set_options(impl_->viewport, options), "slDLSSGSetOptions")) {
        suspend(&inputs.token); return false;
    }
    impl_->status.enabled = true;
    impl_->status.reason = "DLSS requested; presentation statistics are reported separately from rendered FPS";
    return true;
#else
    return false;
#endif
}

void DlssProvider::finish_present([[maybe_unused]] const DlssFrameToken& token) {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (impl_->last_token.reservation == token.reservation) impl_->last_token = {};
#endif
}

bool DlssProvider::collect_state() {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (!impl_->get_state) return false;
    // Failed acquisition can leave no packet to consume the deferred result.
    // Ordinary fallback presentation still publishes that failure promptly.
    const auto frame_result = static_cast<sl::Result>(
        impl_->threaded_result.exchange(static_cast<int32_t>(sl::Result::eOk)));
    const char* frame_operation = impl_->token_index_exhausted.exchange(false) ?
        "Streamline frame identity exhausted; restart required" : "Reflex frame/marker";
    if (!impl_->accept(frame_result, frame_operation)) {
        suspend();
        return false;
    }
    const auto api_error = asynchronous_api_error.exchange(VK_SUCCESS, std::memory_order_relaxed);
    if (api_error != VK_SUCCESS) {
        suspend();
        impl_->status.available = false;
        impl_->frame_available.store(false, std::memory_order_relaxed);
        impl_->status.reason = "Asynchronous Streamline Vulkan presentation failed: " + std::to_string(api_error);
        return false;
    }
    sl::DLSSGState state;
    if (!impl_->accept(impl_->get_state(impl_->viewport, state, nullptr), "slDLSSGGetState")) {
        suspend(); return false;
    }
    impl_->status.native_status = static_cast<uint32_t>(state.status);
    impl_->status.maximum_generated_frames = state.numFramesToGenerateMax;
    impl_->status.last_sdk_presented_frames = state.numFramesActuallyPresented;
    impl_->status.total_sdk_presented_frames += state.numFramesActuallyPresented;
    impl_->status.estimated_vram_bytes = state.estimatedVRAMUsageInBytes;
    impl_->minimum_dimension = state.minWidthOrHeight;
    // Per-frame constants, resolution and Reflex state do not exist during
    // the initial OFF capability probe. Enforce them once generation is on.
    if (impl_->status.enabled && state.status != sl::DLSSGStatus::eOk) {
        suspend();
        impl_->status.available = false;
        impl_->frame_available.store(false, std::memory_order_relaxed);
        impl_->status.reason = "DLSS-G reports an unavailable integration/status bitmask: " +
            std::to_string(impl_->status.native_status);
        return false;
    }
    // SDK counts combine real and generated presentations. Asynchronous state
    // queries can include several base frames, so they do not establish which
    // frames were interpolated or actually scanned out by the display.
    return true;
#else
    return false;
#endif
}

bool DlssProvider::suspend([[maybe_unused]] const DlssFrameToken* token) {
    impl_->status.enabled = false;
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (!impl_->status.initialized || !impl_->set_options) return true;
    impl_->options.mode = sl::DLSSGMode::eOff;
    bool success = impl_->accept(impl_->set_options(impl_->viewport, impl_->options), "Disable DLSS-G");
    const auto* ticket = token && token->native_token ? token : &impl_->last_token;
    if (ticket->native_token) {
        const std::array tags{
            sl::ResourceTag{nullptr, sl::kBufferTypeDepth, sl::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeUIColorAndAlpha, sl::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeBidirectionalDistortionField, sl::eValidUntilPresent}};
        success &= impl_->accept(impl_->set_tags(*static_cast<sl::FrameToken*>(ticket->native_token),
            impl_->viewport, tags.data(), static_cast<uint32_t>(tags.size()), nullptr), "Clear DLSS resource tags");
    }
    impl_->last_token = {};
    return success;
#else
    return true;
#endif
}

DlssProviderStatus DlssProvider::status() const { return impl_->status; }

void DlssProvider::shutdown_after_drain() {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE
    if (impl_->status.initialized) {
        suspend();
        impl_->accept(impl_->shutdown(), "slShutdown");
    }
    impl_->status.initialized = false;
    impl_->status.available = false;
    impl_->frame_available.store(false, std::memory_order_relaxed);
    impl_->status.enabled = false;
    impl_->ping_message = 0;
#endif
}

} // namespace heaven
