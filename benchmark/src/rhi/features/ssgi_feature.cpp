/*
 * HeavenBench — SSGI feature implementation
 *
 * Modelled on godrays_feature / volumetric_cloud_feature: a world-space
 * fullscreen quad parked in front of the camera each frame, sampling the
 * shared half-res depth pre-pass (FrameContext::depth_texture) to reconstruct
 * VIEW-SPACE positions/normals and gather one-bounce indirect light.
 *
 * The reconstruction + gather math lives entirely in heaven_ssgi.filamat
 * (embedded via materials_data.h); here we only feed per-frame
 * uniforms. The depth source is now the LINEAR scene-depth texture (F,
 * eye-space forward distance in metres) produced by the depth pre-pass /
 * heaven_depth proxy render (scene-depth-pipeline), so the reconstruction is a
 * plain view-ray un-projection scaled by the sampled distance. The one camera
 * parameter it needs is `projScale` = (tan(fovY/2)*aspect, tan(fovY/2)) — the
 * per-axis view-ray slope, built from the same camera FOV the depth pre-pass and
 * the CPU mirror (engine/ssgi_reconstruct_ref.h) agree on. No reversed-Z /
 * inverse-projection matrix is needed any more.
 */
#include "rhi/features/ssgi_feature.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "render_tuning.h"

#include "engine/ssgi_reconstruct_ref.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/TextureSampler.h>

#include <backend/PixelBufferDescriptor.h>

#include <utils/EntityManager.h>

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>
#include <math/mat4.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace heaven {

void SsgiFeature::init(RenderContext& ctx, const AppConfig& config) {
    auto* engine_ = ctx.engine;

    // Load the SSGI material (embedded it).
    material_ = filament::Material::Builder()
        .package(k_heaven_ssgi_filamat, k_heaven_ssgi_filamat_size)
        .build(*engine_);
    if (!material_) {
        std::printf("  SSGI: material failed to load\n");
        return;
    }

    // Resolve the gather sample/ray count from the ssgi-quality knob — the
    // single source of truth shared with the CLI / property tests
    // (ssgi_samples, bounded by ssgi_sample_max).
    num_samples_ = ssgi_samples(config.quality.ssgi_quality, config.quality);

    mi_ = material_->createInstance();
    material_tuning::bind_ssgi(*mi_);

    // Depth texture — placeholder until the actual depth pre-pass texture is
    // bound in update() once it exists. Linear F (eye-forward metres),
    // non-filterable.
    filament::TextureSampler depth_sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    mi_->setParameter("depthTexture", ctx.resources->placeholder_depth_texture(), depth_sampler);

    // Scene-colour copy to gather one-bounce light from. The real scene-colour
    // capture is owned by the particle/heat-haze feature's scene-capture seam
    // (only one feature can drive it per frame) and is not wired into SSGI yet.
    // Until it is, bind a feature-owned 1×1 BLACK texture: because SSGI
    // composites ADDITIVELY (heaven_ssgi.mat: blending:add), black is the
    // no-data identity — the gather averages it and adds zero, so an
    // enabled-but-unwired SSGI is artifact-free. The previous code bound the
    // shared 1×1 WHITE placeholder here on the (incorrect) assumption that it
    // "adds no net colour"; for an additive pass white is the opposite — it
    // injects full-intensity light wherever the gather weight is non-zero, i.e.
    // at every depth discontinuity where the depth-reconstructed normal is
    // degenerate (stair edges, foliage silhouettes), which showed up as bright
    // white streaks that swam with the camera.
    color_stub_ = filament::Texture::Builder()
        .width(1).height(1).levels(1)
        .format(filament::Texture::InternalFormat::RGBA8)
        .build(*engine_);
    {
        static uint8_t black[] = { 0, 0, 0, 255 };
        color_stub_->setImage(*engine_, 0,
            filament::Texture::PixelBufferDescriptor(
                black, 4,
                filament::Texture::Format::RGBA,
                filament::Texture::Type::UBYTE));
    }
    filament::TextureSampler color_sampler(
        filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    mi_->setParameter("colorTexture", color_stub_, color_sampler);

    // Per-axis view-ray slope (tan(fovY/2)*aspect, tan(fovY/2)) for the linear
    // view-space reconstruction. Set per frame in update() once the aspect
    // ratio is known.
    mi_->setParameter("projScale", filament::math::float2{1.0f, 1.0f});

    // Previous-frame view-projection for reprojection — set per frame in
    // update(); identity until then.
    mi_->setParameter("prevViewProj", filament::math::mat4f{});

    // One texel of the (half-res) depth/colour buffer in UV units — set per
    // frame once the depth target size is known.
    mi_->setParameter("texelSize", filament::math::float2{
        render_tuning::ssgi.fallback_texel_size.x,
        render_tuning::ssgi.fallback_texel_size.y});

    // Bounded screen-space gather radius in UV units: keeps the
    // colour-bleed contribution local and the per-pixel cost bounded.
    mi_->setParameter("sampleRadius", render_tuning::ssgi.sample_radius);

    // Gather sample/ray count from the quality knob.
    mi_->setParameter("numSamples", num_samples_);

    // Master indirect-light scale. SSGI gathers the PREVIOUS frame's lit colour
    // (a roughly LDR average of nearby surfaces) and adds it; a full-strength
    // add would wash the scene out, so keep the one-bounce bleed subtle. The
    // effective value is scaled per frame in update().
    mi_->setParameter("intensity", base_intensity_);

    // Fullscreen quad geometry (same shape as god-rays / clouds quads).
    struct QuadVertex {
        filament::math::float3 position;
        filament::math::float2 uv;
    };
    static QuadVertex quad_verts[] = {
        {{-1, -1, 0}, {0, 0}},
        {{ 1, -1, 0}, {1, 0}},
        {{ 1,  1, 0}, {1, 1}},
        {{-1,  1, 0}, {0, 1}},
    };
    static uint16_t quad_indices[] = { 0, 1, 2, 0, 2, 3 };

    vb_ = filament::VertexBuffer::Builder()
        .vertexCount(4)
        .bufferCount(2)
        .attribute(filament::VertexAttribute::POSITION, 0,
                   filament::VertexBuffer::AttributeType::FLOAT3,
                   0, sizeof(QuadVertex))
        .attribute(filament::VertexAttribute::UV0, 0,
                   filament::VertexBuffer::AttributeType::FLOAT2,
                   sizeof(filament::math::float3), sizeof(QuadVertex))
        .attribute(filament::VertexAttribute::TANGENTS, 1,
                   filament::VertexBuffer::AttributeType::SHORT4, 0, 0)
        .normalized(filament::VertexAttribute::TANGENTS)
        .build(*engine_);
    vb_->setBufferAt(*engine_, 0,
        filament::VertexBuffer::BufferDescriptor(quad_verts, sizeof(quad_verts)));

    static filament::math::short4 dummy_tangents[] = {
        {0, 0, 16383, 16383}, {0, 0, 16383, 16383},
        {0, 0, 16383, 16383}, {0, 0, 16383, 16383}
    };
    vb_->setBufferAt(*engine_, 1,
        filament::VertexBuffer::BufferDescriptor(dummy_tangents, sizeof(dummy_tangents)));

    ib_ = filament::IndexBuffer::Builder()
        .indexCount(6)
        .bufferType(filament::IndexBuffer::IndexType::USHORT)
        .build(*engine_);
    ib_->setBuffer(*engine_,
        filament::IndexBuffer::BufferDescriptor(quad_indices, sizeof(quad_indices)));

    // Renderable entity. Priority 5 (drawn before clouds/god rays) so the
    // additive indirect-light contribution composites under the sky effects.
    entity_ = utils::EntityManager::get().create();
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  vb_, ib_)
        .material(0, mi_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(false)
        .priority(5)
        .boundingBox(uncullable_box())
        .build(*engine_, entity_);

    // Explicit transform component — Renderable doesn't add one automatically,
    // and update() needs one to position the quad.
    {
        auto& tcm = engine_->getTransformManager();
        tcm.create(entity_);
    }

    // Feature-owned scene + view for the SSGI quad. It is rendered as a
    // SEPARATE additive composite over the already-blitted clean scene (see
    // render_scene_color_composite), so it is never part of the captured
    // history — this is what breaks the self-feedback white-out. TRANSLUCENT
    // blend mode makes the view composite over the existing target content
    // (the blitted scene) rather than clearing it; the material's blending:add
    // then adds the bounce.
    ssgi_scene_ = engine_->createScene();
    ssgi_scene_->addEntity(entity_);

    ssgi_view_ = engine_->createView();
    ssgi_view_->setScene(ssgi_scene_);
    ssgi_view_->setCamera(ctx.camera);
    ssgi_view_->setPostProcessingEnabled(false);
    ssgi_view_->setShadowingEnabled(false);
    ssgi_view_->setBlendMode(filament::View::BlendMode::TRANSLUCENT);

    // Disabled by default so a flagless run is Shipping_Look-equivalent
    // enabled_ gates whether the backend renders the composite.
    enabled_ = config.features.ssgi;

    std::printf("  SSGI: screen-space global illumination initialized (%d samples)%s\n",
                num_samples_, enabled_ ? "" : " (disabled)");
}

void SsgiFeature::on_enabled_changed(RenderContext& /*ctx*/, bool /*on*/) {
    // The SSGI quad lives in its own view (ssgi_view_), not the main scene, so
    // there is nothing to add/remove here — the backend simply skips the
    // composite render while the feature is disabled (enabled() is checked at
    // the call site).
}

void SsgiFeature::render_scene_color_composite(RenderContext& ctx,
                                               filament::RenderTarget* target) {
    if (!mi_ || !ssgi_view_) return;
    ssgi_view_->setRenderTarget(target);
    ctx.renderer->render(ssgi_view_);
}

void SsgiFeature::on_depth_prepass_begin(RenderContext& /*ctx*/) {
    // No-op: the SSGI quad lives in ssgi_scene_, not the main scene, and the
    // depth pre-pass renders the proxy scene — so the quad never pollutes the
    // depth render and there is nothing to pull out.
}

void SsgiFeature::on_depth_prepass_end(RenderContext& /*ctx*/) {
    // No-op (see on_depth_prepass_begin).
}

void SsgiFeature::release_shared_targets(RenderContext& ctx) {
    if (ssgi_view_) ssgi_view_->setRenderTarget(nullptr);
    const filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    if (mi_) mi_->setParameter("depthTexture",
        ctx.resources->placeholder_depth_texture(), sampler);
    bound_depth_ = nullptr;
    if (mi_ && color_stub_) {
        const filament::TextureSampler color_sampler(
            filament::TextureSampler::MinFilter::LINEAR,
            filament::TextureSampler::MagFilter::LINEAR,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        mi_->setParameter("colorTexture", color_stub_, color_sampler);
    }
    bound_color_ = nullptr;
}

void SsgiFeature::update(RenderContext& ctx, const FrameContext& frame) {
    if (!mi_) return;
    auto* engine_ = ctx.engine;
    const Camera& camera = *frame.camera;
    int viewport_w = frame.viewport_w;
    int viewport_h = frame.viewport_h;

    // Keep the feature-owned composite view sized to the viewport.
    if (ssgi_view_) {
        ssgi_view_->setViewport({0, 0,
            static_cast<uint32_t>(viewport_w),
            static_cast<uint32_t>(viewport_h)});
    }

    // Bind the shared depth pre-pass texture once it exists / on resize. Done
    // outside any render pass (update() runs before the render passes) so
    // descriptor-set updates don't happen mid-render-pass.
    if (frame.depth_texture && frame.depth_texture != bound_depth_) {
        filament::TextureSampler depth_sampler(
            filament::TextureSampler::MinFilter::NEAREST,
            filament::TextureSampler::MagFilter::NEAREST,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        mi_->setParameter("depthTexture", frame.depth_texture, depth_sampler);
        bound_depth_ = frame.depth_texture;

        // One texel of the half-res depth buffer in UV units, for the
        // neighbour taps in the normal reconstruction.
        if (frame.depth_w > 0 && frame.depth_h > 0) {
            mi_->setParameter("texelSize", filament::math::float2{
                1.0f / static_cast<float>(frame.depth_w),
                1.0f / static_cast<float>(frame.depth_h)});
        }
    }

    // Bind the scene-colour gather source: the PREVIOUS frame's lit colour,
    // captured by the backend's SceneColorCapture (FrameContext::scene_color_
    // texture). The backend copies the clean main-view capture into a stable
    // history texture after each frame; rebind on change/resize, outside any
    // render pass (update() runs before the render passes). When no history
    // exists yet (first frame, or scene
    // colour not provided) fall back to the black stub so the additive gather
    // contributes nothing rather than injecting white.
    filament::Texture* want_color = frame.scene_color_texture
                                   ? frame.scene_color_texture : color_stub_;
    if (want_color != bound_color_) {
        filament::TextureSampler color_sampler(
            filament::TextureSampler::MinFilter::LINEAR,
            filament::TextureSampler::MagFilter::LINEAR,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        mi_->setParameter("colorTexture", want_color, color_sampler);
        bound_color_ = want_color;
    }

    // Per-axis view-ray slope projScale = (tan(fovY/2)*aspect, tan(fovY/2)) for
    // the linear view-space reconstruction. MUST match the depth pre-pass FOV
    // and the CPU mirror (engine/ssgi_reconstruct_ref.h::
    // view_ray_scale), so the sampled LINEAR eye-forward distance un-projects to
    // a correct view-space position (NO reversed-Z / inverse-projection matrix).
    float aspect = safe_aspect(viewport_w, viewport_h);
    glm::vec2 proj_scale = ssgi_ref::view_ray_scale(
        camera.fov, aspect);
    mi_->setParameter("projScale",
                      filament::math::float2{proj_scale.x, proj_scale.y});

    // Reprojection matrix: retain the PREVIOUS frame's complete projection
    // and view, including its FOV/aspect if either changes. The shader projects
    // each gather sample's current
    // world position through this to read the history colour that belongs to
    // that geometry, so the bounce stays aligned to surfaces under motion. On
    // the first frame (no history) fall back to the current view (no motion).
    glm::mat4 proj = glm::perspective(
        glm::radians(camera.fov),
        aspect, 0.5f, render_tuning::fullscreen.scene_far_depth);
    glm::mat4 cur_view = camera.view_matrix();
    const glm::mat4 prev_vp = frame.scene_color_view_proj.value_or(proj * cur_view);
    mi_->setParameter("prevViewProj", glm_to_filament(prev_vp));

    // Position the fullscreen quad just inside the far plane (same trick as god
    // rays / clouds). The fragment shader uses the depth buffer for the actual
    // reconstruction; this just guarantees every screen pixel runs the shader.
    glm::mat4 view_mat = camera.view_matrix();
    glm::vec3 cam_pos = camera.position;
    glm::vec3 cam_fwd = -glm::vec3(view_mat[0][2], view_mat[1][2], view_mat[2][2]);
    glm::vec3 cam_right = glm::vec3(view_mat[0][0], view_mat[1][0], view_mat[2][0]);
    glm::vec3 cam_up = glm::vec3(view_mat[0][1], view_mat[1][1], view_mat[2][1]);

    float dist = render_tuning::fullscreen.ssgi_distance;
    float half_h = dist * std::tan(glm::radians(
        camera.fov * 0.5f));
    float half_w = half_h * aspect;
    glm::vec3 center = cam_pos + cam_fwd * dist;

    glm::mat4 quad_xform(1.0f);
    quad_xform[0] = glm::vec4(cam_right * half_w, 0.0f);
    quad_xform[1] = glm::vec4(cam_up * half_h, 0.0f);
    quad_xform[2] = glm::vec4(cam_fwd, 0.0f);
    quad_xform[3] = glm::vec4(center, 1.0f);

    auto& tcm = engine_->getTransformManager();
    auto ti = tcm.getInstance(entity_);
    if (ti) {
        tcm.setTransform(ti, glm_to_filament(quad_xform));
    }
}

void SsgiFeature::destroy(RenderContext& ctx) {
    auto* engine_ = ctx.engine;
    if (entity_) {
        if (ssgi_scene_) ssgi_scene_->remove(entity_);
        engine_->destroy(entity_);
        utils::EntityManager::get().destroy(entity_);
        entity_ = {};
    }
    if (ssgi_view_)  { engine_->destroy(ssgi_view_);  ssgi_view_  = nullptr; }
    if (ssgi_scene_) { engine_->destroy(ssgi_scene_); ssgi_scene_ = nullptr; }
    if (mi_) { engine_->destroy(mi_); mi_ = nullptr; }
    if (material_) { engine_->destroy(material_); material_ = nullptr; }
    if (color_stub_) { engine_->destroy(color_stub_); color_stub_ = nullptr; }
    if (vb_) { engine_->destroy(vb_); vb_ = nullptr; }
    if (ib_) { engine_->destroy(ib_); ib_ = nullptr; }
}

} // namespace heaven
