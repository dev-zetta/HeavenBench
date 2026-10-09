/*
 * HeavenBench — Procedural sky-sphere feature
 *
 * Draws a large camera-centred UV sphere with a per-pixel sky gradient,
 * sun disc, and animated cloud layer (heaven_sky.mat). It replaces the
 * *visible* background; Filament's cubemap Skybox is detached while this
 * feature is enabled (the backend re-attaches it when the feature is
 * off). The IBL probe (procedural cubemap reflections) is owned by the
 * backend and is unaffected — the sphere only paints what the camera
 * sees, not the lighting environment.
 *
 * Migrated verbatim out of FilamentBackend::init_sky / update_sky.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_SKY_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_SKY_FEATURE_H_

#include "rhi/render_feature.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>

#include <utils/Entity.h>

namespace heaven {

class SkyFeature final : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "sky"; }

    void init(RenderContext& ctx, const AppConfig& config) override;
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void destroy(RenderContext& ctx);

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

private:
    filament::Material*         material_  = nullptr;
    filament::MaterialInstance* mi_        = nullptr;
    filament::VertexBuffer*     vb_        = nullptr;
    filament::IndexBuffer*      ib_        = nullptr;
    utils::Entity               entity_;
    const filament::Texture*          cloud_tex_  = nullptr;  // owned by resource cache
    const filament::Texture*          cloud_mask_ = nullptr;  // owned by resource cache
    const filament::Texture*          moon_tex_ = nullptr;
    const filament::Texture*          moon_normal_tex_ = nullptr;
    const filament::Texture*          milky_way_tex_ = nullptr;
    bool                        moon_tex_owned_ = false;
    bool                        moon_normal_tex_owned_ = false;
    bool                        milky_way_tex_owned_ = false;
    bool                        in_scene_   = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_SKY_FEATURE_H_
