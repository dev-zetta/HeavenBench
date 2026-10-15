/*
 * HeavenBench — Lens flare feature implementation
 *
 * Moved verbatim out of FilamentBackend::init_lens_flares /
 * update_lens_flares. The build now reads the backend's scene lights
 * through FrameContext::scene_lights instead of reaching into the
 * backend's emission/authored light vectors.
 */
#include "rhi/features/lens_flare_feature.h"
#include "rhi/frame_generation/temporal_effects.h"
#include "rhi/render_test_scene_assets.h"
#include "engine/feature_assets.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "engine/path_resolver.h"
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/LightManager.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/TextureSampler.h>

#include <utils/EntityManager.h>

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/mat4.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <utility>

namespace heaven {

void LensFlareFeature::init(RenderContext& ctx, const AppConfig& config) {
    auto* engine_ = ctx.engine;
    enabled_ = config.features.lens_flares;

    // The lens-flare halo reuses the `fire_glow_dr` soft-halo sprite
    // (shared with the particle feature; the texture cache dedupes).
    glow_tex_ = resolve_feature_texture(
        ctx, FeatureAsset::Glow, TextureSemantic::ColorSRGB);

    material_ = filament::Material::Builder()
        .package(k_heaven_lens_flare_filamat, k_heaven_lens_flare_filamat_size)
        .build(*engine_);
    if (!material_) {
        std::printf("  Lens flare: material failed to load\n");
        return;
    }
    if (!glow_tex_) {
        std::printf("  Lens flare: glow texture missing — disabled\n");
        return;
    }

    // Shared index buffer (every flare has the same 2-triangle index
    // pattern). The VB is per-flare, built lazily — each flare bakes
    // its world position into its POSITION attribute (firefly pattern).
    const uint32_t* idx = new uint32_t[6]{0, 1, 2, 0, 2, 3};
    ib_ = filament::IndexBuffer::Builder()
        .indexCount(6)
        .bufferType(filament::IndexBuffer::IndexType::UINT)
        .build(*engine_);
    ib_->setBuffer(*engine_,
        filament::IndexBuffer::BufferDescriptor(
            idx, 6 * sizeof(uint32_t),
            [](void* b, size_t, void*) { delete[] static_cast<uint32_t*>(b); }));
}

void LensFlareFeature::build(RenderContext& ctx, const FrameContext& frame) {
    auto* engine_ = ctx.engine;
    built_ = true;

    filament::TextureSampler glow_sampler(
        filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);

    // Linear scene-depth sampler — point-sampled, clamped (same as the
    // other depth consumers, godrays / volfog).
    filament::TextureSampler depth_sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);

    // Build a flare anchored at `pos`. Each flare gets its own
    // 4-vertex VB with the world-space anchor baked into the POSITION
    // attribute — the vertex shader reads it via
    // material.worldPosition.xyz and adds camera-aligned corner
    // offsets for the quad.
    auto build_flare = [&](utils::Entity light_entity,
                           glm::vec3 pos,
                           int floating_group,
                           glm::vec3 local_pos,
                           int instance_index,
                           std::size_t source_index,
                           float nominal_intensity,
                           filament::math::float3 tint,
                           float brightness,
                           float fade_far,
                           float fade_near,
                           float max_radius) {
        Flare lf;
        lf.light_entity = light_entity;
        lf.world_pos = pos;
        lf.spawn_pos = pos;
        lf.floating_group = floating_group;
        lf.local_pos = local_pos;
        lf.instance_index = instance_index;
        lf.source_index = source_index;
        lf.nominal_intensity = nominal_intensity;
        lf.base_brightness = brightness;

        struct Vtx { filament::math::float3 p; filament::math::float2 uv; };
        auto* verts = new Vtx[4]{
            {{pos.x, pos.y, pos.z}, {0.0f, 0.0f}},
            {{pos.x, pos.y, pos.z}, {1.0f, 0.0f}},
            {{pos.x, pos.y, pos.z}, {1.0f, 1.0f}},
            {{pos.x, pos.y, pos.z}, {0.0f, 1.0f}},
        };
        auto* tangents = new filament::math::short4[4]{
            {0, 0, 16383, 16383}, {0, 0, 16383, 16383},
            {0, 0, 16383, 16383}, {0, 0, 16383, 16383},
        };

        lf.vb = filament::VertexBuffer::Builder()
            .vertexCount(4)
            .bufferCount(2)
            .attribute(filament::VertexAttribute::POSITION, 0,
                       filament::VertexBuffer::AttributeType::FLOAT3,
                       offsetof(Vtx, p), sizeof(Vtx))
            .attribute(filament::VertexAttribute::UV0, 0,
                       filament::VertexBuffer::AttributeType::FLOAT2,
                       offsetof(Vtx, uv), sizeof(Vtx))
            .attribute(filament::VertexAttribute::TANGENTS, 1,
                       filament::VertexBuffer::AttributeType::SHORT4, 0, 0)
            .normalized(filament::VertexAttribute::TANGENTS)
            .build(*engine_);
        lf.vb->setBufferAt(*engine_, 0,
            filament::VertexBuffer::BufferDescriptor(
                verts, 4 * sizeof(Vtx),
                [](void* b, size_t, void*) { delete[] static_cast<Vtx*>(b); }));
        lf.vb->setBufferAt(*engine_, 1,
            filament::VertexBuffer::BufferDescriptor(
                tangents, 4 * sizeof(filament::math::short4),
                [](void* b, size_t, void*) {
                    delete[] static_cast<filament::math::short4*>(b);
                }));

        lf.mi = material_->createInstance();
        if (!lf.mi) return;
        material_tuning::bind_lens_flare(*lf.mi);

        lf.mi->setParameter("flareTex", glow_tex_, glow_sampler);
        // Bind a placeholder for the linear scene-depth sampler so the
        // descriptor is always valid; update() swaps in the real
        // FrameContext::depth_texture once the depth pre-pass has
        // produced it. The zero-reading depth placeholder makes the
        // material's fragment fallback treat the read as "far / no
        // geometry" (sceneDist <= 0 → 2000 m) so the flare is NOT
        // occluded before/without a real depth texture.
        lf.mi->setParameter("depthTexture",
                            ctx.resources->placeholder_depth_texture(), depth_sampler);
        lf.mi->setParameter("tint",       tint);
        lf.mi->setParameter("fadeFar",    fade_far);
        lf.mi->setParameter("fadeNear",   fade_near);
        lf.mi->setParameter("maxRadius",  max_radius);
        lf.mi->setParameter("brightness", brightness);

        lf.entity = utils::EntityManager::get().create();
        // Bounding box covers the spawn position plus the max possible
        // flare radius. Per-frame motion is applied via TransformManager.
        const float r = std::max(
            render_tuning::lens_flare.minimum_bounds_radius,
            max_radius * render_tuning::lens_flare.bounds_radius_scale);
        filament::RenderableManager::Builder(1)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                      lf.vb, ib_, 0, 6)
            .material(0, lf.mi)
            .castShadows(false)
            .receiveShadows(false)
            .culling(false)
            .priority(7)
            .boundingBox(box_from_min_max(
                {lf.world_pos.x - r, lf.world_pos.y - r, lf.world_pos.z - r},
                {lf.world_pos.x + r, lf.world_pos.y + r, lf.world_pos.z + r}))
            .build(*engine_, lf.entity);

        if (enabled_) ctx.scene->addEntity(lf.entity);
        auto& tcm = engine_->getTransformManager();
        tcm.create(lf.entity);

        flares_.push_back(lf);
    };

    size_t n_authored = 0, n_emission = 0;
    for (std::size_t source_index = 0; source_index < frame.scene_lights.size(); ++source_index) {
        const auto& sl = frame.scene_lights[source_index];
        // An illumination origin (including an authored accent spot) is not
        // necessarily a visible bulb. The spawner explicitly identifies the
        // glass enclosure or an associated flame; this also avoids a second
        // flare for the authored spot inside the same lamp glass.
        if (!sl.flare_source) continue;
        const filament::math::float3 color{sl.color.x, sl.color.y, sl.color.z};
        build_flare(sl.entity, sl.world_pos, sl.floating_group,
                    sl.local_pos, sl.instance_index, source_index,
                    sl.nominal_intensity > 0.0f ? sl.nominal_intensity : sl.intensity,
                    color, 0.32f, 55.0f, 7.0f, 0.34f);
        if (sl.authored) ++n_authored;
        else ++n_emission;
    }

    in_scene_ = enabled_;
    std::printf("  Lens flares: built %zu (%zu authored + %zu emission)\n",
                flares_.size(), n_authored, n_emission);
}

void LensFlareFeature::on_enabled_changed(RenderContext& ctx, bool on) {
    if (!built_) return;   // build() honours enabled_ when it runs
    if (on && !in_scene_) {
        for (auto& lf : flares_) if (lf.entity) ctx.scene->addEntity(lf.entity);
        in_scene_ = true;
    } else if (!on && in_scene_) {
        for (auto& lf : flares_) if (lf.entity) ctx.scene->remove(lf.entity);
        in_scene_ = false;
    }
}

void LensFlareFeature::on_depth_prepass_begin(RenderContext& ctx) {
    if (!in_scene_) return;
    for (auto& lf : flares_) if (lf.entity) ctx.scene->remove(lf.entity);
}

void LensFlareFeature::on_depth_prepass_end(RenderContext& ctx) {
    if (!in_scene_) return;
    for (auto& lf : flares_) if (lf.entity) ctx.scene->addEntity(lf.entity);
}

void LensFlareFeature::release_shared_targets(RenderContext& ctx) {
    const filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    for (auto& entry : flares_) {
        if (entry.mi) entry.mi->setParameter("depthTexture",
            ctx.resources->placeholder_depth_texture(), sampler);
    }
    bound_depth_ = nullptr;
}

void LensFlareFeature::update(RenderContext& ctx, const FrameContext& frame) {
    if (!material_ || !ib_) return;

    // Lazy build once the scene lights are available (the backend
    // populates FrameContext::scene_lights after the scene is built and
    // authored lights spawned).
    if (!built_) {
        if (frame.scene_lights.empty()) return;
        build(ctx, frame);
    }

    // Bind the shared linear scene-depth texture onto every flare's
    // material instance once it exists / on resize. Done here in
    // update() (which the backend runs BEFORE any render pass, after
    // ensuring the depth target) so the descriptor-set update never
    // happens mid-render-pass — Vulkan forbids that (AGENTS.md
    // depth-prepass participation). Each flare owns its own
    // MaterialInstance, so the new texture is set on all of them.
    if (frame.depth_texture && frame.depth_texture != bound_depth_) {
        filament::TextureSampler depth_sampler(
            filament::TextureSampler::MinFilter::NEAREST,
            filament::TextureSampler::MagFilter::NEAREST,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        for (auto& lf : flares_) {
            if (lf.mi) {
                lf.mi->setParameter("depthTexture", frame.depth_texture, depth_sampler);
            }
        }
        bound_depth_ = frame.depth_texture;
    }

    auto& tcm = ctx.engine->getTransformManager();
    auto& light_manager = ctx.engine->getLightManager();

    // Resolve each flare's current world position:
    //   - floating_group >= 0: anchor + (0,0,z_offset) for the island bob.
    //   - instance_index >= 0: instance.transform * local_pos (follows an
    //     animated parent mesh — building windows etc.).
    // Apply the delta from the baked spawn
    // position as the entity transform (Filament composes it with the
    // model-space POSITION attribute). Occlusion is the soft scene-depth
    // comparison in the fragment shader (depthTexture bound above), so
    // nothing else is needed here.
    for (auto& lf : flares_) {
        if (!lf.mi) continue;
        float intensity = 0.0f;

        // Read the same live source position used for illumination. Retain the
        // explicit descriptor-only route for fixtures without a light entity.
        if (const auto light = light_manager.getInstance(lf.light_entity)) {
            const auto position = light_manager.getPosition(light);
            lf.world_pos = {position.x, position.y, position.z};
            intensity = light_manager.getIntensity(light);
        } else if (!lf.light_entity && lf.source_index < frame.scene_lights.size()) {
            // Explicit descriptor-only fixtures still have a power state.
            // A retired real source does not fall back to a cosmetic flare.
            const auto& source = frame.scene_lights[lf.source_index];
            if (!source.entity && source.flare_source) intensity = source.intensity;
        }
        if (!lf.light_entity && lf.floating_group >= 0) {
            float z_off = 0.0f;
            for (const auto& [g, z] : frame.floating_offsets) {
                if (g == lf.floating_group) { z_off = z; break; }
            }
            lf.world_pos = lf.local_pos + glm::vec3(0.0f, 0.0f, z_off);
        } else if (!lf.light_entity && lf.instance_index >= 0 &&
                   std::cmp_less(lf.instance_index, frame.instances.size())) {
            const auto& inst = frame.instances[lf.instance_index];
            glm::vec4 wp4 = inst.transform * glm::vec4(lf.local_pos, 1.0f);
            lf.world_pos = glm::vec3(wp4);
        }

        const float angular_gain = frame.camera ? lens_flare_angular_gain(
            frame.camera->front(), lf.world_pos - frame.camera->position) : 0.0f;
        lf.mi->setParameter("brightness", lf.base_brightness *
            lens_flare_power_gain(intensity, lf.nominal_intensity) * angular_gain);

        glm::vec3 delta = lf.world_pos - lf.spawn_pos;
        if (auto ti = tcm.getInstance(lf.entity); ti) {
            filament::math::mat4f m = filament::math::mat4f::translation(
                filament::math::float3{delta.x, delta.y, delta.z});
            tcm.setTransform(ti, m);
        }
    }
}

void LensFlareFeature::destroy(RenderContext& ctx) {
    auto* engine_ = ctx.engine;
    for (auto& lf : flares_) {
        if (lf.entity) {
            if (in_scene_) ctx.scene->remove(lf.entity);
            engine_->destroy(lf.entity);
            utils::EntityManager::get().destroy(lf.entity);
        }
        if (lf.mi) engine_->destroy(lf.mi);
        if (lf.vb) engine_->destroy(lf.vb);
    }
    flares_.clear();
    if (ib_) { engine_->destroy(ib_); ib_ = nullptr; }
    if (material_) { engine_->destroy(material_); material_ = nullptr; }
    // glow_tex_ owned by the resource cache.
    glow_tex_ = nullptr;
    built_ = false;
    in_scene_ = false;
    bound_depth_ = nullptr;
}


void LensFlareFeature::append_temporal_draws(std::vector<TemporalEffectDraw>& draws) const {
    if (!enabled_ || !in_scene_) return;
    const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR, filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    for (const auto& flare : flares_) {
        TemporalEffectDraw draw{.identity = flare.entity.getId(), .kind = TemporalEffectKind::LensFlare,
            .source_entity = flare.entity, .vertices = flare.vb, .indices = ib_, .source = flare.mi};
        draw.textures[0] = {"flareTex", glow_tex_, sampler};
        draw.textures[1] = {"depthTexture", bound_depth_,
            {filament::TextureSampler::MinFilter::NEAREST, filament::TextureSampler::MagFilter::NEAREST,
             filament::TextureSampler::WrapMode::CLAMP_TO_EDGE}};
        draws.push_back(draw);
    }
}

} // namespace heaven
