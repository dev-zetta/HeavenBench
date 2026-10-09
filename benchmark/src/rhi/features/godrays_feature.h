/*
 * HeavenBench — Volumetric light shafts (god rays) feature
 *
 * Screen-space radial blur from the sun's projected position, gated on
 * the sun being on-screen and bright enough. Samples the shared
 * half-resolution depth pre-pass for occlusion, so it is a depth
 * pre-pass participant: its fullscreen quad must be pulled out of the
 * scene while the depth view renders, then re-added before the main
 * view draws.
 *
 * Migrated verbatim out of FilamentBackend::init_godrays /
 * update_godrays.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_GODRAYS_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_GODRAYS_FEATURE_H_

#include "rhi/render_feature.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>

#include <utils/Entity.h>

namespace heaven {

class GodraysFeature final : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "god_rays"; }

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

    // Last depth texture we bound, so we only rebind on resize (the
    // backend rebuilds the depth texture when the viewport changes).
    filament::Texture*          bound_depth_ = nullptr;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_GODRAYS_FEATURE_H_
