/*
 * HeavenBench — Scene-colour capture (previous-frame history)
 *
 * Stable main-view colour target + previous-frame history copy, mirroring the
 * proven heat-haze fullscreen blit shape without alternating the main view's
 * render target every frame. Engaged only when a feature (SSGI) requests scene
 * colour; see the header.
 */
#include "rhi/scene_color_capture.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"   // glm_to_filament
#include "rhi/materials_data.h"  // k_heaven_blit_filamat

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Camera.h>
#include <filament/Renderer.h>
#include <filament/Viewport.h>
#include <filament/RenderTarget.h>
#include <filament/Texture.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/TextureSampler.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace heaven {

SceneColorCapture::~SceneColorCapture() {
    if (!core_.engine) return;
    auto* engine = core_.engine;

    destroy_targets();

    if (blit_entity_) {
        if (composite_scene_) composite_scene_->remove(blit_entity_);
        engine->destroy(blit_entity_);
        utils::EntityManager::get().destroy(blit_entity_);
        blit_entity_ = {};
    }
    if (blit_vb_)       { engine->destroy(blit_vb_); blit_vb_ = nullptr; }
    if (blit_ib_)       { engine->destroy(blit_ib_); blit_ib_ = nullptr; }
    if (blit_mi_)       { engine->destroy(blit_mi_); blit_mi_ = nullptr; }
    if (blit_material_) { engine->destroy(blit_material_); blit_material_ = nullptr; }
    if (composite_view_)  { engine->destroy(composite_view_); composite_view_ = nullptr; }
    if (composite_scene_) { engine->destroy(composite_scene_); composite_scene_ = nullptr; }
}

void SceneColorCapture::destroy_targets() {
    auto* engine = core_.engine;
    for (auto* target : rt_) {
        if (!target) continue;
        if (core_.view && core_.view->getRenderTarget() == target) {
            core_.view->setRenderTarget(nullptr);
        }
        if (composite_view_ && composite_view_->getRenderTarget() == target) {
            composite_view_->setRenderTarget(nullptr);
        }
    }
    if (rt_[0]) { engine->destroy(rt_[0]); rt_[0] = nullptr; }
    if (rt_[1]) { engine->destroy(rt_[1]); rt_[1] = nullptr; }
    if (color_tex_[0]) { engine->destroy(color_tex_[0]); color_tex_[0] = nullptr; }
    if (color_tex_[1]) { engine->destroy(color_tex_[1]); color_tex_[1] = nullptr; }
    if (depth_tex_) { engine->destroy(depth_tex_); depth_tex_ = nullptr; }
    history_view_proj_.reset();
}

void SceneColorCapture::build_blit_pipeline() {
    auto* engine = core_.engine;
    if (blit_material_) return;   // built once

    blit_material_ = filament::Material::Builder()
        .package(k_heaven_blit_filamat, k_heaven_blit_filamat_size)
        .build(*engine);
    if (!blit_material_) {
        std::printf("  Scene-colour capture: blit material failed to load — "
                    "SSGI scene-colour gather disabled\n");
        return;
    }
    blit_mi_ = blit_material_->createInstance();

    // Fullscreen quad ([-1,1] clip, UV [0,1]) — same shape as the heat-haze
    // composite blit and the post-process feature quads.
    struct QuadVertex {
        filament::math::float3 position;
        filament::math::float2 uv;
    };
    static constexpr std::array<QuadVertex, 4> quad_verts = {{
        {{-1, -1, 0}, {0, 0}},
        {{ 1, -1, 0}, {1, 0}},
        {{ 1,  1, 0}, {1, 1}},
        {{-1,  1, 0}, {0, 1}},
    }};
    static constexpr std::array<std::uint16_t, 6> quad_indices = {
        0, 1, 2, 0, 2, 3};

    blit_vb_ = filament::VertexBuffer::Builder()
        .vertexCount(4)
        .bufferCount(2)
        .attribute(filament::VertexAttribute::POSITION, 0,
                   filament::VertexBuffer::AttributeType::FLOAT3, 0, sizeof(QuadVertex))
        .attribute(filament::VertexAttribute::UV0, 0,
                   filament::VertexBuffer::AttributeType::FLOAT2,
                   sizeof(filament::math::float3), sizeof(QuadVertex))
        .attribute(filament::VertexAttribute::TANGENTS, 1,
                   filament::VertexBuffer::AttributeType::SHORT4, 0, 0)
        .normalized(filament::VertexAttribute::TANGENTS)
        .build(*engine);
    blit_vb_->setBufferAt(*engine, 0,
        filament::VertexBuffer::BufferDescriptor(
            quad_verts.data(), sizeof(quad_verts)));
    static constexpr std::array<filament::math::short4, 4> dummy_tangents = {{
        {0, 0, 16383, 16383}, {0, 0, 16383, 16383},
        {0, 0, 16383, 16383}, {0, 0, 16383, 16383}
    }};
    blit_vb_->setBufferAt(*engine, 1,
        filament::VertexBuffer::BufferDescriptor(
            dummy_tangents.data(), sizeof(dummy_tangents)));

    blit_ib_ = filament::IndexBuffer::Builder()
        .indexCount(6)
        .bufferType(filament::IndexBuffer::IndexType::USHORT)
        .build(*engine);
    blit_ib_->setBuffer(*engine,
        filament::IndexBuffer::BufferDescriptor(
            quad_indices.data(), sizeof(quad_indices)));

    blit_entity_ = utils::EntityManager::get().create();
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  blit_vb_, blit_ib_)
        .material(0, blit_mi_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(false)
        .priority(0)
        .boundingBox(uncullable_box())
        .build(*engine, blit_entity_);
    engine->getTransformManager().create(blit_entity_);

    composite_scene_ = engine->createScene();
    composite_scene_->addEntity(blit_entity_);

    composite_view_ = engine->createView();
    composite_view_->setScene(composite_scene_);
    composite_view_->setCamera(core_.camera);
    composite_view_->setPostProcessingEnabled(false);
    composite_view_->setShadowingEnabled(false);
    composite_view_->setBlendMode(filament::View::BlendMode::OPAQUE);
    // Render target stays nullptr (the swap chain).
}

bool SceneColorCapture::ensure(int w, int h) {
    auto* engine = core_.engine;

    build_blit_pipeline();
    if (!blit_mi_) return false;   // no blit material → caller renders direct

    if (w_ == w && h_ == h && rt_[0]) return true;

    destroy_targets();
    w_ = w;
    h_ = h;
    // Full-resolution HDR colour targets. [0] is the stable main-view capture
    // target; [1] receives a post-render copy and is exposed next frame as the
    // history texture. Keeping the main view on [0] avoids invalidating
    // Filament's temporal post-process history by changing render targets every
    // frame.
    // Filament TextureUsage explicitly permits combinations of these bit flags.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto color_usage = static_cast<filament::Texture::Usage>(
        std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
        std::to_underlying(filament::Texture::Usage::SAMPLEABLE));
    for (auto& color_texture : color_tex_) {
        color_texture = filament::Texture::Builder()
            .width(w).height(h).levels(1)
            .usage(color_usage)
            .format(filament::Texture::InternalFormat::RGBA16F)
            .build(*engine);
    }
    depth_tex_ = filament::Texture::Builder()
        .width(w).height(h).levels(1)
        .usage(filament::Texture::Usage::DEPTH_ATTACHMENT)
        .format(filament::Texture::InternalFormat::DEPTH32F)
        .build(*engine);

    for (int i = 0; i < 2; ++i) {
        rt_[i] = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_tex_[i])
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH,  depth_tex_)
            .build(*engine);
    }
    // All surface instances are committed at beginFrame, including this blit
    // when heat haze claims the frame and aim() is skipped. Replace its old
    // texture binding here, on every target rebuild, before that commit.
    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    blit_mi_->setParameter("sceneTexture", color_tex_[0], sampler);
    return true;
}

filament::Texture* SceneColorCapture::history_texture() const {
    if (!history_view_proj_) return nullptr;
    return color_tex_[1];
}

void SceneColorCapture::aim(const Camera& camera, int viewport_w, int viewport_h) {
    if (!blit_mi_) return;

    composite_view_->setViewport({0, 0,
        static_cast<uint32_t>(viewport_w),
        static_cast<uint32_t>(viewport_h)});

    // Position the blit quad just inside the far plane so it covers the whole
    // viewport regardless of camera direction (the heat-haze composite trick).
    float aspect = safe_aspect(viewport_w, viewport_h);
    glm::mat4 view_mat = camera.view_matrix();
    glm::vec3 cam_pos   = camera.position;
    glm::vec3 cam_fwd   = -glm::vec3(view_mat[0][2], view_mat[1][2], view_mat[2][2]);
    glm::vec3 cam_right =  glm::vec3(view_mat[0][0], view_mat[1][0], view_mat[2][0]);
    glm::vec3 cam_up    =  glm::vec3(view_mat[0][1], view_mat[1][1], view_mat[2][1]);
    float dist   = 1990.0f;
    float half_h = dist * std::tan(glm::radians(camera.fov * 0.5f));
    float half_w = half_h * aspect;
    glm::vec3 center = cam_pos + cam_fwd * dist;
    glm::mat4 q(1.0f);
    q[0] = glm::vec4(cam_right * half_w, 0.0f);
    q[1] = glm::vec4(cam_up    * half_h, 0.0f);
    q[2] = glm::vec4(cam_fwd,            0.0f);
    q[3] = glm::vec4(center, 1.0f);
    auto& tcm = core_.engine->getTransformManager();
    if (auto ti = tcm.getInstance(blit_entity_); ti) {
        tcm.setTransform(ti, glm_to_filament(q));
    }
}

void SceneColorCapture::render_blit() {
    if (composite_view_ && core_.renderer) core_.renderer->render(composite_view_);
}

void SceneColorCapture::render_history_copy() {
    if (!composite_view_ || !core_.renderer || !rt_[1]) return;
    composite_view_->setRenderTarget(rt_[1]);
    core_.renderer->render(composite_view_);
}

void SceneColorCapture::set_output_target(filament::RenderTarget* rt) {
    if (composite_view_) composite_view_->setRenderTarget(rt);
}

void SceneColorCapture::advance(const Camera& camera, int viewport_w, int viewport_h) {
    // Commit camera and pixels together after render_history_copy(). A rejected
    // frame or another feature's capture path reaches neither operation.
    history_view_proj_ = glm::perspective(glm::radians(camera.fov),
        safe_aspect(viewport_w, viewport_h), 0.5f,
        render_tuning::fullscreen.scene_far_depth) * camera.view_matrix();
}

} // namespace heaven
