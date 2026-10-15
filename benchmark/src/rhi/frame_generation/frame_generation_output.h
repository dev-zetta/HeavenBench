#ifndef HEAVEN_FRAME_GENERATION_OUTPUT_H
#define HEAVEN_FRAME_GENERATION_OUTPUT_H

#include "rhi/backend_core.h"
#include <glm/mat4x4.hpp>
#include <memory>

namespace filament { class RenderTarget; class Texture; class View; }

namespace heaven {

struct TemporalCameraHistory {
    glm::mat4 current_clip_from_world{1.0f};
    glm::mat4 previous_clip_from_world{1.0f};
    bool valid = false;
};

// SDR display-domain scene and isolated premultiplied UI. The owner detaches
// borrowed targets before ensure() resizes them and before destruction.
class FrameGenerationOutput {
public:
    explicit FrameGenerationOutput(const BackendCore& core);
    ~FrameGenerationOutput();
    FrameGenerationOutput(const FrameGenerationOutput&) = delete;
    FrameGenerationOutput& operator=(const FrameGenerationOutput&) = delete;
    bool ensure(int width, int height);
    bool needs_resize(int width, int height) const noexcept;
    bool valid() const noexcept;
    filament::RenderTarget* scene_rt() const noexcept;
    filament::RenderTarget* ui_rt() const noexcept;
    filament::Texture* color_texture() const noexcept;
    filament::Texture* depth_texture() const noexcept;
    filament::Texture* motion_texture() const noexcept;
    filament::Texture* ui_texture() const noexcept;
    // Optional coherent heat warp. RG maps distorted output UV to the
    // undistorted scene; BA carries the inverse displacement. Both use
    // top-left normalized UV, with undistorted depth/motion and isolated UI.
    // Null disables the warp without allocating any distortion resources.
    bool prepare_distortion(filament::Texture* coarse_field);
    filament::Texture* distortion_texture() const noexcept;

    // Borrow the transparent skybox/target for this draw. The caller restores
    // the view's target, scene skybox and blend mode immediately afterward.
    void bind_ui(filament::View& view);
    void unbind_ui();
    // Packed temporal RG=top-left UV current-to-previous motion, B=inverted
    // Vulkan depth, A=validity. Bind before beginFrame, outside render passes.
    // Background rays carry camera rotation at infinite distance. History is
    // supplied from the last accepted source, never a rejected application attempt.
    void prepare(filament::Texture* packed, int width, int height, bool has_ui,
                 const TemporalCameraHistory& camera_history = {},
                 filament::Texture* effects = nullptr);
    void render_extract();
    void render_composite();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven

#endif // HEAVEN_FRAME_GENERATION_OUTPUT_H
