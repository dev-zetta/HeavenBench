/*
 * HeavenBench — HDR10 output encode (PQ / ST.2084)
 *
 * Implementation mirrors SceneColorCapture's blit pipeline (the proven
 * fullscreen-quad composite shape) with a single fp16 capture target and the
 * heaven_hdr_encode material doing the PQ encode. See the header.
 */
#include "rhi/hdr_output.h"
#include "rhi/filament_bounds.h"
#include "rhi/materials_data.h"  // k_heaven_hdr_encode_filamat
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
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

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace heaven {

HdrOutput::~HdrOutput() {
    if (!core_.engine) return;
    auto* engine = core_.engine;

    destroy_target();

    if (encode_entity_) {
        if (composite_scene_) composite_scene_->remove(encode_entity_);
        engine->destroy(encode_entity_);
        utils::EntityManager::get().destroy(encode_entity_);
        encode_entity_ = {};
    }
    if (encode_vb_)       { engine->destroy(encode_vb_); encode_vb_ = nullptr; }
    if (encode_ib_)       { engine->destroy(encode_ib_); encode_ib_ = nullptr; }
    if (encode_mi_)       { engine->destroy(encode_mi_); encode_mi_ = nullptr; }
    if (encode_material_) { engine->destroy(encode_material_); encode_material_ = nullptr; }
    if (composite_view_)  { engine->destroy(composite_view_); composite_view_ = nullptr; }
    if (composite_scene_) { engine->destroy(composite_scene_); composite_scene_ = nullptr; }
    if (sdr_background_) { engine->destroy(sdr_background_); sdr_background_ = nullptr; }
}

void HdrOutput::destroy_target() {
    auto* engine = core_.engine;
    if (rt_)        { engine->destroy(rt_); rt_ = nullptr; }
    if (color_tex_) { engine->destroy(color_tex_); color_tex_ = nullptr; }
    if (depth_tex_) { engine->destroy(depth_tex_); depth_tex_ = nullptr; }
    if (sdr_layer_rt_) { engine->destroy(sdr_layer_rt_); sdr_layer_rt_ = nullptr; }
    if (sdr_layer_tex_) { engine->destroy(sdr_layer_tex_); sdr_layer_tex_ = nullptr; }
    if (sdr_layer_depth_) { engine->destroy(sdr_layer_depth_); sdr_layer_depth_ = nullptr; }
    bound_src_ = nullptr;
}

void HdrOutput::set_luminance(float reference_nits, float peak_nits) {
    reference_nits_ = reference_nits;
    peak_nits_      = peak_nits;
    params_dirty_   = true;
}

void HdrOutput::build_pipeline() {
    auto* engine = core_.engine;
    if (encode_material_) return;   // built once

    encode_material_ = filament::Material::Builder()
        .package(k_heaven_hdr_encode_filamat, k_heaven_hdr_encode_filamat_size)
        .build(*engine);
    if (!encode_material_) {
        std::fprintf(stderr, "HDR output: encode material failed to load\n");
        return;
    }
    encode_mi_ = encode_material_->createInstance();

    // Fullscreen quad ([-1,1] clip, UV [0,1]) — same shape as heaven_blit.
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

    encode_vb_ = filament::VertexBuffer::Builder()
        .vertexCount(4)
        .bufferCount(1)
        .attribute(filament::VertexAttribute::POSITION, 0,
                   filament::VertexBuffer::AttributeType::FLOAT3, 0, sizeof(QuadVertex))
        .attribute(filament::VertexAttribute::UV0, 0,
                   filament::VertexBuffer::AttributeType::FLOAT2,
                   sizeof(filament::math::float3), sizeof(QuadVertex))
        .build(*engine);
    encode_vb_->setBufferAt(*engine, 0,
        filament::VertexBuffer::BufferDescriptor(
            quad_verts.data(), sizeof(quad_verts)));

    encode_ib_ = filament::IndexBuffer::Builder()
        .indexCount(6)
        .bufferType(filament::IndexBuffer::IndexType::USHORT)
        .build(*engine);
    encode_ib_->setBuffer(*engine,
        filament::IndexBuffer::BufferDescriptor(
            quad_indices.data(), sizeof(quad_indices)));

    encode_entity_ = utils::EntityManager::get().create();
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  encode_vb_, encode_ib_)
        .material(0, encode_mi_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(false)
        .priority(0)
        .boundingBox(box_from_min_max(
            {-render_tuning::hdr.fullscreen_bounds_extent,
             -render_tuning::hdr.fullscreen_bounds_extent,
             -render_tuning::hdr.fullscreen_bounds_extent},
            {render_tuning::hdr.fullscreen_bounds_extent,
             render_tuning::hdr.fullscreen_bounds_extent,
             render_tuning::hdr.fullscreen_bounds_extent}))
        .build(*engine, encode_entity_);

    composite_scene_ = engine->createScene();
    composite_scene_->addEntity(encode_entity_);

    composite_view_ = engine->createView();
    composite_view_->setScene(composite_scene_);
    composite_view_->setCamera(core_.camera);
    composite_view_->setPostProcessingEnabled(false);
    composite_view_->setShadowingEnabled(false);
    composite_view_->setBlendMode(filament::View::BlendMode::OPAQUE);
    // Render target stays nullptr → the encode quad writes the swap chain.

    // Custom render targets cannot rely on the swapchain's clear options.
    // Draw transparent black behind each SDR layer so removed UI widgets do
    // not leave stale pixels in the next frame. Borrowed by the UI/splash scenes.
    sdr_background_ = filament::Skybox::Builder()
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .build(*engine);
}

bool HdrOutput::ensure(int w, int h) {
    auto* engine = core_.engine;
    if (!engine || w <= 0 || h <= 0) return false;

    build_pipeline();
    if (!encode_mi_) return false;   // no encode material → HDR unavailable

    if (params_dirty_) {
        encode_mi_->setParameter("referenceNits", reference_nits_);
        encode_mi_->setParameter("peakNits", peak_nits_);
        params_dirty_ = false;
    }

    if (!needs_resize(w, h) && valid()) return true;

    destroy_target();
    w_ = w;
    h_ = h;

    // The main view renders here: linear Rec.2020 display-referred HDR, so fp16.
    // Filament TextureUsage explicitly permits combinations of these bit flags.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto color_usage = static_cast<filament::Texture::Usage>(
        std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
        std::to_underlying(filament::Texture::Usage::SAMPLEABLE));
    color_tex_ = filament::Texture::Builder()
        .width(w).height(h).levels(1)
        .usage(color_usage)
        .format(filament::Texture::InternalFormat::RGBA16F)
        .build(*engine);
    depth_tex_ = filament::Texture::Builder()
        .width(w).height(h).levels(1)
        .usage(filament::Texture::Usage::DEPTH_ATTACHMENT)
        .format(filament::Texture::InternalFormat::DEPTH32F)
        .build(*engine);

    rt_ = filament::RenderTarget::Builder()
        .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_tex_)
        .texture(filament::RenderTarget::AttachmentPoint::DEPTH,  depth_tex_)
        .build(*engine);

    sdr_layer_tex_ = filament::Texture::Builder()
        .width(w).height(h).levels(1)
        .usage(color_usage)
        .format(filament::Texture::InternalFormat::RGBA16F)
        .build(*engine);
    sdr_layer_depth_ = filament::Texture::Builder()
        .width(w).height(h).levels(1)
        .usage(filament::Texture::Usage::DEPTH_ATTACHMENT)
        .format(filament::Texture::InternalFormat::DEPTH32F)
        .build(*engine);
    sdr_layer_rt_ = filament::RenderTarget::Builder()
        .texture(filament::RenderTarget::AttachmentPoint::COLOR0, sdr_layer_tex_)
        .texture(filament::RenderTarget::AttachmentPoint::DEPTH, sdr_layer_depth_)
        .build(*engine);
    return valid();
}

void HdrOutput::bind_sdr_layer(filament::View& view) {
    view.setRenderTarget(sdr_layer_rt_);
    view.getScene()->setSkybox(sdr_background_);
    // This controls the view's final blit, not the alpha blending of the UI
    // geometry. Replace every destination pixel, including transparent black.
    view.setBlendMode(filament::View::BlendMode::OPAQUE);
}

void HdrOutput::prepare(int viewport_w, int viewport_h,
                        bool has_scene, bool has_sdr_layer) {
    if (!encode_mi_) return;

    composite_view_->setViewport({0, 0,
        static_cast<uint32_t>(viewport_w),
        static_cast<uint32_t>(viewport_h)});

    // Bind the scene texture we are about to render into as the encode source.
    // Done here (before beginFrame) so the descriptor update is outside any
    // render pass.
    if (bound_src_ != color_tex_) {
        filament::TextureSampler sampler(
            filament::TextureSampler::MinFilter::LINEAR,
            filament::TextureSampler::MagFilter::LINEAR,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        encode_mi_->setParameter("sceneTexture", color_tex_, sampler);
        encode_mi_->setParameter("sdrLayerTexture", sdr_layer_tex_, sampler);
        bound_src_ = color_tex_;
    }
    encode_mi_->setParameter("hasScene", has_scene);
    encode_mi_->setParameter("hasSdrLayer", has_sdr_layer);
}

void HdrOutput::set_output_target(filament::RenderTarget* target) {
    if (composite_view_) composite_view_->setRenderTarget(target);
}

void HdrOutput::encode() {
    if (core_.renderer) encode(*core_.renderer);
}

void HdrOutput::encode(filament::Renderer& renderer) {
    if (composite_view_) renderer.render(composite_view_);
}

} // namespace heaven
