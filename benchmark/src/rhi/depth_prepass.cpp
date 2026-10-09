/*
 * HeavenBench — Depth pre-pass target
 *
 * Produces the Scene_Depth_Texture as an F COLOR attachment holding linear
 * eye-space distance (metres). See depth_prepass.h for why depth is produced
 * as colour rather than via a sampleable depth attachment.
 */
#include "rhi/depth_prepass.h"
#include "render_tuning.h"
#include "rhi/filament_util.h"   // glm_to_filament
#include "rhi/materials_data.h"  // k_heaven_depth_filamat

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Camera.h>
#include <filament/Viewport.h>
#include <filament/RenderTarget.h>
#include <filament/Texture.h>
#include <filament/Skybox.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>

#include <math/vec4.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <glm/glm.hpp>

#include <cstdio>
#include <utility>

namespace heaven {

// Far value written into the linear-depth colour target where no proxy
// geometry covers a pixel, so consumers read those pixels as "no geometry".
// Matches the depth camera's far plane (see aim()).
static constexpr float k_far_depth = render_tuning::fullscreen.scene_far_depth;

DepthPrePass::~DepthPrePass() {
    if (!core_.engine) return;
    auto* engine = core_.engine;
    if (rt_)               engine->destroy(rt_);
    if (linear_depth_tex_) engine->destroy(linear_depth_tex_);
    if (depth_tex_)        engine->destroy(depth_tex_);
    if (view_)             engine->destroy(view_);
    if (depth_material_)   engine->destroy(depth_material_);
    if (depth_masked_material_) engine->destroy(depth_masked_material_);
    if (clear_skybox_)     engine->destroy(clear_skybox_);
    if (depth_scene_)      engine->destroy(depth_scene_);
    if (camera_) {
        engine->destroyCameraComponent(cam_entity_);
        utils::EntityManager::get().destroy(cam_entity_);
    }
}

void DepthPrePass::build_once() {
    if (built_once_) return;
    built_once_ = true;

    auto* engine = core_.engine;

    // Dedicated scene for the depth view's proxy renderables (added later by
    // DepthProxyBuilder). Cleared to the far value via a constant-colour
    // Skybox: a custom render target cannot use Renderer::ClearOptions (those
    // apply only to the swap chain), so a Skybox is the supported way to clear
    // it. With post-processing off the colour is written verbatim, so empty
    // pixels read k_far_depth metres = "no geometry".
    depth_scene_ = engine->createScene();
    clear_skybox_ = filament::Skybox::Builder()
        .color({k_far_depth, 0.0f, 0.0f, 1.0f})
        .build(*engine);
    depth_scene_->setSkybox(clear_skybox_);

    // The depth-output base material. DepthProxyBuilder creates cached
    // per-surface instances so displacement matches visible geometry.
    depth_material_ = filament::Material::Builder()
        .package(k_heaven_depth_filamat, k_heaven_depth_filamat_size)
        .build(*engine);
    if (!depth_material_) {
        std::printf("  DepthPrePass: heaven_depth material failed to load — "
                    "scene-depth occlusion disabled for consumers\n");
        return;
    }
    // Alpha-masked variant for foliage. Loaded best-effort: if it fails,
    // foliage proxies fall back to the opaque instance (the prior solid-card
    // behaviour) rather than disabling depth entirely.
    depth_masked_material_ = filament::Material::Builder()
        .package(k_heaven_depth_masked_filamat, k_heaven_depth_masked_filamat_size)
        .build(*engine);
    if (!depth_masked_material_) {
        std::printf("  DepthPrePass: heaven_depth_masked material failed to load — "
                    "foliage will write solid silhouettes into scene depth\n");
    }
}

void DepthPrePass::ensure(int w, int h) {
    auto* engine = core_.engine;

    build_once();

    // A prior attempt to build the colour/depth target failed and will not
    // recover (it is a GPU-capability issue, not a transient one). Stay
    // disabled so we do not re-attempt or re-log every frame; depth_texture()
    // keeps returning null and consumers fall back to no occlusion.
    if (target_failed_) return;

    int tw = w / render_tuning::depth.downsample;
    int th = h / render_tuning::depth.downsample;

    if (tw_ == tw && th_ == th && rt_) return;

    // Destroy old size-dependent resources
    if (view_) view_->setRenderTarget(nullptr);
    if (rt_) { engine->destroy(rt_); rt_ = nullptr; }
    if (linear_depth_tex_) { engine->destroy(linear_depth_tex_); linear_depth_tex_ = nullptr; }
    if (depth_tex_) { engine->destroy(depth_tex_); depth_tex_ = nullptr; }

    tw_ = tw;
    th_ = th;

    // F COLOR attachment — the Scene_Depth_Texture. SAMPLEABLE so consumers
    // can read it in their fragment shader; full precision across the
    // 0.5..2000 m camera range.
    // Filament TextureUsage explicitly permits combinations of these bit flags.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto color_usage = static_cast<filament::Texture::Usage>(
        std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
        std::to_underlying(filament::Texture::Usage::SAMPLEABLE));
    linear_depth_tex_ = filament::Texture::Builder()
        .width(tw).height(th).levels(1)
        .usage(color_usage)
        .format(filament::Texture::InternalFormat::R32F)
        .build(*engine);

    // DEPTH32F attachment — write-only depth test so the colour target keeps
    // the NEAREST surface per pixel. Never sampled, so no SAMPLEABLE usage.
    depth_tex_ = filament::Texture::Builder()
        .width(tw).height(th).levels(1)
        .usage(filament::Texture::Usage::DEPTH_ATTACHMENT)
        .format(filament::Texture::InternalFormat::DEPTH32F)
        .build(*engine);

    // If either attachment failed to allocate we cannot produce a valid
    // scene-depth target. Log once, tear down the partial resources, and leave
    // depth_texture() returning null: the renderer keeps running with depth
    // occlusion disabled (consumers treat the absent texture as "far / no
    // geometry") and never dereferences a half-built target.
    if (!linear_depth_tex_ || !depth_tex_) {
        std::printf("  DepthPrePass: failed to build the R32F scene-depth target "
                    "(%dx%d) — scene-depth occlusion disabled for consumers\n",
                    tw, th);
        if (linear_depth_tex_) { engine->destroy(linear_depth_tex_); linear_depth_tex_ = nullptr; }
        if (depth_tex_)        { engine->destroy(depth_tex_); depth_tex_ = nullptr; }
        if (view_)             view_->setRenderTarget(nullptr);
        target_failed_ = true;
        tw_ = th_ = 0;
        return;
    }

    rt_ = filament::RenderTarget::Builder()
        .texture(filament::RenderTarget::AttachmentPoint::COLOR0, linear_depth_tex_)
        .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_tex_)
        .build(*engine);

    // Same graceful handling if the render target itself fails to build.
    if (!rt_) {
        std::printf("  DepthPrePass: failed to build the scene-depth render "
                    "target — scene-depth occlusion disabled for consumers\n");
        engine->destroy(linear_depth_tex_); linear_depth_tex_ = nullptr;
        engine->destroy(depth_tex_);        depth_tex_ = nullptr;
        if (view_) view_->setRenderTarget(nullptr);
        target_failed_ = true;
        tw_ = th_ = 0;
        return;
    }

    // Create a separate View for the depth pre-pass (never touches the main
    // view). It renders the dedicated proxy scene, not the main scene.
    if (!view_) {
        cam_entity_ = utils::EntityManager::get().create();
        camera_ = engine->createCamera(cam_entity_);
        view_ = engine->createView();
        view_->setScene(depth_scene_);
        view_->setCamera(camera_);
        view_->setPostProcessingEnabled(false);
        view_->setShadowingEnabled(false);
    }

    view_->setRenderTarget(rt_);
    view_->setViewport({0, 0, static_cast<uint32_t>(tw), static_cast<uint32_t>(th)});
}

void DepthPrePass::aim(const Camera& camera, int viewport_w, int viewport_h) {
    float aspect = safe_aspect(viewport_w, viewport_h);
    camera_->setProjection(camera.fov, aspect,
                           render_tuning::depth.near_plane,
                           render_tuning::depth.far_plane,
                           filament::Camera::Fov::VERTICAL);
    glm::mat4 view_mat = camera.view_matrix();
    camera_->setModelMatrix(glm_to_filament(glm::inverse(view_mat)));
}

} // namespace heaven
