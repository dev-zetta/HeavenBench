#pragma once

#include "rhi/backend_core.h"
#include <memory>

namespace filament { class RenderTarget; class Texture; class View; }

namespace heaven {

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

    // Borrow the transparent skybox/target for this draw. The caller restores
    // the view's target, scene skybox and blend mode immediately afterward.
    void bind_ui(filament::View& view);
    void unbind_ui();
    // Packed temporal RG=top-left UV current-to-previous motion, B=inverted
    // Vulkan depth, A=validity. Bind before beginFrame, outside render passes.
    void prepare(filament::Texture* packed, int width, int height, bool has_ui);
    void render_extract();
    void render_composite();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven
