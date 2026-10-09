/*
 * HeavenBench — Scene-colour capture (previous-frame history)
 *
 * Screen-space global illumination (SsgiFeature) composites inline in the
 * main scene, so it cannot read the colour buffer it is part of. The standard
 * technique for an inline screen-space lighting pass is to gather from the
 * PREVIOUS frame's lit result. This subsystem provides exactly that: a stable
 * main-view colour target, a separate previous-frame history texture, and
 * fullscreen blits to the swap chain / history target.
 *
 * Each captured frame:
 *   1. history_texture() returns the colour copied from the clean main view
 *      LAST frame. SSGI binds it as its gather source in
 *      update() — outside any render pass (Vulkan forbids descriptor changes
 *      mid-pass).
 *   2. current_rt() is the stable render target the backend points the main
 *      view at THIS frame, so the clean lit scene lands in a sampleable RGBA16F
 *      texture instead of the swap chain.
 *   3. after the main view renders, render_blit() copies that texture to the
 *      swap chain (heaven_blit material), render_history_copy() copies the same
 *      clean texture into the history target, and advance() marks that copy
 *      valid for the next frame.
 *
 * It is core infrastructure, only engaged when a feature requests scene colour
 * (SSGI). When no such feature is enabled the backend renders the main view
 * straight to the swap chain as before, so the --no-ssgi path is unchanged
 * (Shipping_Look parity, no offscreen + blit overhead). It deliberately does
 * NOT participate when the heat-haze feature owns the scene-capture seam this
 * frame (only one thing can redirect the main view); SSGI then reuses its prior
 * history for that frame.
 */
#ifndef HEAVENBENCH_SRC_RHI_SCENE_COLOR_CAPTURE_H_
#define HEAVENBENCH_SRC_RHI_SCENE_COLOR_CAPTURE_H_

#include "rhi/backend_core.h"
#include "engine/camera.h"

namespace filament {
class View;
class Scene;
class Renderer;
class RenderTarget;
class Texture;
class Material;
class MaterialInstance;
class VertexBuffer;
class IndexBuffer;
}

#include <utils/Entity.h>
#include <optional>

namespace heaven {

class SceneColorCapture {
public:
    explicit SceneColorCapture(const BackendCore& core) : core_(core) {}
    ~SceneColorCapture();

    // Lazily build the blit pipeline (once) and (re)create the colour/depth
    // targets for the given full-resolution viewport. No-op when
    // the size is unchanged. Returns false if the blit material is unavailable
    // (the backend then skips the scene-colour path and renders direct).
    bool ensure(int w, int h);

    // True once the blit pipeline + targets exist.
    [[nodiscard]] bool valid() const { return blit_mi_ != nullptr && rt_[0] != nullptr; }

    // The previous frame's lit colour, for a feature to gather from. Null until
    // at least one frame has been captured (so the first frame falls back).
    [[nodiscard]] filament::Texture* history_texture() const;
    [[nodiscard]] std::optional<glm::mat4> history_view_proj() const {
        return history_view_proj_;
    }

    // The render target the main view should render into this frame.
    [[nodiscard]] filament::RenderTarget* current_rt() const { return rt_[0]; }

    // Point the blit quad at the full viewport. ensure() has already bound its
    // colour source, including frames on which another capture path is used.
    void aim(const Camera& camera, int viewport_w, int viewport_h);

    // Blit the current colour texture to the swap chain (call after the main
    // view render, inside begin/endFrame).
    void render_blit();

    // Copy the current clean colour texture into the history target. The source
    // remains current_rt(), so SSGI's own composite never feeds back.
    void render_history_copy();

    // Final output target for the blit composite. nullptr (default) = swap
    // chain; under HDR display output the backend points this at the linear
    // Rec.2020 fp16 HDR scene target so the encode pass PQ-encodes it.
    void set_output_target(filament::RenderTarget* rt);

    // Mark the latest history copy valid for the next frame.
    void advance(const Camera& camera, int viewport_w, int viewport_h);

private:
    void build_blit_pipeline();
    void destroy_targets();

    BackendCore core_;

    // Colour targets: [0] is the stable main-view capture target, [1] is the
    // previous-frame history copy. depth_tex_ is attached so the main view can
    // render normally; the history blit has depth testing disabled.
    filament::Texture*      color_tex_[2] = { nullptr, nullptr };
    filament::Texture*      depth_tex_    = nullptr;
    filament::RenderTarget* rt_[2]        = { nullptr, nullptr };
    int                     w_     = 0;
    int                     h_     = 0;
    std::optional<glm::mat4> history_view_proj_;

    // Blit pipeline (heaven_blit): a dedicated composite view/scene with one
    // fullscreen quad that samples the current colour texture.
    filament::Material*         blit_material_ = nullptr;
    filament::MaterialInstance* blit_mi_       = nullptr;
    filament::VertexBuffer*     blit_vb_       = nullptr;
    filament::IndexBuffer*      blit_ib_       = nullptr;
    utils::Entity               blit_entity_;
    filament::Scene*            composite_scene_ = nullptr;
    filament::View*             composite_view_  = nullptr;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_SCENE_COLOR_CAPTURE_H_
