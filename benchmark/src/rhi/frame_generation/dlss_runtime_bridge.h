#ifndef HEAVEN_DLSS_RUNTIME_BRIDGE_H
#define HEAVEN_DLSS_RUNTIME_BRIDGE_H

#include "dlss_provider.h"

#include <glm/glm.hpp>

#include <expected>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace filament::backend { struct VulkanCommandContext; }

namespace heaven {

class Camera;
class DlssPlatform;
class FrameGenerationOutput;

// A copy travels with the driver's native callback. Never read a newer
// application-frame token or camera while processing an older submission.
struct DlssRuntimeFrame {
    DlssFrameToken token;
    glm::mat4 projection{1.0f};
    glm::mat4 current_view_projection{1.0f};
    glm::mat4 previous_view_projection{1.0f};
    float near_plane = 0.0f;
    // Use the maximum finite float for an infinite-far rendering projection;
    // Filament's finite culling distance is a different camera property.
    float far_plane = 0.0f;
    bool reset = true;
    bool temporal_coverage_complete = false;
    bool has_ui = false;
};

// Adapts the application-owned SDR temporal targets to the optional SDK. It
// does not submit commands, run interpolation itself, or invent a swapchain.
class DlssRuntimeBridge final {
public:
    DlssRuntimeBridge(DlssPlatform& platform,
            std::shared_ptr<FrameGenerationOutput> output);
    ~DlssRuntimeBridge();
    DlssRuntimeBridge(const DlssRuntimeBridge&) = delete;
    DlssRuntimeBridge& operator=(const DlssRuntimeBridge&) = delete;

    // Application thread, before sampling input. Sleep precedes SimulationStart.
    // The same returned ticket must be copied into DlssRuntimeFrame and all
    // subsequent markers. A missing ticket leaves this iteration unprofiled.
    DlssFrameToken begin_frame(uint64_t application_frame);
    bool mark(const DlssFrameToken& token, DlssLatencyMarker marker) const;

    // Serialized driver thread, after all four persistent textures have been
    // rendered, outside a render pass, before endFrame/present. Image order is
    // HUDless color, extracted depth, extracted motion, premultiplied UI.
    std::expected<void, std::string> prepare_native(
            const filament::backend::VulkanCommandContext& context,
            const Camera& camera, const DlssRuntimeFrame& frame);

    // Serialized driver thread. Before resizing/reloading/destroying output:
    // enqueue suspension, drain backend/GPU/SDK presentation, then mutate or
    // release targets. A shared pointer retains the object, not retired images
    // if another owner calls ensure() prematurely. Destroy bridge after drain.
    void suspend_generation();

private:
    DlssPlatform& platform_;
    std::shared_ptr<FrameGenerationOutput> output_;
    std::optional<uint64_t> previous_application_frame_;
    std::chrono::steady_clock::time_point previous_native_callback_;
    uint64_t swapchain_epoch_ = 0;
};

} // namespace heaven

#endif // HEAVEN_DLSS_RUNTIME_BRIDGE_H
