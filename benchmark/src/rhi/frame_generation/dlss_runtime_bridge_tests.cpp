// GPU-independent camera/layout contracts. No opaque test handle is submitted
// to Vulkan or Streamline; the SDK's UV reconstruction supplies the oracle.
#include "dlss_runtime_inputs.h"
#include "dlss_status.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <cstdio>
#include <exception>
#include <limits>
#include <numbers>
#include <type_traits>

namespace heaven {
namespace {

template<class T> T fake_handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) {
        if (!value) return nullptr;
        static std::array<std::max_align_t, 8> identities{};
        return reinterpret_cast<T>(&identities.at(value));
    }
    else return static_cast<T>(value);
}

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "[dlss_runtime_contract] %s\n", message);
    return condition;
}

bool close(float left, float right, float tolerance = 2e-4f) {
    return std::isfinite(left) && std::isfinite(right) && std::abs(left - right) <= tolerance;
}

// Deliberately ordinary row-vector multiplication, independent of GLM's
// column-vector math used by the implementation.
std::array<float, 4> row_multiply(const std::array<float, 4>& vector,
        const std::array<float, 16>& matrix) {
    std::array<float, 4> result{};
    for (size_t column = 0; column != 4; ++column)
        for (size_t row = 0; row != 4; ++row)
            result[column] += vector[row] * matrix[row * 4 + column];
    return result;
}

glm::vec3 screen_from_gl(const glm::vec4& clip) {
    return {0.5f + 0.5f * clip.x / clip.w,
            0.5f - 0.5f * clip.y / clip.w,
            0.5f - 0.5f * clip.z / clip.w};
}

bool run_dlss_status_tests() {
    FrameGenerationState state;
    state.request(FrameGenerationMode::Dlss);
    state.set_capability({FrameGenerationMode::Dlss, true, "Native inputs ready"});
    state.record_real_frame(true);
    state.record_real_frame(true);
    auto snapshot = state.status();
    const auto epoch = snapshot.history_epoch;
    DlssProviderStatus provider;
    provider.initialized = true;
    provider.available = true;
    provider.enabled = true;
    provider.total_sdk_presented_frames = 12;
    apply_dlss_provider_status(state, snapshot, provider);
    bool success = check(snapshot.effective == FrameGenerationMode::Dlss &&
        snapshot.history_ready && snapshot.history_epoch == epoch &&
        snapshot.sdk_presented_frames == 12 &&
        !snapshot.counters.generated_frames_available &&
        !snapshot.counters.presentation_submissions_available,
        "healthy provider changed activation/history or invented exact split counts");

    provider.available = provider.enabled = false;
    provider.reason = "Asynchronous native presentation failed";
    apply_dlss_provider_status(state, snapshot, provider);
    success &= check(snapshot.effective == FrameGenerationMode::Off &&
        !snapshot.capability.available && !snapshot.history_ready &&
        snapshot.reason == provider.reason && snapshot.history_epoch == epoch + 1 &&
        snapshot.counters.real_frames == 2 && snapshot.sdk_presented_frames == 12 &&
        state.status().effective == FrameGenerationMode::Off,
        "asynchronous provider failure left active state or usable history");
    apply_dlss_provider_status(state, snapshot, provider);
    success &= check(snapshot.history_epoch == epoch + 1,
        "repeated provider failure repeatedly invalidated history");
    provider.available = true;
    apply_dlss_provider_status(state, snapshot, provider);
    success &= check(snapshot.effective == FrameGenerationMode::Off,
        "provider recovery bypassed native temporal-input validation");

    state.set_capability({FrameGenerationMode::Dlss, false, "Transparent coverage incomplete"});
    snapshot = state.status();
    provider.available = false;
    provider.reason = "Provider suspended";
    apply_dlss_provider_status(state, snapshot, provider);
    success &= check(snapshot.reason == "Transparent coverage incomplete",
        "provider status overwrote the stricter application coverage reason");
    provider.initialized = false;
    apply_dlss_provider_status(state, snapshot, provider);
    success &= check(!snapshot.sdk_presented_frames, "uninitialized SDK retained a measured total");
    provider.initialized = true;
    provider.total_sdk_presented_frames = 0;
    apply_dlss_provider_status(state, snapshot, provider);
    success &= check(snapshot.sdk_presented_frames == 0,
        "measured SDK zero was confused with unavailable coverage");

    state.request(FrameGenerationMode::Fsr3);
    state.set_capability({FrameGenerationMode::Fsr3, true, "FSR ready"});
    snapshot = state.status();
    apply_dlss_provider_status(state, snapshot, provider);
    success &= check(snapshot.effective == FrameGenerationMode::Fsr3 &&
        snapshot.reason == "FSR ready" && !snapshot.sdk_presented_frames,
        "DLSS failure changed the selected FSR provider");
    state.request(FrameGenerationMode::Off);
    snapshot = state.status();
    apply_dlss_provider_status(state, snapshot, provider);
    return check(snapshot.effective == FrameGenerationMode::Off && snapshot.reason == "Disabled",
        "DLSS failure changed an explicit Off request") && success;
}

bool run_dlss_runtime_bridge_tests() {
    constexpr float near_plane = 0.5f;
    constexpr float aspect = 16.0f / 9.0f;
    const float fov = glm::radians(67.0f);
    auto projection = glm::perspective(fov, aspect, near_plane, 2000.0f);
    // Filament preserves infinite-far rendering while culling at finite range.
    projection[2][2] = -1.0f;
    projection[3][2] = -2.0f * near_plane;
    const glm::vec3 position{7.0f, -4.0f, 3.0f};
    const auto view = glm::lookAt(position, glm::vec3{0.0f, 2.0f, 1.0f}, glm::vec3{0,0,1});
    const auto previous_view = glm::lookAt(position + glm::vec3{.3f,-.2f,.1f},
        glm::vec3{.1f,2.2f,1.2f}, glm::vec3{0,0,1});
    const auto current_vp = projection * view;
    const auto previous_vp = projection * previous_view;
    const auto constants = dlss_runtime_detail::camera_constants(glm::inverse(view), projection,
        current_vp, previous_vp, near_plane, std::numeric_limits<float>::max(), fov, aspect, false);
    bool success = run_dlss_status_tests();
    success &= check(constants.has_value(), "actual infinite-far camera rejected");
    if (!constants) return false;

    const std::array world_points{glm::vec4{0,2,1,1}, glm::vec4{-2,1,2,1},
        glm::vec4{1,4,.1f,1}, glm::vec4{-3,5,4,1}};
    for (const auto& world : world_points) {
        const auto current_screen = screen_from_gl(current_vp * world);
        const auto previous_screen = screen_from_gl(previous_vp * world);
        // Pinned Streamline shaders/mvec.hlsl reconstructs (2u-1,1-2v,depth,1).
        const std::array current_clip{2.0f * current_screen.x - 1.0f,
            1.0f - 2.0f * current_screen.y, current_screen.z, 1.0f};
        const auto previous_clip = row_multiply(current_clip, constants->clip_to_previous_clip);
        const glm::vec3 predicted_previous{
            0.5f + 0.5f * previous_clip[0] / previous_clip[3],
            0.5f - 0.5f * previous_clip[1] / previous_clip[3],
            previous_clip[2] / previous_clip[3]};
        success &= check(close(predicted_previous.x, previous_screen.x) &&
            close(predicted_previous.y, previous_screen.y) &&
            close(predicted_previous.z, previous_screen.z),
            "noncentral reprojection disagrees with independently projected previous UV/depth");
        const auto restored = row_multiply(previous_clip, constants->previous_clip_to_clip);
        for (size_t i = 0; i != 3; ++i)
            success &= check(close(restored[i] / restored[3], current_clip[i]),
                "previous-to-current clip inverse did not roundtrip");
    }

    for (const float distance : {near_plane, 2.0f, 100.0f}) {
        const std::array view_point{0.3f, -0.2f, -distance, 1.0f};
        const auto clip = row_multiply(view_point, constants->view_to_clip);
        success &= check(close(clip[2] / clip[3], near_plane / distance),
            "infinite reversed projection does not match actual tagged depth");
        const auto roundtrip = row_multiply(clip, constants->clip_to_view);
        for (size_t i = 0; i != 4; ++i)
            success &= check(close(roundtrip[i], view_point[i], 2e-3f),
                "view-to-clip inverse did not roundtrip");
    }
    success &= check(close(constants->position[0], position.x) &&
        close(constants->position[1], position.y) && close(constants->position[2], position.z),
        "camera position lost world transform");
    const auto reset = dlss_runtime_detail::camera_constants(glm::inverse(view), projection,
        current_vp, previous_vp, near_plane, std::numeric_limits<float>::max(), fov, aspect, true);
    constexpr std::array<float,16> identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    success &= check(reset && reset->reset && reset->clip_to_previous_clip == identity &&
        reset->previous_clip_to_clip == identity, "camera cut retained a previous transform");
    success &= check(constants->depth_inverted && !constants->motion_jittered &&
        constants->jitter_pixels == std::array<float,2>{0,0},
        "unjittered reversed-depth flags do not match extracted inputs");
    success &= check(!dlss_runtime_detail::camera_constants(glm::mat4(0), projection,
        current_vp, previous_vp, near_plane, 2000, fov, aspect, false),
        "singular camera accepted");
    auto invalid = projection;
    invalid[2][1] = std::numeric_limits<float>::quiet_NaN();
    success &= check(!dlss_runtime_detail::camera_constants(glm::inverse(view), invalid,
        current_vp, previous_vp, near_plane, 2000, fov, aspect, false),
        "non-finite camera accepted");
    // Retain well-conditioned matrices to isolate the scalar FOV contract.
    const auto with_fov = [&](float value) {
        return dlss_runtime_detail::camera_constants(glm::inverse(view), projection,
            current_vp, previous_vp, near_plane, std::numeric_limits<float>::max(),
            value, aspect, false);
    };
    success &= check(!with_fov(std::numbers::pi_v<float>), "pi-radian field of view accepted");
    success &= check(!with_fov(std::nextafter(std::numbers::pi_v<float>,
        std::numeric_limits<float>::infinity())), "field of view above pi accepted");
    success &= check(with_fov(std::nextafter(std::numbers::pi_v<float>, 0.0f)).has_value(),
        "field of view immediately below pi rejected");

    filament::backend::VulkanCommandImage native;
    native.image = fake_handle<VkImage>(1);
    native.view = fake_handle<VkImageView>(2);
    native.memory = fake_handle<VkDeviceMemory>(3);
    native.format = VK_FORMAT_R16G16_SFLOAT;
    native.extent = {1920, 1080};
    native.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    native.layout = VK_IMAGE_LAYOUT_GENERAL;
    native.restoredLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const auto tagged = dlss_runtime_detail::present_image(native);
    success &= check(tagged.layout == native.restoredLayout && tagged.layout != native.layout,
        "tag used transient callback layout instead of the later presentation state");
    success &= check(tagged.image == native.image && tagged.view == native.view &&
        tagged.memory == native.memory && tagged.format == native.format &&
        tagged.usage == native.usage && tagged.width == 1920 && tagged.height == 1080,
        "native tag fabricated or lost image/allocation metadata");
    success &= check(dlss_runtime_detail::sdr_backbuffer_format(VK_FORMAT_B8G8R8A8_UNORM) &&
        dlss_runtime_detail::sdr_backbuffer_format(VK_FORMAT_R8G8B8A8_UNORM) &&
        !dlss_runtime_detail::sdr_backbuffer_format(VK_FORMAT_B8G8R8A8_SRGB) &&
        !dlss_runtime_detail::sdr_backbuffer_format(VK_FORMAT_R16G16B16A16_SFLOAT),
        "unqualified encoded SDR/backbuffer domain accepted");
    return success;
}

} // namespace
} // namespace heaven

#ifdef HEAVEN_DLSS_RUNTIME_INPUT_TEST_MAIN
int main() {
    try {
        const bool passed = heaven::run_dlss_runtime_bridge_tests();
        std::printf("DLSS runtime camera/layout contracts: %s\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DLSS runtime camera/layout contracts: exception: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DLSS runtime camera/layout contracts: unknown exception\n");
        return 1;
    }
}
#endif
