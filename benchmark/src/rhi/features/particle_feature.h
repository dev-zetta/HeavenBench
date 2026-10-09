/*
 * HeavenBench — Particle + heat-haze feature
 *
 * GPU-driven billboard particles (chimney smoke, torch flame, brazier
 * glow, fireflies) plus the companion heat-haze refraction pass. These
 * are one feature because the heat-haze billboards SHARE the smoke
 * emitters' vertex/index buffers — splitting them would mean one owner
 * holding buffers the other draws from, inviting lifetime bugs.
 *
 * All per-particle motion happens in the materials' vertex shaders, so
 * per frame the feature only pushes a `time` uniform per emitter and
 * (for floating-island emitters) a Z translation.
 *
 * Heat haze adds a second render pass: the main view is captured to an
 * offscreen RGBA16F texture, then a separate composite view blits it
 * back to the swap chain and draws refraction billboards that sample the
 * captured texture at offset UVs. The feature opts into that pass only
 * when a chimney is within visible range (it is otherwise free).
 *
 * Migrated verbatim out of FilamentBackend::init_particles /
 * set_particle_emitters / update_particles / init_composite /
 * ensure_scene_capture and the composite section of render_frame.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_PARTICLE_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_PARTICLE_FEATURE_H_

#include "rhi/render_feature.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/View.h>

#include <utils/Entity.h>

#include <glm/glm.hpp>

#include <vector>

namespace heaven {

class ParticleFeature final : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "particles"; }

    void init(RenderContext& ctx, const AppConfig& config) override;
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void release_shared_targets(RenderContext& ctx) override;
    void destroy(RenderContext& ctx);

    // Receives the scene's particle emitters (from the loader). Stored
    // and built lazily on the first update().
    void set_emitters(std::span<const ParticleEmitter> emitters);

    // ── Heat-haze composite participation ───────────────────────
    [[nodiscard]] bool wants_scene_capture(const FrameContext& frame) const override;
    void begin_scene_capture(RenderContext& ctx, const FrameContext& frame) override;
    void render_composite(RenderContext& ctx) override;

    // Heat haze samples the shared half-res depth pre-pass for
    // occlusion, so it needs the backend to run that pass (and provide
    // the depth texture via FrameContext). It does NOT remove any
    // renderables for the pass — its haze quads live in a separate
    // composite scene and its smoke quads should stay visible — so the
    // begin/end hooks are intentionally left as no-ops.
    [[nodiscard]] bool needs_depth_prepass() const override {
        return heat_haze_enabled_ && !haze_emitters_.empty();
    }

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

private:
    // One GPU-driven billboard emitter. The heat-haze companion reuses
    // the smoke emitter's vb/ib (so its own vb/ib stay null and aren't
    // double-freed).
    struct Emitter {
        utils::Entity                entity;
        filament::VertexBuffer*      vb = nullptr;
        filament::IndexBuffer*       ib = nullptr;
        filament::MaterialInstance*  mi = nullptr;
        glm::vec3                    origin{0.0f};
        int                          floating_group = -1;
    };

    void build_emitters(RenderContext& ctx);
    void ensure_scene_capture(RenderContext& ctx, int w, int h);

    // Materials (owned)
    filament::Material*          smoke_material_   = nullptr;
    filament::Material*          flame_material_   = nullptr;
    filament::Material*          glow_material_    = nullptr;
    filament::Material*          firefly_material_ = nullptr;
    filament::Texture*           glow_tex_         = nullptr; // resource cache

    std::vector<Emitter>         emitters_;
    std::vector<ParticleEmitter> pending_;
    bool                         built_   = false;
    bool                         in_scene_ = false;

    // ── Heat-haze composite pipeline ────────────────────────────
    bool                         heat_haze_enabled_ = true;
    filament::Material*          heat_haze_material_ = nullptr;
    std::vector<Emitter>         haze_emitters_;

    filament::Material*          blit_material_ = nullptr;
    filament::MaterialInstance*  blit_mi_       = nullptr;
    filament::VertexBuffer*      blit_vb_       = nullptr;
    filament::IndexBuffer*       blit_ib_       = nullptr;
    utils::Entity                blit_entity_;
    filament::Scene*             composite_scene_ = nullptr;
    filament::View*              composite_view_  = nullptr;

    filament::Texture*           scene_capture_tex_   = nullptr;
    filament::Texture*           scene_capture_depth_ = nullptr;
    filament::RenderTarget*      scene_capture_rt_    = nullptr;
    int                          scene_capture_w_     = 0;
    int                          scene_capture_h_     = 0;

    // Last half-res depth pre-pass texture bound to the haze
    // instances' sceneDepth sampler (rebind only on change/resize).
    filament::Texture*           bound_depth_ = nullptr;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_PARTICLE_FEATURE_H_
