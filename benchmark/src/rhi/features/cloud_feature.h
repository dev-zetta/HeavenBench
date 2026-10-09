/*
 * HeavenBench — Cloud layer feature
 *
 * Animated cloud plane drawn by a device-domain quad at far depth.
 * Dual-layer scrolling cloud textures with a mask and sun/moon lighting.
 *
 * Dormant by default: this plane approximation is separate from the
 * volumetric cloud feature. The renderable is built but only added to the
 * scene when explicitly enabled; opaque geometry occludes it naturally.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_CLOUD_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_CLOUD_FEATURE_H_

#include "rhi/render_feature.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>

#include <utils/Entity.h>

namespace heaven {

class CloudFeature final : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "clouds"; }

    void init(RenderContext& ctx, const AppConfig& config) override;
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void destroy(RenderContext& ctx);

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

private:
    filament::Material*         material_ = nullptr;
    filament::MaterialInstance* mi_       = nullptr;
    filament::VertexBuffer*     vb_       = nullptr;
    filament::IndexBuffer*      ib_       = nullptr;
    utils::Entity               entity_;
    filament::Texture*          tex1_     = nullptr;
    filament::Texture*          tex2_     = nullptr;
    filament::Texture*          mask_     = nullptr;
    bool                        in_scene_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_CLOUD_FEATURE_H_
