/*
 * HeavenBench — Wet-surface reflections feature implementation
 *
 * scope: the per-frame Wetness_System proper. update() reads
 * the weather's effective intensity, pushes the wetness global
 * (specBoost, diffuseDarken, wetAmount, 1) to material global 0 for the
 * lit shaders, and retunes the view's screen-space-reflections options
 * (maxDistance / stride) as a monotonic function of intensity. At
 * intensity 0 it restores the wetness global to {0,0,0,1} and SSR to the
 * saved shipping options, giving bit-identical Shipping_Look parity with
 * no discontinuity at the boundary.  established the
 * scaffolding, the capability gate, and the destroy / toggle-off
 * restore.
 */
#include "rhi/features/wet_reflections_feature.h"
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/View.h>

#include <math/vec4.h>

#include <cstdio>

namespace heaven {

bool WetReflectionsFeature::probe_capability(filament::Engine& engine) const {
    // Production  capability check. The technique needs the standard
    // (FEATURE_LEVEL_1, GLES-3.0-class) feature set that screen-space
    // reflections and per-view material globals depend on; FEATURE_LEVEL_0
    // backends (GLES 2.0) lack it. On every real desktop Vulkan/GL backend
    // this is satisfied, so the gate degrades to "always capable". A test
    // overrides this to force the unavailable branch.
    return engine.getActiveFeatureLevel() >=
           filament::Engine::FeatureLevel::FEATURE_LEVEL_1;
}

void WetReflectionsFeature::init(RenderContext& ctx, const AppConfig& config) {
    // Honour the feature toggle first. `--no-wet-reflections` ships
    // the feature disabled; in that state we leave the view's SSR options
    // and the wetness material global untouched so the Shipping_Look is
    // preserved.
    enabled_ = config.features.wet_reflections;

    // ──  capability / Vulkan-extension gate ─────────────────
    // The enhanced reflection path notionally requires a GPU capability.
    // Because all rendering goes through Filament (lib/filament must not be
    // patched beyond the two documented patches), we treat "requires a
    // Vulkan extension" as a Filament capability probe (see
    // probe_capability()). The probe is a protected virtual seam so a unit
    // test can force the unavailable branch deterministically without a GPU
    // dependency; in production it is the real feature-level
    // check, which every desktop Vulkan/GL backend satisfies.
    ssr_capable_ = probe_capability(*ctx.engine);

    if (!ssr_capable_) {
        // Disable and leave SSR / the material global untouched.
        std::printf("  Wet reflections: required GPU capability unavailable "
                    "— technique disabled\n");
        enabled_ = false;
        return;
    }

    // ── Save the shipping SSR options for an exact restore  ──
    // configure_post_processing() applies the shipping SSR options to the
    // view lazily on the first rendered frame — AFTER feature init — so
    // reading view->getScreenSpaceReflectionsOptions() here would capture
    // Filament's defaults, not the shipping look. Capture the documented
    // shipping values (post_process.cpp) explicitly so destroy / toggle-off
    // restore the EXACT shipping reflection state regardless of init order.
    shipping_ssr_ = filament::ScreenSpaceReflectionsOptions{};
    shipping_ssr_.enabled     = config.features.reflections;
    shipping_ssr_.thickness   = render_tuning::post_process.ssr.x;
    shipping_ssr_.bias        = render_tuning::post_process.ssr.y;
    shipping_ssr_.maxDistance = render_tuning::post_process.ssr.z;
    shipping_ssr_.stride      = render_tuning::post_process.ssr.w;
    shipping_ssr_saved_ = true;

    std::printf("  Wet reflections: wet-surface SSR + wetness shading initialized%s\n",
                enabled_ ? "" : " (disabled)");
}

void WetReflectionsFeature::update(RenderContext& ctx, const FrameContext& frame) {
    // Stay inert if the capability gate disabled the feature  or the
    // feature is toggled off, and guard the weather handle so we never
    // dereference a null FrameContext::weather.
    if (!ssr_capable_ || !enabled_) return;
    if (!frame.weather) return;
    if (!ctx.view) return;

    // The accumulated surface-wetness level that drives the wet look this
    // frame. Unlike effective_intensity() (the instantaneous rain level), this
    // LAGS the precipitation: it soaks toward the rain target over
    // wet_soak_seconds when it starts raining and decays toward 0 over
    // wet_dry_seconds when it stops (the accumulation runs in
    // WeatherState::advance(dt), already called once per frame from the main
    // loop). Everything below is a pure function of it, so the wetness shading
    // and the reflection retune build up and fade out together with the ground
    // soaking, not instantly with the commanded rain.
    const float w = frame.weather->surface_wetness();

    // ── Surface wetness 0: exact Shipping_Look parity  ────
    // With no accumulated wetness the surface_wetness_global() collapses to
    // (0,0,0,1). Restore the documented default material global and the saved
    // shipping SSR options so the output is bit-identical to the Shipping_Look
    // — and so the retune below converges to exactly these values as w → 0,
    // leaving no discontinuity at the parity boundary. The lit shaders' wetness
    // branch only runs when g.z > 0, so this is the no-op path end to end. This
    // is hit both before any rain has soaked in AND after the ground has fully
    // dried out.
    if (w <= 0.0f) {
        restore_shipping_state(ctx);
        return;
    }

    // ── Wetness global  ───────────────────────
    // Push (specBoost, diffuseDarken, wetAmount, 1) to material global 0 using
    // the ACCUMULATED wetness packer; the lit materials read it as
    // getMaterialGlobal0() and, when g.z > 0, weight it by the surface-up
    // alignment to raise specular and darken diffuse on rain-exposed surfaces.
    const glm::vec4 g = frame.weather->surface_wetness_global();
    ctx.view->setMaterialGlobal(0, filament::math::float4{g.x, g.y, g.z, g.w});

    // ── SSR retune (the selected modern technique) ────────
    // Retune Filament's already-enabled screen-space reflections as a
    // monotonic function of the accumulated wetness, starting from the saved
    // shipping options so the endpoints are a single source of truth and the
    // values converge back to the shipping look as w → 0 (no parity-boundary
    // jump). Driving from surface_wetness() (not effective_intensity()) means
    // reflections strengthen as the ground soaks and fade as it dries, in step
    // with the wet shading:
    //   - maxDistance grows so wet surfaces mirror geometry further away
    //     (lamp pools on the road reflect more of the scene in the storm);
    //   - stride tightens (toward Filament's 1.0 floor) for fewer step
    //     artefacts / crisper reflections on the wet cobblestone.
    // Both interpolate linearly with w from the shipping value (w=0) to the
    // full-storm endpoint (w=1); the other shipping fields (enabled,
    // thickness, bias) are preserved.
    constexpr float kStormMaxDistance =
        render_tuning::post_process.wet_ssr.x;
                                                // pulled back from 60.0 — SSR is view-dependent and
                                                // swims as the camera translates; keep it closer to
                                                // the shipping look so it adds subtle wet reflection
                                                // without a strong swimming mirror)
    constexpr float kStormStride =
        render_tuning::post_process.wet_ssr.y;
                                                // was 1.0 — nearer shipping 1.5 to reduce swim)
    filament::ScreenSpaceReflectionsOptions ssr = shipping_ssr_;
    ssr.maxDistance = shipping_ssr_.maxDistance +
                      (kStormMaxDistance - shipping_ssr_.maxDistance) * w;
    ssr.stride = shipping_ssr_.stride +
                 (kStormStride - shipping_ssr_.stride) * w;
    ctx.view->setScreenSpaceReflectionsOptions(ssr);
}

void WetReflectionsFeature::restore_shipping_state(RenderContext& ctx) {
    // Restore EXACT Shipping_Look reflection state: the wetness
    // material global back to Filament's default {0,0,0,1} (the no-op path
    // in the lit shaders) and SSR back to the saved shipping options. Only
    // touch SSR if we actually captured the shipping options, so a feature
    // that disabled at the capability gate never writes the view.
    if (!ctx.view) return;
    ctx.view->setMaterialGlobal(0, filament::math::float4{0.0f, 0.0f, 0.0f, 1.0f});
    if (shipping_ssr_saved_) {
        ctx.view->setScreenSpaceReflectionsOptions(shipping_ssr_);
    }
}

void WetReflectionsFeature::on_enabled_changed(RenderContext& ctx, bool on) {
    // Turning OFF restores the shipping reflection state by the next frame
    // Turning ON is a no-op here — the per-frame wetness push and
    // SSR retune resume in update(), so the visual contribution
    // is restored without any work at toggle time.
    if (!on) {
        restore_shipping_state(ctx);
    }
}

void WetReflectionsFeature::destroy(RenderContext& ctx) {
    // Leave the view exactly as the Shipping_Look expects: no
    // owned GPU resources to free (the feature renders nothing), just the
    // view-state restore.
    restore_shipping_state(ctx);
}

} // namespace heaven
