/*
 * HeavenBench — Raymarched volumetric clouds feature
 *
 * Screen-space depth-buffer raymarched 3D cloud layer. Same proven
 * post-process pattern as god rays and volumetric fog: a world-space
 * fullscreen quad parked just inside the far plane in front of the
 * camera each frame, sampling the shared half-resolution depth pre-pass
 * to know where solid geometry is, and raymarching a horizontal cloud
 * slab in world space, compositing the result as a transparent blend
 * over the scene.
 *
 * This feature supersedes the dormant CloudFeature (the legacy
 * cloud-quad layer, which stays off by default so the two never
 * composite). It is a depth pre-pass participant: its fullscreen quad
 * is pulled out of the scene while the depth view renders, then re-added
 * before the main view draws (the god-rays / volfog pattern).
 *
 * Motion is advanced solely from FrameContext::time so it is
 * frame-rate-independent. Disabled by default
 * (config.features.volumetric_clouds), so a flagless run is
 * Shipping_Look-equivalent.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_VOLUMETRIC_CLOUD_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_VOLUMETRIC_CLOUD_FEATURE_H_

#include "rhi/render_feature.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>

#include <utils/Entity.h>

namespace heaven {

class VolumetricCloudFeature final : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "volumetric-clouds"; }

    void init(RenderContext& ctx, const AppConfig& config) override;
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void release_shared_targets(RenderContext& ctx) override;
    void destroy(RenderContext& ctx);

    [[nodiscard]] bool needs_depth_prepass() const override { return mi_ != nullptr; }
    void on_depth_prepass_begin(RenderContext& ctx) override;
    void on_depth_prepass_end(RenderContext& ctx) override;

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

private:
    filament::Material*         material_ = nullptr;
    filament::MaterialInstance* mi_       = nullptr;
    filament::VertexBuffer*     vb_       = nullptr;
    filament::IndexBuffer*      ib_       = nullptr;
    utils::Entity               entity_;
    bool                        in_scene_ = false;

    // Raymarch step budget, resolved from the cloud-quality knob at init.
    int                         num_steps_ = 1;

    // Last depth texture we bound, so we only rebind on resize (the
    // backend rebuilds the depth texture when the viewport changes).
    filament::Texture*          bound_depth_ = nullptr;
    uint32_t                    frame_index_ = 0;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_VOLUMETRIC_CLOUD_FEATURE_H_
