/*
 * HeavenBench — Depth pre-pass target
 *
 * God rays, volumetric fog and heat haze all sample a half-resolution
 * scene-depth texture. The backend renders a depth-only View into a
 * dedicated RenderTarget once per frame (whenever any enabled feature
 * needs it), then features read the resulting texture via
 * FrameContext::depth_texture.
 *
 * The scene-depth texture is produced as an F COLOR attachment holding
 * linear eye-space distance (metres), NOT as a sampleable depth attachment:
 * in this Filament build the custom render-target's DEPTH attachment retains
 * its clear value (sampling it returns zero everywhere), whereas COLOR
 * attachments are reliably written and sampleable. A parallel set of
 * depth-proxy renderables (see DepthProxyBuilder) draw the scene with the
 * `heaven_depth` material, which writes that linear distance to colour. The
 * DEPTH32F attachment is kept write-only purely so the colour target keeps
 * the NEAREST surface per pixel; it is never sampled.
 *
 * This subsystem owns that depth View / Camera / RenderTarget, the F
 * linear-depth + write-only depth attachments, the shared `heaven_depth`
 * base Materials, and a dedicated proxy Scene cleared to the far value
 * (2000 m) so empty pixels read "far". Per-surface instances are owned by
 * DepthProxyBuilder. It is core infrastructure,
 * not a toggleable feature — volfog, god rays and heat haze all depend on it.
 */
#ifndef HEAVENBENCH_SRC_RHI_DEPTH_PREPASS_H_
#define HEAVENBENCH_SRC_RHI_DEPTH_PREPASS_H_

#include "rhi/backend_core.h"
#include "engine/camera.h"

#include <utils/Entity.h>

namespace filament {
class View;
class Camera;
class RenderTarget;
class Texture;
class Scene;
class Skybox;
class Material;
class MaterialInstance;
}

namespace heaven {

class DepthPrePass {
public:
    explicit DepthPrePass(const BackendCore& core) : core_(core) {}
    ~DepthPrePass();

    // Lazily (re)create the half-res depth target + view for the given
    // full-resolution viewport. No-op when the size is unchanged.
    void ensure(int w, int h);

    // Point the depth camera at the same view as the main camera. Called
    // each frame before rendering the pre-pass.
    void aim(const Camera& camera, int viewport_w, int viewport_h);

    [[nodiscard]] filament::View*    view()          const { return view_; }
    // The Scene_Depth_Texture: an F COLOR attachment holding linear
    // eye-space distance (metres). Empty pixels read the far value (2000).
    [[nodiscard]] filament::Texture* depth_texture() const { return linear_depth_tex_; }
    // The dedicated proxy scene the depth view renders. Depth-proxy
    // renderables (DepthProxyBuilder) are added here; it is cleared to the
    // far value by clear_skybox_.
    [[nodiscard]] filament::Scene*   proxy_scene()   const { return depth_scene_; }
    // The shared `heaven_depth` MaterialInstance every opaque proxy renderable
    // uses. Null if the material failed to build (consumers fall back to no
    // depth).
    [[nodiscard]] filament::Material* material() const { return depth_material_; }
    // The `heaven_depth_masked` base Material used for ALPHA-TESTED foliage
    // proxies. DepthProxyBuilder creates one instance per foliage diffuse
    // texture from this (binding the diffuse alpha as the cutout), so the depth
    // silhouette matches the see-through foliage rather than a solid card. Null
    // if the masked material failed to build (foliage then falls back to the
    // opaque instance, i.e. the prior solid-card behaviour).
    [[nodiscard]] filament::Material* masked_material() const { return depth_masked_material_; }
    [[nodiscard]] int width()  const { return tw_; }
    [[nodiscard]] int height() const { return th_; }

private:
    // Build the size-independent resources (materials, proxy
    // scene + clear skybox) exactly once. Safe to call every ensure().
    void build_once();

    BackendCore core_;
    filament::View*         view_      = nullptr;
    filament::Camera*       camera_    = nullptr;
    utils::Entity           cam_entity_;
    filament::RenderTarget* rt_        = nullptr;
    // F COLOR attachment: linear eye-space distance in metres (sampleable).
    filament::Texture*      linear_depth_tex_ = nullptr;
    // DEPTH32F attachment: write-only depth test for the proxy render; never
    // sampled (sampling a custom RT's depth attachment returns zero here).
    filament::Texture*      depth_tex_ = nullptr;

    // Size-independent, built once by build_once().
    filament::Scene*            depth_scene_   = nullptr;  // proxy renderables live here
    filament::Skybox*           clear_skybox_  = nullptr;  // clears colour to far (2000)
    filament::Material*         depth_material_ = nullptr;  // heaven_depth
    // heaven_depth_masked — base material for alpha-tested foliage proxies.
    // DepthProxyBuilder owns the per-foliage instances it creates from this.
    filament::Material*         depth_masked_material_ = nullptr;
    bool                        built_once_     = false;
    // Set if building the F colour / DEPTH32F target failed (e.g. the GPU
    // does not support a sampleable F colour attachment). When true the
    // pass stays disabled: depth_texture() returns null so consumers fall back
    // to no occlusion, and ensure() does not re-attempt / re-log every frame

    bool                        target_failed_  = false;

    int                     tw_ = 0;
    int                     th_ = 0;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_DEPTH_PREPASS_H_
