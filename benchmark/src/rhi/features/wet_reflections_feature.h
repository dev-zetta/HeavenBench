/*
 * HeavenBench — Wet-surface reflections feature
 *
 * Part of the weather system (see
 * Hosts the Wetness_System
 * and the selected modern rendering technique: wet-surface
 * screen-space reflections. The feature owns NO renderables — it operates
 * purely by setting a per-view material global (the wetness term read by
 * the lit materials as getMaterialGlobal0()) and by retuning the main
 * view's screen-space-reflections options while rain is active. This is a
 * legitimate IRenderFeature: the "resource" it owns is the view state it
 * sets and restores.
 *
 * Capability gate: the enhanced reflection path notionally requires
 * a GPU capability (a Vulkan extension / feature level). `init` probes for
 * it; if unavailable it logs a diagnostic naming the disabled technique,
 * initialises the feature disabled, and leaves the view's SSR options and
 * the wetness material global untouched so the Shipping_Look is preserved.
 *
 * Parity: the wetness global defaults to {0,0,0,1} (Filament's
 * documented default global, the no-op path in the lit shaders) and the
 * shipping ScreenSpaceReflectionsOptions are saved at init, so destroy and
 * a runtime toggle-off restore EXACTLY the shipping reflection state.
 *
 * Skeleton scope: the IRenderFeature scaffolding, the
 * capability gate in init, saving the shipping SSR options, and the
 * destroy / toggle-off restore. The per-frame wetness-global push and the
 * SSR retune land in: update() reads the weather's wetness
 * global, pushes it to material global 0, and retunes the view's SSR
 * options as a function of effective intensity (restoring exact shipping
 * parity at intensity 0).
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_WET_REFLECTIONS_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_WET_REFLECTIONS_FEATURE_H_

#include "rhi/render_feature.h"

#include <filament/Options.h>   // filament::ScreenSpaceReflectionsOptions

namespace heaven {

// Not `final`: the capability decision is a protected virtual seam
// (`probe_capability`) so a test can subclass and force the
// "capability unavailable" branch deterministically without a GPU
// dependency. The default seam is the exact production
// feature-level check, so behaviour is unchanged for the shipping feature.
class WetReflectionsFeature : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "wet-reflections"; }

    void init(RenderContext& ctx, const AppConfig& config) override;   // cap gate
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void destroy(RenderContext& ctx);                                  // (restore)

    // A live SSR edit must also change the baseline restored by dry weather
    // or disabling this feature; other wet-surface shading remains independent.
    void set_reflections_enabled(bool enabled) { shipping_ssr_.enabled = enabled; }

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

    // capability / Vulkan-extension probe seam. Returns whether the
    // device exposes the capability the enhanced reflection path needs.
    // The default implementation is the production check — the technique
    // requires the standard FEATURE_LEVEL_1 (GLES-3.0-class) feature set
    // that per-view material globals and screen-space reflections depend
    // on; FEATURE_LEVEL_0 (GLES 2.0) backends lack it. It is `virtual` so a
    // test can override it to force the unavailable branch
    // deterministically; on every real desktop backend it returns true, so
    // the gate degrades to "always capable" in production.
    [[nodiscard]] virtual bool probe_capability(filament::Engine& engine) const;

private:
    // Restore the Shipping_Look reflection state on `ctx.view`: wetness
    // material global back to {0,0,0,1} and SSR back to the saved shipping
    // options. The single place the parity-restore lives, shared by
    // destroy() and on_enabled_changed(false).
    void restore_shipping_state(RenderContext& ctx);

    // capability / Vulkan-extension gate. The shipping technique runs
    // entirely through Filament's already-enabled SSR plus the wetness
    // global, so this degrades to "always capable" on real desktop
    // backends, but the disabled-path branch is real and tested.
    bool ssr_capable_ = true;

    // The shipping ScreenSpaceReflectionsOptions, captured at init so the
    // feature can restore EXACT parity when disabled.
    filament::ScreenSpaceReflectionsOptions shipping_ssr_{};
    bool shipping_ssr_saved_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_WET_REFLECTIONS_FEATURE_H_
