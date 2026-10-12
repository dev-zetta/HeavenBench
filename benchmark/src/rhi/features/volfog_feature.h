/*
 * HeavenBench — Volumetric fog feature
 *
 * Screen-space per-light ray-marched volumetric scattering. Re-uses the
 * shared half-resolution depth pre-pass (same as god rays), so it is a
 * depth pre-pass participant: its fullscreen quad is pulled out of the
 * scene while the depth view renders, then re-added before the main view.
 *
 * The material packs up to MAX_LIGHTS scattering contributors into
 * uniform arrays each frame; the CPU picks the strongest by proximity ×
 * reach from the scene's point/spot lights (provided via
 * FrameContext::scene_light_entities).
 *
 * A separate bounded analytic local-volume list adds soft enclosed-lamp glow
 * within this pass. It follows live source power/position and scene depth,
 * independently of optical-axis flares and the medium's scattering budget.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_VOLFOG_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_VOLFOG_FEATURE_H_

#include "rhi/render_feature.h"
#include "render_tuning.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>

#include <utils/Entity.h>

namespace heaven {

class VolfogFeature final : public IRenderFeature {
public:
    static constexpr int MAX_LIGHTS =
        render_tuning::volumetric_fog.maximum_lights;

    [[nodiscard]] const char* name() const override { return "volumetric_fog"; }

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
    filament::Material*         material_  = nullptr;
    filament::MaterialInstance* mi_        = nullptr;
    filament::VertexBuffer*     vb_        = nullptr;
    filament::IndexBuffer*      ib_        = nullptr;
    filament::Texture*          noise_tex_ = nullptr;
    utils::Entity               entity_;
    bool                        in_scene_  = false;

    filament::Texture*          bound_depth_ = nullptr;
    uint32_t                    frame_index_ = 0;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_VOLFOG_FEATURE_H_
