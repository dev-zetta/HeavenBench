/*
 * HeavenBench — Cloud layer feature implementation
 *
 * Moved verbatim out of FilamentBackend::init_clouds / update_clouds.
 */
#include "rhi/features/cloud_feature.h"
#include "rhi/frame_generation/temporal_effects.h"
#include "rhi/render_test_scene_assets.h"
#include "engine/feature_assets.h"
#include "rhi/filament_bounds.h"
#include "rhi/cloud_lighting.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "engine/path_resolver.h"
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>
#include <filament/TextureSampler.h>

#include <utils/EntityManager.h>

#include <math/vec3.h>
#include <cstdio>

namespace heaven {

void CloudFeature::init(RenderContext& ctx, const AppConfig& config) {
    auto* engine_ = ctx.engine;

    // Load cloud material
    material_ = filament::Material::Builder()
        .package(k_heaven_clouds_filamat, k_heaven_clouds_filamat_size)
        .build(*engine_);
    if (!material_) {
        std::printf("  Clouds: material failed to load\n");
        return;
    }

    // Load cloud textures
    tex1_ = resolve_feature_texture(ctx, FeatureAsset::CloudLayer1, TextureSemantic::ColorSRGB);
    tex2_ = resolve_feature_texture(ctx, FeatureAsset::CloudLayer2, TextureSemantic::ColorSRGB);
    mask_ = resolve_feature_texture(ctx, FeatureAsset::CloudMask, TextureSemantic::ScalarLinear);

    if (!tex1_ || !tex2_) {
        std::printf("  Clouds: textures not found, skipping\n");
        return;
    }

    // Create material instance
    mi_ = material_->createInstance();
    material_tuning::bind_clouds(*mi_);

    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);

    mi_->setParameter("cloudTex1", tex1_, sampler);
    mi_->setParameter("cloudTex2", tex2_, sampler);
    mi_->setParameter("cloudMask", mask_ ? mask_ : tex1_, sampler);
    // The cloud deck height comes from the original Heaven scene: the
    // ObjectSky / cloud volume in heaven.world is positioned at world
    // Z = -1000 (transform translation), with the floating islands
    // sitting around Z = 0..25. So the cloud layer lives well *below*
    // the islands — the benchmark camera flies among them and looks
    // down onto the deck. (Z-up coordinate system; see FILE_FORMATS.md.)
    mi_->setParameter("cloudHeight", ctx.test_scene_assets
        ? ctx.test_scene_assets->atmosphere().cloud_plane_z
        : render_tuning::clouds.legacy_height);
    if (ctx.test_scene_assets) {
        const auto motion = render_tuning::clouds.shader_motion;
        mi_->setParameter("cloudMotionTuning", filament::math::float4{
            motion.x, motion.y, ctx.test_scene_assets->atmosphere().cloud_texture_scale, motion.w});
    }
    mi_->setParameter("cameraPos", filament::math::float3{0, 0, 0});
    const auto illumination = cloud_lighting(lighting_for_time_of_day(config.time_of_day));
    mi_->setParameter(
        "sunDir", filament::math::float3{illumination.direction.x, illumination.direction.y, illumination.direction.z});
    mi_->setParameter(
        "sunColor",
        filament::math::float3{illumination.direct.x, illumination.direct.y, illumination.direct.z});
    mi_->setParameter(
        "skyColor",
        filament::math::float3{illumination.ambient.x, illumination.ambient.y, illumination.ambient.z});
    mi_->setParameter("time", 0.0f);

    // Device-domain fullscreen quad. The material moves only clip-space Z
    // to the far plane; its original Z=0 reconstructs finite view rays.
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

    // This optional legacy plane approximation is separate from the
    // raymarched volumetric clouds and retains its configured default.
    entity_ = utils::EntityManager::get().create();

    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  vb_, ib_)
        .material(0, mi_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(false)
        .priority(0) // background before other transparent effects
        .boundingBox(uncullable_box())
        .build(*engine_, entity_);

    // Honour the configured default; this legacy approximation is opt-in.
    enabled_ = config.features.clouds;
    if (enabled_) {
        ctx.scene->addEntity(entity_);
        in_scene_ = true;
        std::printf("  Clouds: enabled\n");
    } else {
        std::printf("  Clouds: dormant (legacy layer disabled)\n");
    }
}

void CloudFeature::on_enabled_changed(RenderContext& ctx, bool on) {
    if (!entity_) return;
    if (on && !in_scene_) {
        ctx.scene->addEntity(entity_);
        in_scene_ = true;
    } else if (!on && in_scene_) {
        ctx.scene->remove(entity_);
        in_scene_ = false;
    }
}

void CloudFeature::update(RenderContext& /*ctx*/, const FrameContext& frame) {
    if (!mi_) return;
    const Camera& camera = *frame.camera;
    const LightingParams& lighting = *frame.lighting;
    const auto illumination = cloud_lighting(lighting);

    // Update dynamic parameters
    mi_->setParameter("cameraPos",
        filament::math::float3{camera.position.x, camera.position.y, camera.position.z});
    mi_->setParameter("sunDir",
        filament::math::float3{illumination.direction.x, illumination.direction.y, illumination.direction.z});
    mi_->setParameter("sunColor",
        filament::math::float3{illumination.direct.x, illumination.direct.y, illumination.direct.z});
    mi_->setParameter("skyColor",
        filament::math::float3{illumination.ambient.x, illumination.ambient.y, illumination.ambient.z});
    mi_->setParameter("time", frame.time);
}

void CloudFeature::destroy(RenderContext& ctx) {
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
    // tex1_/tex2_/mask_ are owned by the resource provider's cache.
    tex1_ = tex2_ = mask_ = nullptr;
}


void CloudFeature::append_temporal_draws(std::vector<TemporalEffectDraw>& draws) const {
    if (!enabled_ || !in_scene_ || !entity_ || !mi_) return;
    const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR, filament::TextureSampler::WrapMode::REPEAT);
    TemporalEffectDraw draw{.identity = entity_.getId(), .kind = TemporalEffectKind::CloudPlane,
        .source_entity = entity_, .vertices = vb_, .indices = ib_, .source = mi_};
    draw.textures[0] = {"cloudTex1", tex1_, sampler};
    draw.textures[1] = {"cloudTex2", tex2_, sampler};
    draw.textures[2] = {"cloudMask", mask_ ? mask_ : tex1_, sampler};
    draws.push_back(draw);
}

} // namespace heaven
