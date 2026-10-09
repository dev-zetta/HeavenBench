/*
 * HeavenBench — God rays feature implementation
 *
 * Moved verbatim out of FilamentBackend::init_godrays / update_godrays.
 * Depth binding now happens in update() via the FrameContext depth
 * texture rather than the backend's ensure_depth_target().
 */
#include "rhi/features/godrays_feature.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/TextureSampler.h>

#include <utils/EntityManager.h>

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>

namespace heaven {

void GodraysFeature::init(RenderContext& ctx, const AppConfig& config) {
    auto* engine_ = ctx.engine;

    // Load the god rays material
    material_ = filament::Material::Builder()
        .package(k_heaven_godrays_filamat, k_heaven_godrays_filamat_size)
        .build(*engine_);
    if (!material_) {
        std::printf("  God rays: material failed to load\n");
        return;
    }

    // Create material instance with default parameters
    mi_ = material_->createInstance();
    material_tuning::bind_godrays(*mi_);
    mi_->setParameter("sunVisible", false);
    mi_->setParameter("sunScreenPos", filament::math::float2{0.5f, 0.5f});
    // Tuned subtle: the radial-blur sun shafts are a small accent
    // in the sky around the sun position, NOT a bright bloom that
    // affects scene geometry. Earlier the contribution was bright
    // enough that even with a tight sunFade radius, the halo bled
    // into nearby buildings and grass when the sun was near (or
    // just off) screen, producing a saturated yellow flare. With
    // these reduced numbers shafts read as faint warm streaks in
    // sky pixels around the sun, which is what they should be.
    const auto scattering = render_tuning::godrays.scattering;
    const auto sun_color = render_tuning::godrays.initial_sun_color;
    mi_->setParameter("density", scattering.x);
    mi_->setParameter("weight", scattering.y);
    mi_->setParameter("decayRate", scattering.z);
    mi_->setParameter("exposure", scattering.w);
    mi_->setParameter(
        "sunColor",
        filament::math::float3{sun_color.x, sun_color.y, sun_color.z});

    // Set depth texture — use placeholder initially; replaced with the
    // actual depth pre-pass texture in update() once it exists.
    filament::TextureSampler depth_sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    mi_->setParameter("depthTexture", ctx.resources->placeholder_depth_texture(), depth_sampler);

    // Create a fullscreen quad for the god rays overlay
    // Two triangles covering [-1,1] in clip space, with UVs [0,1]
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

    // Dummy tangents for the 4 vertices
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

    // Create the god rays entity — positioned at the camera, scaled to fill the view
    entity_ = utils::EntityManager::get().create();

    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  vb_, ib_)
        .material(0, mi_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(false)
        .priority(7)  // render last (lowest priority = rendered last for transparent)
        .boundingBox(uncullable_box())
        .build(*engine_, entity_);

    enabled_ = config.features.god_rays;
    if (enabled_) {
        ctx.scene->addEntity(entity_);
        in_scene_ = true;
    }

    std::printf("  God rays: volumetric light scattering initialized%s\n",
                enabled_ ? "" : " (disabled)");
}

void GodraysFeature::on_enabled_changed(RenderContext& ctx, bool on) {
    if (!entity_) return;
    if (on && !in_scene_) {
        ctx.scene->addEntity(entity_);
        in_scene_ = true;
    } else if (!on && in_scene_) {
        ctx.scene->remove(entity_);
        in_scene_ = false;
    }
}

void GodraysFeature::on_depth_prepass_begin(RenderContext& ctx) {
    // Pull the quad out of the scene so it doesn't pollute the depth
    // pre-pass (its quad sits at the far plane and would write depth).
    if (entity_ && in_scene_) ctx.scene->remove(entity_);
}

void GodraysFeature::on_depth_prepass_end(RenderContext& ctx) {
    if (entity_ && in_scene_) ctx.scene->addEntity(entity_);
}

void GodraysFeature::release_shared_targets(RenderContext& ctx) {
    const filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    if (mi_) mi_->setParameter("depthTexture",
        ctx.resources->placeholder_depth_texture(), sampler);
    bound_depth_ = nullptr;
}

void GodraysFeature::update(RenderContext& ctx, const FrameContext& frame) {
    if (!mi_) return;
    auto* engine_ = ctx.engine;
    const Camera& camera = *frame.camera;
    const LightingParams& lighting = *frame.lighting;
    int viewport_w = frame.viewport_w;
    int viewport_h = frame.viewport_h;

    // Bind the shared depth pre-pass texture once it exists / on resize.
    // Done outside beginFrame (update() runs before the render passes)
    // so descriptor-set updates don't happen mid-render-pass.
    if (frame.depth_texture && frame.depth_texture != bound_depth_) {
        filament::TextureSampler depth_sampler(
            filament::TextureSampler::MinFilter::NEAREST,
            filament::TextureSampler::MagFilter::NEAREST,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        mi_->setParameter("depthTexture", frame.depth_texture, depth_sampler);
        bound_depth_ = frame.depth_texture;
    }

    // Project sun position to screen space.
    glm::mat4 view_mat = camera.view_matrix();
    float aspect = safe_aspect(viewport_w, viewport_h);
    glm::mat4 proj_mat = glm::perspective(glm::radians(camera.fov), aspect, 0.5f, 2000.0f);

    // Sun is a directional light — place it far away along the sun direction
    glm::vec3 sun_world = lighting.sun_dir * 500.0f + camera.position;
    glm::vec4 sun_clip = proj_mat * view_mat * glm::vec4(sun_world, 1.0f);

    bool sun_visible = sun_clip.w > 0.0f;
    glm::vec2 sun_ndc(0.0f);
    if (sun_visible) {
        sun_ndc = glm::vec2(sun_clip) / sun_clip.w;
        // Sun must be on-screen (with a small margin so a sun
        // *just* off the edge can still produce shafts inside the
        // visible frame). Earlier `[-1.5, 1.5]` allowed the sun
        // to project well outside the screen, and the screen-space
        // radial blur happily wrote a saturated bloom halo near
        // whichever screen edge the sun was nearest — exactly the
        // "yellow tint when looking near the sun direction"
        // artefact. With a tight `[-0.15, 1.15]` margin the shader
        // returns zero unless the sun actually contributes.
        sun_visible = sun_ndc.x > -1.15f && sun_ndc.x < 1.15f
                   && sun_ndc.y > -1.15f && sun_ndc.y < 1.15f;
    }
    // Also gate on intensity — see comment in the night branch.
    if (lighting.sun_intensity < 5000.0f) {
        sun_visible = false;
    }

    glm::vec2 sun_screen = sun_ndc * 0.5f + 0.5f;

    mi_->setParameter("sunVisible", sun_visible);
    mi_->setParameter("sunScreenPos",
        filament::math::float2{sun_screen.x, sun_screen.y});
    mi_->setParameter("sunColor",
        filament::math::float3{lighting.sun_color.x, lighting.sun_color.y, lighting.sun_color.z});

    // Position the god rays quad in front of the camera
    glm::vec3 cam_pos = camera.position;
    glm::vec3 cam_fwd = -glm::vec3(view_mat[0][2], view_mat[1][2], view_mat[2][2]);
    glm::vec3 cam_right = glm::vec3(view_mat[0][0], view_mat[1][0], view_mat[2][0]);
    glm::vec3 cam_up = glm::vec3(view_mat[0][1], view_mat[1][1], view_mat[2][1]);

    // Place the quad just inside the far plane so it's always visible
    float dist = render_tuning::fullscreen.godrays_distance;
    float half_h = dist * std::tan(glm::radians(
        camera.fov * 0.5f));
    float half_w = half_h * aspect;

    glm::vec3 center = cam_pos + cam_fwd * dist;
    glm::mat4 quad_transform(1.0f);
    quad_transform[0] = glm::vec4(cam_right * half_w, 0.0f);
    quad_transform[1] = glm::vec4(cam_up * half_h, 0.0f);
    quad_transform[2] = glm::vec4(cam_fwd, 0.0f);
    quad_transform[3] = glm::vec4(center, 1.0f);

    auto& tcm = engine_->getTransformManager();
    auto ti = tcm.getInstance(entity_);
    if (ti) {
        tcm.setTransform(ti, glm_to_filament(quad_transform));
    }
}

void GodraysFeature::destroy(RenderContext& ctx) {
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

} // namespace heaven
