#ifndef HEAVEN_FRAME_GENERATION_RUNTIME_H
#define HEAVEN_FRAME_GENERATION_RUNTIME_H

#include "engine/frame_generation.h"
#include "rhi/backend_core.h"
#include "rhi/rhi.h"
#include <memory>
#include <mutex>
#include <chrono>
#include <vector>
#include "rhi/frame_generation/temporal_grass.h"
#include "rhi/frame_generation/dlss_provider.h"

namespace filament { class Material; class RenderTarget; class View; }
namespace heaven {
class FrameGenerationPlatform;
class FrameGenerationOutput;
class DlssPlatform;
class DlssRuntimeBridge;
class TemporalScene;
class TemporalGrass;
class MeshStore;
class TextureStore;

// Owns optional temporal resources, created only after an explicit request.
// An ordinary Off startup creates no Runtime. After switching back to Off,
// keep real-only presentation bookkeeping without temporal draws or SDK work.
// Drain the engine before destroying it.
class FrameGenerationRuntime {
public:
    FrameGenerationRuntime(const BackendCore& core, FrameGenerationPlatform* platform,
                           DlssPlatform* dlss_platform = nullptr);
    ~FrameGenerationRuntime();
    void request(FrameGenerationMode mode);
    FrameGenerationStatus status();
    bool prepare(MeshStore& meshes, TextureStore& textures,
        std::span<const Instance> instances, std::span<const GpuMesh> gpu_meshes,
        const MaterialResolver& resolver, const AppConfig& config,
        const Camera& camera, int width, int height, bool has_ui);
    void bind_ui(filament::View& view);
    void unbind_ui();
    void prepare_grass(std::span<const TemporalGrassPatch> patches,
        std::span<const Instance> instances, const Camera& camera, float time);
    filament::RenderTarget* scene_target() const;
    void render_temporal();
    void render_composite();
    void submit(bool accepted, const Camera& camera);
    void set_bones(int mesh_index, std::span<const glm::mat4> matrices);
    void set_positions(uint64_t handle, std::span<const Vertex> vertices);
    void invalidate_history();
    void begin_application_frame(uint64_t application_frame);
    void end_simulation();
    void native_window_message(uint32_t message);
    void drain();
    TemporalScene* temporal_scene() const { return temporal_.get(); }
private:
    struct Native;
    bool unavailable(std::string reason);
    BackendCore core_;
    [[maybe_unused]] FrameGenerationPlatform* platform_ = nullptr;
    [[maybe_unused]] DlssPlatform* dlss_platform_ = nullptr;
    std::unique_ptr<TemporalScene> temporal_;
    std::unique_ptr<TemporalGrass> grass_;
    std::shared_ptr<FrameGenerationOutput> output_;
    std::unique_ptr<DlssRuntimeBridge> dlss_;
    DlssFrameToken dlss_token_;
    std::unique_ptr<Native> native_;
    filament::Material* motion_ = nullptr;
    filament::Material* masked_ = nullptr;
    filament::Material* grass_material_ = nullptr;
    mutable std::mutex mutex_;
    FrameGenerationState state_;
    bool active_ = false;
    bool reset_ = true;
    [[maybe_unused]] int width_ = 0, height_ = 0;
    std::chrono::steady_clock::time_point previous_source_;
    std::optional<Camera> previous_camera_;
    glm::mat4 projection_{1.0f};
    glm::mat4 current_clip_{1.0f};
    glm::mat4 previous_clip_{1.0f};
    bool temporal_coverage_complete_ = false;
    [[maybe_unused]] bool has_ui_ = false;
    bool native_tracking_complete_ = true;
};
}

#endif // HEAVEN_FRAME_GENERATION_RUNTIME_H
