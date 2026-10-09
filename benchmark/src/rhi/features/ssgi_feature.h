/*
 * HeavenBench — Screen-space global illumination (SSGI) feature
 *
 * Screen-space, depth-buffer one-bounce indirect lighting. The same proven
 * post-process pattern as god rays and volumetric clouds: a world-space
 * fullscreen quad parked just inside the far plane in front of the camera
 * each frame, sampling the shared half-resolution depth pre-pass
 * (FrameContext::depth_texture) to reconstruct VIEW-SPACE positions/normals,
 * gathering one-bounce indirect light from a copy of the scene colour within a
 * bounded screen-space radius (producing colour bleeding) and
 * compositing the result ADDITIVELY into the scene's indirect lighting
 * (heaven_ssgi.mat uses blending:add).
 *
 * It is a depth pre-pass participant: needs_depth_prepass() returns
 * true and the quad is pulled out of the scene while the depth view renders,
 * then re-added before the main view draws (the god-rays / volfog pattern).
 *
 * Enabled by default (config.features.ssgi, default true) as part of the full
 * advanced-rendering look; --no-ssgi disables it for Shipping_Look parity
 * Registered exactly once in FilamentBackend::init_features (smoke
 * test 9.5 asserts exactly one "ssgi" feature).
 *
 * The SSGI material needs a copy of the rendered scene colour to gather from
 * (colorTexture). The single backend scene-capture seam (begin_scene_capture /
 * render_composite) is owned by the particle/heat-haze feature and only one
 * feature can drive it per frame, so the real scene-colour gather is not wired
 * yet. Until it is, colorTexture is bound to a feature-owned 1×1 BLACK texture
 * (color_stub_): SSGI composites ADDITIVELY, so a black source is the no-data
 * identity (adds nothing) and keeps the enabled-but-unwired state artifact-free.
 * A WHITE stub (the earlier choice) instead injected light at every depth
 * discontinuity — the bright-streak glitch — because the gather weight is
 * non-zero where the depth-reconstructed normal is degenerate.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_SSGI_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_SSGI_FEATURE_H_

#include "rhi/render_feature.h"
#include "render_tuning.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>

#include <utils/Entity.h>

#include <glm/glm.hpp>

namespace filament {
class Scene;
class View;
class RenderTarget;
}

namespace heaven {

class SsgiFeature final : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "ssgi"; }

    void init(RenderContext& ctx, const AppConfig& config) override;
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void release_shared_targets(RenderContext& ctx) override;
    void destroy(RenderContext& ctx);

    [[nodiscard]] bool needs_depth_prepass() const override { return mi_ != nullptr; }
    void on_depth_prepass_begin(RenderContext& ctx) override;
    void on_depth_prepass_end(RenderContext& ctx) override;

    // SSGI gathers from the rendered scene colour; ask the backend to capture
    // it (previous-frame history) while the feature is enabled and built.
    [[nodiscard]] bool wants_scene_color() const override { return mi_ != nullptr; }

    // Draw the SSGI bounce OVER the displayed scene, after the clean main view
    // has been captured + blitted. The SSGI quad lives in a feature-owned view
    // (ssgi_view_) that is NOT part of the captured main view, so its output is
    // never fed back into the history it gathers from — breaking the self-
    // feedback white-out.
    void render_scene_color_composite(RenderContext& ctx,
                                      filament::RenderTarget* target) override;

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

private:
    filament::Material*         material_ = nullptr;
    filament::MaterialInstance* mi_       = nullptr;
    filament::VertexBuffer*     vb_       = nullptr;
    filament::IndexBuffer*      ib_       = nullptr;
    utils::Entity               entity_;

    // Feature-owned scene + view holding the SSGI quad. The quad lives HERE,
    // not in the main scene, so it is never part of the captured history —
    // the backend renders this view as a separate additive composite over the
    // already-blitted clean scene (render_scene_color_composite), which breaks
    // the self-feedback loop that washed the screen white.
    filament::Scene*            ssgi_scene_ = nullptr;
    filament::View*             ssgi_view_  = nullptr;

    // Gather sample/ray count, resolved from the ssgi-quality knob at init.
    int                         num_samples_ = 1;

    // Last depth texture we bound, so we only rebind on resize (the
    // backend rebuilds the depth texture when the viewport changes).
    filament::Texture*          bound_depth_ = nullptr;

    // 1×1 BLACK stand-in for the (not-yet-wired) scene-colour gather source.
    // SSGI composites ADDITIVELY (heaven_ssgi.mat: blending:add), so the
    // no-data identity is BLACK (zero), not white: the gather averages this
    // colour and adds it, and adding black adds nothing. A white stub instead
    // injected full-intensity light wherever the geometric gather weight was
    // non-zero — i.e. at every depth discontinuity (stair edges, foliage
    // silhouettes) where the reconstructed normal is degenerate — producing
    // bright white streaks that swam with the camera. Owned here until the real
    // scene-colour capture is wired through the scene-capture seam.
    filament::Texture*          color_stub_ = nullptr;

    // Last scene-colour texture bound as the gather source. SSGI samples the
    // PREVIOUS frame's clean lit colour (FrameContext::scene_color_texture);
    // rebind on change/resize. Null until the first history frame exists, when
    // we fall back to color_stub_ (black ⇒ zero additive contribution).
    filament::Texture*          bound_color_ = nullptr;

    // At-rest indirect-light scale (the `intensity` uniform). The gather reads
    // lit scene colour rather than raw albedo, so keep the additive term small.
    float base_intensity_ = render_tuning::ssgi.base_intensity;

};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_SSGI_FEATURE_H_
