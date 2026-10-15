/*
 * HeavenBench — Rain feature (falling precipitation + splashes)
 *
 * Part of the weather system (see
 * Renders falling
 * precipitation  and owns the Splash_System. Modelled on the
 * ParticleFeature precedent: build-once GPU buffers, all per-particle
 * motion in the vertex shader, only a handful of `time` + `intensity`
 * uniforms pushed per frame. The CPU never moves a vertex.
 *
 * Driven by FrameContext::weather (a read-only WeatherState handle);
 * rain renders nothing only while Rain_Intensity is 0 (e.g. via
 * `--rain-intensity 0` or `--no-rain`). The default intensity is now
 * medium (0.5), so the default run renders the storm.
 *
 * Skeleton scope: the IRenderFeature scaffolding, resource
 * init with graceful failure, the toggle/destroy lifecycle, and
 * the depth-pre-pass declaration. The build-once streaks/splashes and
 * the per-frame uniform updates land in tasks 6.2 / 6.3 — build_rain /
 * build_splashes / update are minimal stubs here.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_RAIN_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_RAIN_FEATURE_H_

#include "rhi/render_feature.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>

#include <utils/Entity.h>

#include <cstdint>

namespace heaven {

class RainFeature final : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "rain"; }

    void init(RenderContext& ctx, const AppConfig& config) override;   // graceful
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void append_temporal_draws(std::vector<TemporalEffectDraw>& draws) const override;
    void release_shared_targets(RenderContext& ctx) override;
    void destroy(RenderContext& ctx);

    // Splash projection samples scene depth to place impacts on the
    // ground, so the shared depth pre-pass must run while rain is
    // active. Only true once the renderables are actually built.
    [[nodiscard]] bool needs_depth_prepass() const override {
        return enabled_ && built_;
    }

    // The splash material SAMPLES the shared depth pre-pass texture
    // (FrameContext::depth_texture), which is that pass's own depth
    // attachment. Leaving the streak/splash renderables in the scene
    // during the depth render makes Filament bind a descriptor set that
    // reads the attachment it is writing — a Vulkan read/write feedback
    // hazard that crashes the optimized build. So, exactly like god rays
    // and volumetric fog, pull both quads out of the scene while the
    // depth view renders and re-add them before the main view.
    void on_depth_prepass_begin(RenderContext& ctx) override;
    void on_depth_prepass_end(RenderContext& ctx) override;

    // Build-once instrumentation (mirrors GrassFeature::generation_count).
    // The streak / splash geometry is built once on the first active frame
    // and animated through uniforms thereafter; the backend reads these
    // through debug_build_counters() so a gated smoke case can confirm each
    // builder ran exactly once and the per-frame path never regenerates
    // geometry (weather spec). Incremented in build_rain /
    // build_splashes.
    [[nodiscard]] uint64_t streak_build_count() const { return streak_build_count_; }
    [[nodiscard]] uint64_t splash_build_count() const { return splash_build_count_; }

    // Test-only: the depth texture the splash instance is currently bound to
    // (the shared FrameContext::depth_texture last seen in update()). Null until
    // the shared depth pre-pass has produced a texture and an active frame bound
    // it. A gated smoke case  asserts this equals
    // the backend's shared depth texture, proving the feature reads scene depth
    // only through FrameContext::depth_texture. Mirrors the build-count getters:
    // instrumentation only, exposes no internals beyond the bound handle.
    [[nodiscard]] const filament::Texture* bound_depth_texture() const { return bound_depth_; }

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

private:
    void build_rain(RenderContext& ctx);     // build-once streak quads  —
    void build_splashes(RenderContext& ctx); // build-once impact quads  —

    // Streaks: one renderable, up to the centralized kMaxStreaks capacity.
    // quads. heaven_rain.filamat.
    filament::Material*         rain_material_ = nullptr;
    filament::MaterialInstance* rain_mi_       = nullptr;
    filament::VertexBuffer*     rain_vb_       = nullptr;
    filament::IndexBuffer*      rain_ib_       = nullptr;
    utils::Entity               rain_entity_;

    // Splashes: a SINGLE fullscreen world-space quad (Breath-of-the-Wild
    // screen-space rain), built once and positioned in front of the camera each
    // frame exactly like the godrays / volfog passes. The fragment shader
    // samples the shared depth pre-pass per pixel, reconstructs world position +
    // normal, ground-tests it, and tiles ripples in world XY. heaven_splash.filamat.
    filament::Material*         splash_material_ = nullptr;
    filament::MaterialInstance* splash_mi_       = nullptr;
    filament::VertexBuffer*     splash_vb_       = nullptr;
    filament::IndexBuffer*      splash_ib_       = nullptr;
    utils::Entity               splash_entity_;

    // Precipitation / splash sprite textures (owned by the resource
    // cache; held here only as non-owning handles).
    filament::Texture*          drop_tex_   = nullptr;
    filament::Texture*          splash_tex_ = nullptr;

    bool built_     = false; // build-once geometry created
    bool in_scene_  = false; // renderables currently added to the scene
    bool available_ = false; // all materials/textures loaded

    // Build-once counters (read through the backend's debug_build_counters()).
    // Each is bumped exactly once, at its build site, and stays put for the
    // life of the run — the per-frame path only pushes uniforms / transforms.
    uint64_t streak_build_count_ = 0;
    uint64_t splash_build_count_ = 0;

    // Last depth texture bound to the splash instance, so we only rebind
    // on change/resize (binds the real depth pre-pass texture).
    filament::Texture*          bound_depth_ = nullptr;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_RAIN_FEATURE_H_
