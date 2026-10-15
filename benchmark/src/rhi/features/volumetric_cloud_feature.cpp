/*
 * HeavenBench — Raymarched volumetric clouds feature implementation
 *
 * Modelled on godrays_feature / volfog_feature: a world-space fullscreen
 * quad parked in front of the camera each frame, sampling the shared
 * half-res depth pre-pass (FrameContext::depth_texture) to reconstruct
 * world positions and raymarch a horizontal cloud slab.
 *
 * The cloud math lives entirely in heaven_volumetric_clouds.filamat
 * (embedded via materials_data.h); here we only feed per-frame uniforms.
 * Motion is advanced solely from FrameContext::time.
 */
#include "rhi/features/volumetric_cloud_feature.h"
#include "rhi/frame_generation/temporal_effects.h"
#include "rhi/render_test_scene_assets.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"
#include "rhi/cloud_lighting.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/TextureSampler.h>

#include <utils/EntityManager.h>

#include <math/vec3.h>
#include <math/vec4.h>
#include <math/mat4.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>

namespace heaven {

namespace {

float halton2(uint32_t index) {
    float result = 0.0f;
    float scale = 0.5f;
    while (index > 0) {
        result += scale * static_cast<float>(index & 1u);
        index >>= 1u;
        scale *= 0.5f;
    }
    return result;
}

} // namespace

void VolumetricCloudFeature::init(RenderContext& ctx, const AppConfig& config) {
    auto* engine_ = ctx.engine;

    // Load the raymarched volumetric clouds material (embedded it).
    material_ = filament::Material::Builder()
        .package(k_heaven_volumetric_clouds_filamat,
                 k_heaven_volumetric_clouds_filamat_size)
        .build(*engine_);
    if (!material_) {
        std::printf("  Volumetric clouds: material failed to load\n");
        return;
    }

    // Resolve the raymarch step budget from the cloud-quality knob — the
    // single source of truth shared with the CLI / property tests
    // (cloud_raymarch_steps clamps min ≥ 1).
    num_steps_ = cloud_raymarch_steps(config.quality.cloud_quality, config.quality);

    mi_ = material_->createInstance();
    material_tuning::bind_volumetric_clouds(*mi_);

    // Depth texture — zero-reading depth placeholder until the actual depth
    // pre-pass texture is bound in update() once it exists. The shader treats a
    // non-positive read as "far / no geometry", so the clouds render unoccluded
    // (not fully occluded) when the real depth texture is absent.
    filament::TextureSampler depth_sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    mi_->setParameter("depthTexture", ctx.resources->placeholder_depth_texture(), depth_sampler);

    // Camera world position (view-ray origin; set per-frame in update()).
    mi_->setParameter("cameraPos", filament::math::float3{0.0f});

    // Seed the same illumination calculation used by the per-frame update.
    const auto initial = cloud_lighting(lighting_for_time_of_day(config.time_of_day));
    mi_->setParameter("sunDir", filament::math::float3{
        initial.direction.x, initial.direction.y, initial.direction.z});
    mi_->setParameter("sunColor", filament::math::float3{
        initial.direct.x, initial.direct.y, initial.direct.z});
    mi_->setParameter("skyColor", filament::math::float3{
        initial.ambient.x, initial.ambient.y, initial.ambient.z});

    // Cloud slab geometry (world Z, the up axis) — these are placeholders;
    // update() re-derives cloudBottom/cloudTop every frame. The visible
    // volumetric deck is a cloud sea just below the islands, not the distant
    // ObjectSky dome position.
    mi_->setParameter(
        "cloudBottom",
        render_tuning::volumetric_clouds.deck_center_z -
            render_tuning::volumetric_clouds.deck_thickness * 0.5f);
    mi_->setParameter(
        "cloudTop",
        render_tuning::volumetric_clouds.deck_center_z +
            render_tuning::volumetric_clouds.deck_thickness * 0.5f);
    // Broken stratocumulus deck. densityScale is extinction per world metre;
    // cloud cores remain opaque while their low-density edges integrate
    // smoothly.
    mi_->setParameter("coverage", render_tuning::volumetric_clouds.coverage);
    mi_->setParameter(
        "densityScale", render_tuning::volumetric_clouds.density_scale);
    mi_->setParameter(
        "noiseScale", render_tuning::volumetric_clouds.noise_scale);
    mi_->setParameter(
        "maxMarchDistance",
        render_tuning::volumetric_clouds.max_march_distance);
    if (ctx.test_scene_assets) {
        const auto& inputs = ctx.test_scene_assets->atmosphere();
        mi_->setParameter("cloudBottom", inputs.cloud_center_z - inputs.cloud_thickness * 0.5f);
        mi_->setParameter("cloudTop", inputs.cloud_center_z + inputs.cloud_thickness * 0.5f);
        mi_->setParameter("noiseScale", inputs.cloud_noise_scale);
        mi_->setParameter("densityScale", inputs.cloud_extinction);
        mi_->setParameter("maxMarchDistance", inputs.cloud_march_distance);
    }

    mi_->setParameter("numSteps", num_steps_);
    mi_->setParameter("time", 0.0f);
    mi_->setParameter("temporalJitter", 0.5f);

    // Fullscreen quad geometry (same shape as god-rays / volfog quads).
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

    // Renderable entity. Priority 6 (same band as clouds / volfog): drawn
    // before god rays so the sun shafts composite on top of the cloud.
    entity_ = utils::EntityManager::get().create();
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  vb_, ib_)
        .material(0, mi_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(false)
        .priority(6)
        .boundingBox(uncullable_box())
        .build(*engine_, entity_);

    // Explicit transform component — Renderable doesn't add one
    // automatically, and update() needs one to position the quad.
    {
        auto& tcm = engine_->getTransformManager();
        tcm.create(entity_);
    }

    // Disabled by default so a flagless run is Shipping_Look-equivalent
    // the dormant legacy CloudFeature is also off by default,
    // so the two never composite.
    enabled_ = config.features.volumetric_clouds;
    if (enabled_) {
        ctx.scene->addEntity(entity_);
        in_scene_ = true;
    }

    std::printf("  Volumetric clouds: raymarched layer initialized (%d steps)%s\n",
                num_steps_, enabled_ ? "" : " (disabled)");
}

void VolumetricCloudFeature::on_enabled_changed(RenderContext& ctx, bool on) {
    if (!entity_) return;
    if (on && !in_scene_) {
        ctx.scene->addEntity(entity_);
        in_scene_ = true;
    } else if (!on && in_scene_) {
        ctx.scene->remove(entity_);
        in_scene_ = false;
    }
}

void VolumetricCloudFeature::on_depth_prepass_begin(RenderContext& ctx) {
    // Pull the quad out of the scene so it doesn't pollute the depth
    // pre-pass (its quad sits in front of the camera and would write depth).
    if (entity_ && in_scene_) ctx.scene->remove(entity_);
}

void VolumetricCloudFeature::on_depth_prepass_end(RenderContext& ctx) {
    if (entity_ && in_scene_) ctx.scene->addEntity(entity_);
}

void VolumetricCloudFeature::release_shared_targets(RenderContext& ctx) {
    const filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    if (mi_) mi_->setParameter("depthTexture",
        ctx.resources->placeholder_depth_texture(), sampler);
    bound_depth_ = nullptr;
}

void VolumetricCloudFeature::update(RenderContext& ctx, const FrameContext& frame) {
    if (!mi_) return;
    auto* engine_ = ctx.engine;
    const Camera& camera = *frame.camera;
    const LightingParams& lighting = *frame.lighting;
    int viewport_w = frame.viewport_w;
    int viewport_h = frame.viewport_h;

    // Bind the shared depth pre-pass texture once it exists / on resize.
    // Done outside any render pass (update() runs before the render passes)
    // so descriptor-set updates don't happen mid-render-pass.
    if (frame.depth_texture && frame.depth_texture != bound_depth_) {
        filament::TextureSampler depth_sampler(
            filament::TextureSampler::MinFilter::NEAREST,
            filament::TextureSampler::MagFilter::NEAREST,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        mi_->setParameter("depthTexture", frame.depth_texture, depth_sampler);
        bound_depth_ = frame.depth_texture;
    }

    // Camera view ray origin. The shader takes the view-ray direction from
    // the quad's own world position (getWorldPosition) and reads the linear
    // scene-depth texture for occlusion, so no inverse-view-projection
    // reconstruction matrix is needed any more (— same as volfog).
    float aspect = safe_aspect(viewport_w, viewport_h);
    glm::mat4 view_mat = camera.view_matrix();

    mi_->setParameter("cameraPos",
        filament::math::float3{camera.position.x, camera.position.y, camera.position.z});

    // Anchor the volumetric cloud sea far enough below the floating islands
    // for whole masses to fit in view. The attempted -155 top put the camera
    // almost inside the layer, making it fill the lower hemisphere like fog;
    // the original -500 top hid it in distant aerial haze. This midpoint keeps
    // visible parallax while leaving clear air below the playable island band.
    const float cloud_center = ctx.test_scene_assets
        ? ctx.test_scene_assets->atmosphere().cloud_center_z
        : render_tuning::volumetric_clouds.deck_center_z;
    const float thickness = ctx.test_scene_assets
        ? ctx.test_scene_assets->atmosphere().cloud_thickness
        : render_tuning::volumetric_clouds.deck_thickness;
    const float slab_bottom = cloud_center - 0.5f * thickness;
    const float slab_top = cloud_center + 0.5f * thickness;
    mi_->setParameter("cloudBottom", slab_bottom);
    mi_->setParameter("cloudTop",    slab_top);

    const auto illumination = cloud_lighting(lighting);
    mi_->setParameter("sunDir",
        filament::math::float3{illumination.direction.x, illumination.direction.y, illumination.direction.z});
    mi_->setParameter("sunColor",
        filament::math::float3{illumination.direct.x, illumination.direct.y, illumination.direct.z});
    mi_->setParameter("skyColor",
        filament::math::float3{illumination.ambient.x, illumination.ambient.y, illumination.ambient.z});

    // Advance cloud motion solely from the frame-rate-independent clock so
    // wind drift is identical for equal accumulated time.
    mi_->setParameter("time", frame.time);
    const uint32_t sampling_frame = frame.capture_frame
        ? static_cast<uint32_t>(*frame.capture_frame & 1023u) : frame_index_++;
    mi_->setParameter("temporalJitter", halton2((sampling_frame & 1023u) + 1u));

    // Position the fullscreen quad just inside the far plane (same trick as
    // god rays / volfog). The fragment shader uses the depth buffer for the
    // actual ray length; this just makes sure every screen pixel runs the
    // fragment shader.
    glm::vec3 cam_pos = camera.position;
    glm::vec3 cam_fwd = -glm::vec3(view_mat[0][2], view_mat[1][2], view_mat[2][2]);
    glm::vec3 cam_right = glm::vec3(view_mat[0][0], view_mat[1][0], view_mat[2][0]);
    glm::vec3 cam_up = glm::vec3(view_mat[0][1], view_mat[1][1], view_mat[2][1]);

    float dist = render_tuning::fullscreen.volumetric_cloud_distance;
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

void VolumetricCloudFeature::destroy(RenderContext& ctx) {
    auto* engine_ = ctx.engine;
    if (entity_) {
        if (in_scene_) ctx.scene->remove(entity_);
        engine_->destroy(entity_);
        utils::EntityManager::get().destroy(entity_);
        entity_ = {};
    }
    if (mi_) { engine_->destroy(mi_); mi_ = nullptr; }
    if (material_) { engine_->destroy(material_); material_ = nullptr; }
    if (vb_) { engine_->destroy(vb_); vb_ = nullptr; }
    if (ib_) { engine_->destroy(ib_); ib_ = nullptr; }
}


void VolumetricCloudFeature::append_temporal_draws(std::vector<TemporalEffectDraw>& draws) const {
    if (!enabled_ || !in_scene_ || !entity_ || !mi_) return;
    const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST, filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    TemporalEffectDraw draw{.identity = entity_.getId(), .kind = TemporalEffectKind::VolumetricCloud,
        .source_entity = entity_, .vertices = vb_, .indices = ib_, .source = mi_};
    draw.textures[0] = {"depthTexture", bound_depth_, sampler};
    draws.push_back(draw);
}

} // namespace heaven
