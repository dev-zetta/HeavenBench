/*
 * HeavenBench — Advanced material shading models feature implementation
 *
 * scope: the thin coordinating feature that carries the Group-A
 * EnabledModels into the MaterialInstanceCache selector before the scene is
 * built, restores Shipping_Look assignment when disabled, and owns the runtime
 * toggle lifecycle / parity restoration (the WetReflectionsFeature precedent).
 * The per-surface selection itself lives in the cache.
 */
#include "rhi/features/advanced_materials_feature.h"

#include "rhi/material_instance_cache.h"

#include <cstdio>

namespace heaven {

void AdvancedMaterialsFeature::init(RenderContext& /*ctx*/, const AppConfig& config) {
    // Gather the four Group-A toggles into the selector's enable gate. With
    // every field false the selector returns each surface's Shipping_Look model
    // (≤1/255 parity); each field independently gates whether its model
    // may be returned.
    configured_models_ = material_model_ref::EnabledModels{
        config.features.clear_coat,
        config.features.anisotropic,
        config.features.subsurface,
        config.features.cloth,
    };

    // The feature is "on" when any advanced model is requested. With all off it
    // is inert and the cache assigns the Shipping_Look model everywhere.
    enabled_ = configured_models_.clear_coat || configured_models_.anisotropic ||
               configured_models_.subsurface || configured_models_.cloth;

    // Carry the gate into the cache's Material_Model_Selector. init() runs ahead
    // of the first render_frame (where the scene is built and the cache bakes a
    // MaterialInstance per surface), so this is in place before any instance is
    // created. When every model is off this is
    // an all-false gate, restoring the Shipping_Look assignment.
    if (cache_) {
        cache_->set_advanced_models(configured_models_);
    }

    std::printf("  Advanced materials: clear_coat=%d anisotropic=%d subsurface=%d "
                "cloth=%d%s\n",
                configured_models_.clear_coat, configured_models_.anisotropic,
                configured_models_.subsurface, configured_models_.cloth,
                enabled_ ? "" : " (all disabled — Shipping_Look)");
}

void AdvancedMaterialsFeature::on_enabled_changed(RenderContext& /*ctx*/, bool on) {
    // Per-surface model assignment is baked into each surface's MaterialInstance
    // when the scene is built; the cache documents "no effect on already-cached
    // instances". A runtime flip therefore cannot be applied by the next
    // rendered frame once the scene exists. Per, treat that delay as a
    // failure: emit a diagnostic naming the capability and RETAIN the prior valid
    // selector configuration (leave the cache's EnabledModels untouched) rather
    // than render a partially-applied state. set_enabled() has already flipped
    // enabled_; we deliberately do NOT push the new gate into the cache so the
    // on-GPU assignment stays internally consistent.
    std::printf("  Advanced materials: runtime toggle %s cannot be applied to "
                "already-built material instances by the next frame — retaining "
                "the prior assignment (restart with the desired --clear-coat / "
                "--anisotropic / --subsurface / --cloth flags)\n",
                on ? "on" : "off");
}

void AdvancedMaterialsFeature::destroy(RenderContext& /*ctx*/) {
    // The feature owns no GPU resources (it renders nothing). Reset the
    // selector to the Shipping_Look assignment so any later instance build is
    // parity-clean. Guard the cache pointer: teardown destructs the features
    // before the cache is reset, but never rely on lifetime ordering.
    if (cache_) {
        cache_->set_advanced_models(material_model_ref::EnabledModels{});
    }
}

} // namespace heaven
