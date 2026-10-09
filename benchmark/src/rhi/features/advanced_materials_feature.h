/*
 * HeavenBench — Advanced material shading models feature (Group A)
 *
 * The single coordinating IRenderFeature for the four advanced Filament
 * shading models — clear coat, anisotropic,
 * subsurface/translucency, and cloth/sheen  — selected per
 * surface by the data-driven Material_Model_Selector. It is modeled on
 * the WetReflectionsFeature precedent: it owns NO renderables. Its sole job is
 * to carry the Group-A EnabledModels from AppConfig::features into the
 * MaterialInstanceCache selector BEFORE the scene is built and to restore the
 * Shipping_Look assignment when every model is disabled.
 *
 * Why a feature at all?  require every  capability
 * to be registered exactly once in init_features regardless of its integration
 * type, with NO per-effect data member on FilamentBackend and NO per-effect work
 * inlined into render_frame. The actual selection lives in the
 * MaterialInstanceCache extension; this thin feature satisfies the
 * single-registration obligation and owns the toggle lifecycle / parity
 * restoration. The cache is an existing backend infrastructure subsystem passed
 * in by a non-owning pointer, so the backend gains no new member.
 *
 * Carry-in timing: init() runs from init_features(), which executes during
 * init_pipelines() — BEFORE the first render_frame() where SceneBuilder builds
 * the scene and the cache bakes a MaterialInstance per surface. So the
 * EnabledModels set here are in place before any instance is created.
 *
 * Parity: with every Group-A toggle off the carried EnabledModels are
 * all-false, so the selector returns each surface's Shipping_Look model and the
 * existing base material + parameter writes are used unchanged (≤1/255).
 *
 * Runtime toggle: per-surface model assignment is baked into
 * the cached MaterialInstances at scene-build time and the cache documents "no
 * effect on already-cached instances". A runtime flip therefore cannot be
 * applied by the next rendered frame once the scene exists; on_enabled_changed
 * treats that delay as a failure — it emits a diagnostic naming the capability
 * and RETAINS the prior valid selector configuration (leaving the cache
 * untouched) rather than render a partially-applied state.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_ADVANCED_MATERIALS_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_ADVANCED_MATERIALS_FEATURE_H_

#include "rhi/render_feature.h"
#include "engine/material_model_ref.h"  // material_model_ref::EnabledModels

namespace heaven {

class MaterialInstanceCache;

class AdvancedMaterialsFeature : public IRenderFeature {
public:
    // The cache is an existing backend infrastructure subsystem (owned by
    // FilamentBackend). The feature borrows it by non-owning pointer — it never
    // owns or frees it — so the backend gains no per-effect member.
    explicit AdvancedMaterialsFeature(MaterialInstanceCache* cache) noexcept
        : cache_(cache) {}

    [[nodiscard]] const char* name() const override { return "advanced-materials"; }

    void init(RenderContext& ctx, const AppConfig& config) override;
    void destroy(RenderContext& ctx);

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

private:
    // Non-owning pointer to the backend-owned MaterialInstanceCache (the
    // Material_Model_Selector lives there). Borrowed, never freed.
    MaterialInstanceCache* cache_ = nullptr;

    // The per-model enable gate carried into the cache at init — the prior valid
    // configuration retained across a runtime toggle.
    material_model_ref::EnabledModels configured_models_{};
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_ADVANCED_MATERIALS_FEATURE_H_
