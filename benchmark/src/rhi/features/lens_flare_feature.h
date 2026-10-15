/*
 * HeavenBench — Lens flare feature
 *
 * One camera-facing halo quad per warm-orange luminaire (lamp posts,
 * lanterns, torches, glass). The vertex shader anchors the quad at the
 * light's world position (baked into the POSITION attribute) and
 * rebuilds it as a camera-facing billboard scaled by proximity; the
 * fragment shader ramps brightness with distance and samples the glow
 * sprite. Flares reveal on approach (invisible past ~25 m, full bloom
 * by ~4 m), matching the original benchmark.
 *
 * The feature reads the backend's scene lights via
 * FrameContext::scene_lights (it does not own or spawn lights). Per
 * frame it tracks each flare's parent — a floating-island group offset
 * or an animated mesh instance — so flares stay glued to their lamp.
 *
 * Lens flares are a depth-pre-pass participant on two counts: their
 * alpha quads must be pulled from the main scene during the depth
 * render (so they don't write bright depth "voids" that god rays /
 * volfog would read as sky), AND they now sample the linear
 * scene-depth texture themselves for soft occlusion. The material
 * (heaven_lens_flare.mat) compares each flare anchor's eye-space
 * distance against the sampled scene depth and fades the flare out
 * where opaque geometry sits in front of it — a continuous fade with
 * no hard clip edge, replacing the old hardware depth test.
 *
 * Migrated verbatim out of FilamentBackend::init_lens_flares /
 * update_lens_flares.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_LENS_FLARE_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_LENS_FLARE_FEATURE_H_

#include "rhi/render_feature.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>

#include <utils/Entity.h>

#include <glm/glm.hpp>

#include <vector>

namespace heaven {

class LensFlareFeature final : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "lens_flares"; }

    void init(RenderContext& ctx, const AppConfig& config) override;
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void append_temporal_draws(std::vector<TemporalEffectDraw>& draws) const override;
    void release_shared_targets(RenderContext& ctx) override;
    void destroy(RenderContext& ctx);

    // Depth-pre-pass participant. needs_depth_prepass() is gated on
    // built_: the feature only samples (and thus only needs) the depth
    // texture once its flares exist, mirroring godrays' mi_-gating. Its
    // additive quads are pulled from the scene while the shared depth
    // view renders (so they don't pollute the depth) and re-added before
    // the main view. Soft occlusion is done in the fragment shader by
    // sampling the linear scene-depth texture bound in update().
    [[nodiscard]] bool needs_depth_prepass() const override { return built_; }
    void on_depth_prepass_begin(RenderContext& ctx) override;
    void on_depth_prepass_end(RenderContext& ctx) override;

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

private:
    struct Flare {
        utils::Entity                entity;
        utils::Entity                light_entity; // borrowed illumination source
        filament::VertexBuffer*      vb = nullptr;
        filament::MaterialInstance*  mi = nullptr;
        glm::vec3                    world_pos{0.0f};
        glm::vec3                    spawn_pos{0.0f};
        glm::vec3                    local_pos{0.0f};
        int                          floating_group = -1;
        // -1 = static / handled by floating_group; otherwise the flare
        // follows instances[instance_index] (its world_pos recomputed
        // each frame as instance.transform * local_pos).
        int                          instance_index = -1;
        std::size_t                  source_index = 0;
        float                        nominal_intensity = 0.0f;
        float                        base_brightness = 0.0f;
    };

    void build(RenderContext& ctx, const FrameContext& frame);

    filament::Material*      material_ = nullptr;
    filament::IndexBuffer*   ib_       = nullptr;     // shared 2-triangle quad
    filament::Texture*       glow_tex_ = nullptr;     // resource cache
    std::vector<Flare>       flares_;
    bool                     built_    = false;
    bool                     in_scene_ = false;

    // Last linear scene-depth texture bound onto the flare material
    // instances, so we only rebind on first availability / resize (the
    // backend rebuilds the depth texture when the viewport changes).
    filament::Texture*       bound_depth_ = nullptr;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_LENS_FLARE_FEATURE_H_
