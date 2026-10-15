/*
 * HeavenBench — Particle + heat-haze feature implementation
 *
 * Moved verbatim out of FilamentBackend::init_particles /
 * set_particle_emitters / update_particles / init_composite /
 * ensure_scene_capture and the composite section of render_frame.
 */
#include "rhi/features/particle_feature.h"
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
#include <filament/Renderer.h>
#include <filament/Viewport.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/TextureSampler.h>

#include <utils/EntityManager.h>

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

namespace heaven {

void ParticleFeature::init(RenderContext& ctx, const AppConfig& config) {
    auto* engine_ = ctx.engine;
    enabled_ = config.features.particles;

    // Load the GPU-driven smoke material. All per-particle motion
    // happens in heaven_smoke.mat's vertex shader, so the only thing
    // we have to set up here is the Material itself; per-emitter
    // MaterialInstances are created later in build_emitters() once
    // we know what emitters the scene gave us.
    smoke_material_ = filament::Material::Builder()
        .package(k_heaven_smoke_filamat, k_heaven_smoke_filamat_size)
        .build(*engine_);
    if (!smoke_material_) {
        std::printf("  Particles: smoke material failed to load\n");
        return;
    }

    // ── Flame & glow materials (torch / brazier VFX) ────────────
    flame_material_ = filament::Material::Builder()
        .package(k_heaven_flame_filamat, k_heaven_flame_filamat_size)
        .build(*engine_);
    if (!flame_material_) {
        std::printf("  Particles: flame material failed to load\n");
    }

    glow_material_ = filament::Material::Builder()
        .package(k_heaven_glow_filamat, k_heaven_glow_filamat_size)
        .build(*engine_);
    if (!glow_material_) {
        std::printf("  Particles: glow material failed to load\n");
    }

    firefly_material_ = filament::Material::Builder()
        .package(k_heaven_firefly_filamat, k_heaven_firefly_filamat_size)
        .build(*engine_);
    if (!firefly_material_) {
        std::printf("  Particles: firefly material failed to load\n");
    }

    // Load fire textures from the extracted asset tree. The flame
    // itself is now fully procedural (heaven_flame.mat), so it needs
    // no texture; we only need the soft halo for the glow particles
    // and fireflies.
    glow_tex_ = resolve_feature_texture(
        ctx, FeatureAsset::Glow, TextureSemantic::ColorSRGB);
    if (!glow_tex_) {
        std::printf("  Particles: glow texture missing — glow/firefly disabled\n");
    }

    // ── Heat-haze composite pipeline ────────────────────────────
    heat_haze_enabled_ = config.features.heat_haze;
    if (!heat_haze_enabled_) {
        std::printf("  Composite pass: disabled (--no-heat-haze)\n");
    } else {
        blit_material_ = filament::Material::Builder()
            .package(k_heaven_blit_filamat, k_heaven_blit_filamat_size)
            .build(*engine_);
        if (!blit_material_) {
            std::printf("  Composite: blit material failed to load\n");
            heat_haze_enabled_ = false;
        }
    }

    if (heat_haze_enabled_) {
        blit_mi_ = blit_material_->createInstance();

        heat_haze_material_ = filament::Material::Builder()
            .package(k_heaven_heat_haze_filamat, k_heaven_heat_haze_filamat_size)
            .build(*engine_);
        if (!heat_haze_material_) {
            std::printf("  Composite: heat-haze material failed to load\n");
        }

        // ── Fullscreen blit quad ───────────────────────────────
        {
            struct Vert { float x, y, z; float u, v; };
            constexpr std::array<Vert, 4> verts = {{
                {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f},
                { 1.0f, -1.0f, 0.0f, 1.0f, 0.0f},
                { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f},
                {-1.0f,  1.0f, 0.0f, 0.0f, 1.0f},
            }};
            constexpr std::array<std::uint16_t, 6> indices = {
                0, 1, 2, 0, 2, 3};

            blit_vb_ = filament::VertexBuffer::Builder()
                .vertexCount(4).bufferCount(1)
                .attribute(filament::VertexAttribute::POSITION, 0,
                           filament::VertexBuffer::AttributeType::FLOAT3, 0, sizeof(Vert))
                .attribute(filament::VertexAttribute::UV0,      0,
                           filament::VertexBuffer::AttributeType::FLOAT2, 12, sizeof(Vert))
                .build(*engine_);

            auto vbuf = std::make_unique<Vert[]>(verts.size());
            std::ranges::copy(verts, vbuf.get());
            blit_vb_->setBufferAt(*engine_, 0,
                filament::VertexBuffer::BufferDescriptor(
                    vbuf.release(), sizeof(verts),
                    [](void* b, size_t, void*) { delete[] static_cast<Vert*>(b); }));

            blit_ib_ = filament::IndexBuffer::Builder()
                .indexCount(6)
                .bufferType(filament::IndexBuffer::IndexType::USHORT)
                .build(*engine_);

            auto ibuf =
                std::make_unique<std::uint16_t[]>(indices.size());
            std::ranges::copy(indices, ibuf.get());
            blit_ib_->setBuffer(*engine_,
                filament::IndexBuffer::BufferDescriptor(
                    ibuf.release(), sizeof(indices),
                    [](void* b, size_t, void*) {
                        delete[] static_cast<std::uint16_t*>(b);
                    }));

            blit_entity_ = utils::EntityManager::get().create();
            filament::RenderableManager::Builder(1)
                .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                          blit_vb_, blit_ib_, 0, 6)
                .material(0, blit_mi_)
                .castShadows(false)
                .receiveShadows(false)
                .culling(false)
                .priority(0)  // draw before haze quads
                .boundingBox(uncullable_box())
                .build(*engine_, blit_entity_);

            engine_->getTransformManager().create(blit_entity_);
        }

        // ── Composite scene + view ─────────────────────────────
        composite_scene_ = engine_->createScene();
        composite_scene_->addEntity(blit_entity_);

        composite_view_ = engine_->createView();
        composite_view_->setScene(composite_scene_);
        composite_view_->setCamera(ctx.camera);
        composite_view_->setPostProcessingEnabled(false);
        composite_view_->setShadowingEnabled(false);
        composite_view_->setBlendMode(filament::View::BlendMode::OPAQUE);
        // Render target: nullptr = swap chain (default).

        std::printf("  Composite pass initialized\n");
    }

    std::printf("  Particles: GPU smoke material loaded, awaiting emitters%s\n",
                enabled_ ? "" : " (disabled)");
}

void ParticleFeature::set_emitters(std::span<const ParticleEmitter> emitters) {
    pending_.assign(emitters.begin(), emitters.end());
    if (!pending_.empty()) {
        std::printf("  Particles: received %zu emitters from scene\n",
                    pending_.size());
    }
}

void ParticleFeature::build_emitters(RenderContext& ctx) {
    auto* engine_ = ctx.engine;

    // Track how many of each particle class we end up building so
    // the startup log gives the user feedback on what was wired up.
    size_t n_smoke = 0, n_flame = 0, n_glow = 0;

    for (const auto& src : pending_) {
        if (!src.emitter_enabled) continue;
        if (src.material_name.empty()) continue;
        // Refraction-only materials need a separate pass; skip them.
        if (src.material_name.find("refraction") != std::string::npos) continue;

        // Number of slots: each slot has a unique phase offset of
        // `lifetime / slots` seconds, so when `slots = lifetime *
        // spawn_rate` exactly one new particle is alive per
        // spawn-interval window.
        float life = std::max(1.0f, src.life_mean);
        float rate = std::max(0.5f, src.spawn_rate);
        int   slots_target = static_cast<int>(std::round(life * rate));
        // Fireflies need a denser swarm than the authored data
        // suggests (Heaven's flies.node has spawn_rate≈0.5,
        // life≈0.8s → ~5 alive at a time). Crank it up to 24.
        const bool is_flies_emitter = (src.material_name == "flies");
        if (is_flies_emitter)
            slots_target = render_tuning::particles.firefly_slots;
        // The procedural flame is rendered as a small set of FIXED
        // world-oriented cross-planes (an asterisk of quads at
        // evenly-spaced yaws), not a camera-facing billboard. 3
        // planes give the flame visible volume from any side angle.
        const bool is_flame_emitter = (src.material_name == "fire_flame");
        const int slots = is_flame_emitter
            ? render_tuning::particles.flame_planes
            : std::clamp(
                slots_target,
                render_tuning::particles.minimum_slots,
                render_tuning::particles.maximum_slots);
        const int vert_count = slots * 4;
        const int idx_count  = slots * 6;

        // Per-particle data
        //   POSITION.xyz = emitter origin (world space)
        //   UV0.xy       = corner ID
        //   CUSTOM0.xyzw = (slot, base_radius, growth, velocity)
        //   CUSTOM1.x    = lifetime
        //   TANGENTS     = identity quaternion (Filament requires it)
        auto positions =
            std::make_unique<filament::math::float3[]>(vert_count);
        auto uvs = std::make_unique<filament::math::float2[]>(vert_count);
        auto customs0 =
            std::make_unique<filament::math::float4[]>(vert_count);
        auto customs1 =
            std::make_unique<filament::math::float4[]>(vert_count);
        auto tangents =
            std::make_unique<filament::math::short4[]>(vert_count);

        glm::vec3 origin = glm::vec3(src.transform[3]);
        const bool flies = is_flies_emitter;
        constexpr std::array<filament::math::float2, 4> corners = {{
            {0, 0}, {1, 0}, {1, 1}, {0, 1}
        }};
        for (int s = 0; s < slots; ++s) {
            int v = s * 4;
            for (int c = 0; c < 4; ++c) {
                positions[v + c] = {origin.x, origin.y, origin.z};
                uvs[v + c]       = corners[c];
                float radius_m = std::max(0.001f, src.radius_mean);
                float growth_m = src.growth_mean;
                float velocity = std::max(0.0f, src.velocity_mean);
                float lifetime = std::max(0.5f, src.life_mean);
                if (flies) {
                    // Each firefly is a 4 cm bright speck.
                    const auto firefly =
                        render_tuning::particles.firefly_geometry;
                    radius_m = firefly.x;
                    growth_m = firefly.y;
                    velocity = firefly.z;
                    lifetime = firefly.w;
                }
                customs0[v + c]  = filament::math::float4{
                    static_cast<float>(s),
                    radius_m, growth_m, velocity
                };
                customs1[v + c]  = filament::math::float4{
                    lifetime, 0.0f, 0.0f, 0.0f
                };
                tangents[v + c]  = {0, 0, 16383, 16383};
            }
        }

        // Index buffer (two triangles per quad)
        auto indices =
            std::make_unique<std::uint32_t[]>(idx_count);
        for (int s = 0; s < slots; ++s) {
            const auto v = static_cast<std::uint32_t>(s) * 4u;
            std::uint32_t* const o = &indices[static_cast<size_t>(s) * 6];
            o[0] = v + 0; o[1] = v + 1; o[2] = v + 2;
            o[3] = v + 0; o[4] = v + 2; o[5] = v + 3;
        }

        Emitter se;
        se.origin = origin;
        se.floating_group = src.floating_group;
        se.vb = filament::VertexBuffer::Builder()
            .vertexCount(vert_count)
            .bufferCount(5)
            .attribute(filament::VertexAttribute::POSITION, 0,
                       filament::VertexBuffer::AttributeType::FLOAT3, 0, 12)
            .attribute(filament::VertexAttribute::UV0, 1,
                       filament::VertexBuffer::AttributeType::FLOAT2, 0, 8)
            .attribute(filament::VertexAttribute::CUSTOM0, 2,
                       filament::VertexBuffer::AttributeType::FLOAT4, 0, 16)
            .attribute(filament::VertexAttribute::CUSTOM1, 3,
                       filament::VertexBuffer::AttributeType::FLOAT4, 0, 16)
            .attribute(filament::VertexAttribute::TANGENTS, 4,
                       filament::VertexBuffer::AttributeType::SHORT4, 0, 0)
            .normalized(filament::VertexAttribute::TANGENTS)
            .build(*engine_);

        se.vb->setBufferAt(*engine_, 0,
            filament::VertexBuffer::BufferDescriptor(
                positions.release(), vert_count * sizeof(filament::math::float3),
                [](void* b, size_t, void*) {
                    delete[] static_cast<filament::math::float3*>(b);
                }));
        se.vb->setBufferAt(*engine_, 1,
            filament::VertexBuffer::BufferDescriptor(
                uvs.release(), vert_count * sizeof(filament::math::float2),
                [](void* b, size_t, void*) {
                    delete[] static_cast<filament::math::float2*>(b);
                }));
        se.vb->setBufferAt(*engine_, 2,
            filament::VertexBuffer::BufferDescriptor(
                customs0.release(), vert_count * sizeof(filament::math::float4),
                [](void* b, size_t, void*) {
                    delete[] static_cast<filament::math::float4*>(b);
                }));
        se.vb->setBufferAt(*engine_, 3,
            filament::VertexBuffer::BufferDescriptor(
                customs1.release(), vert_count * sizeof(filament::math::float4),
                [](void* b, size_t, void*) {
                    delete[] static_cast<filament::math::float4*>(b);
                }));
        se.vb->setBufferAt(*engine_, 4,
            filament::VertexBuffer::BufferDescriptor(
                tangents.release(), vert_count * sizeof(filament::math::short4),
                [](void* b, size_t, void*) {
                    delete[] static_cast<filament::math::short4*>(b);
                }));

        se.ib = filament::IndexBuffer::Builder()
            .indexCount(idx_count)
            .bufferType(filament::IndexBuffer::IndexType::UINT)
            .build(*engine_);
        se.ib->setBuffer(*engine_,
            filament::IndexBuffer::BufferDescriptor(
                indices.release(), idx_count * sizeof(std::uint32_t),
                [](void* b, size_t, void*) {
                    delete[] static_cast<std::uint32_t*>(b);
                }));

        // Per-emitter material instance: dispatch by source material
        // name. Smoke materials route through the FBM smoke shader;
        // flame and glow materials sample real sprite textures.
        const auto& m = src.material_name;
        const bool is_flame = (m == "fire_flame") && flame_material_;
        const bool is_glow  = (m == "fire_glow") && glow_material_ && glow_tex_;
        const bool is_firefly = (m == "flies") && firefly_material_ && glow_tex_;
        se.temporal_kind = is_flame ? TemporalEffectKind::Flame :
            is_glow ? TemporalEffectKind::Glow : is_firefly ? TemporalEffectKind::Firefly : TemporalEffectKind::Smoke;

        filament::math::float3 tint{0.55f, 0.45f, 0.40f};
        float alpha_scale = 0.10f;

        if (is_flame) {
            se.mi = flame_material_->createInstance();
            material_tuning::bind_flame(*se.mi);
            ++n_flame;
            tint = {1.0f, 0.9f, 0.7f};
            alpha_scale = 1.0f;

            // Per-emitter seed so adjacent torches don't flicker in
            // lock-step. Derived from the emitter origin.
            glm::vec3 o = glm::vec3(src.transform[3]);
            float seed = std::fabs(std::sin(o.x * 12.9898f +
                                            o.y * 78.233f +
                                            o.z * 37.719f)) * 43758.5453f;
            seed = seed - std::floor(seed);
            se.mi->setParameter("seed", seed);
            se.mi->setParameter(
                "flameSpeed", render_tuning::particles.flame_speed);
        } else if (is_glow) {
            se.mi = glow_material_->createInstance();
            material_tuning::bind_glow(*se.mi);
            ++n_glow;
            if (m == "flies") {
                tint = {0.85f, 1.0f, 0.45f};
                alpha_scale = 0.6f;
            } else {
                tint = {1.0f, 0.55f, 0.20f};
                alpha_scale = 0.05f;
            }

            filament::TextureSampler glow_sampler(
                filament::TextureSampler::MinFilter::LINEAR,
                filament::TextureSampler::MagFilter::LINEAR,
                filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
            se.mi->setParameter("glowTex", glow_tex_, glow_sampler);
        } else if (is_firefly) {
            se.mi = firefly_material_->createInstance();
            material_tuning::bind_firefly(*se.mi);
            ++n_glow;  // counted as glow class for diagnostic
            tint = {1.0f, 0.95f, 0.55f};
            alpha_scale = 0.9f;

            filament::TextureSampler glow_sampler(
                filament::TextureSampler::MinFilter::LINEAR,
                filament::TextureSampler::MagFilter::LINEAR,
                filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
            se.mi->setParameter("glowTex", glow_tex_, glow_sampler);
        } else {
            se.mi = smoke_material_->createInstance();
            material_tuning::bind_smoke(*se.mi);
            ++n_smoke;
            if (m == "fire_smoke_white") {
                tint = {0.70f, 0.65f, 0.60f};
                alpha_scale = 0.12f;
            } else if (m == "fire_smoke") {
                tint = {0.55f, 0.45f, 0.40f};
                alpha_scale = 0.10f;
            } else if (m.find("flame") != std::string::npos) {
                tint = {1.6f, 1.0f, 0.45f};
                alpha_scale = 0.30f;
            }
        }

        // Spawn interval matches the slot count and lifetime so slots
        // cycle exactly through one full life period without overlap.
        float spawn_interval = life / static_cast<float>(slots);

        se.mi->setParameter("tint", tint);
        se.mi->setParameter("alphaScale", alpha_scale);
        se.mi->setParameter("time", 0.0f);
        // The procedural flame material has no `spawnInterval` param.
        if (!is_flame) {
            se.mi->setParameter("spawnInterval", spawn_interval);
        }

        const float halo = render_tuning::particles.bounds_halo;
        // Render priority: flame quads MUST draw on top of the glow
        // halo at the same emitter position.
        uint8_t prio = 5;
        if (is_flame)   prio = 6;          // flame on top
        if (is_glow)    prio = 4;          // glow underneath
        if (is_firefly) prio = 5;          // independent
        se.entity = utils::EntityManager::get().create();
        filament::RenderableManager::Builder(1)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                      se.vb, se.ib, 0, idx_count)
            .material(0, se.mi)
            .castShadows(false)
            .receiveShadows(false)
            .culling(false)
            .priority(prio)
            .boundingBox(box_from_min_max(
                {origin.x - halo, origin.y - halo, origin.z - halo},
                {origin.x + halo, origin.y + halo, origin.z + halo}))
            .build(*engine_, se.entity);

        if (enabled_) ctx.scene->addEntity(se.entity);

        auto& tcm = engine_->getTransformManager();
        tcm.create(se.entity);

        emitters_.push_back(se);

        // ── Companion heat-haze emitter ────────────────────────

        // Reuses the smoke emitter's VB+IB (identical layout) but
        // binds the heat-haze material instance. The renderable lives
        // in composite_scene_, NOT the main scene. We leave vb/ib null
        // on this copy so destroy() doesn't double-free the shared
        // buffers — the smoke emitter owns them.

        // Heat-haze only makes sense for *smoke* emitters (chimneys
        // with rising hot air). Flame/glow/firefly particles are tiny.
        if (heat_haze_material_ && composite_scene_
            && !is_flame && !is_glow && !is_firefly) {
            Emitter haze;
            haze.vb = nullptr;  // shared with smoke emitter
            haze.ib = nullptr;
            haze.borrowed_vb = se.vb;
            haze.borrowed_ib = se.ib;
            haze.mi = heat_haze_material_->createInstance();
            if (haze.mi) {
                material_tuning::bind_heat_haze(*haze.mi);
                haze.mi->setParameter("spawnInterval", spawn_interval);
                haze.mi->setParameter("time", 0.0f);

                float haze_strength = 0.020f;
                if (m.find("flame") != std::string::npos)
                    haze_strength = 0.035f;
                haze.mi->setParameter("strength", haze_strength);

                haze.mi->setParameter(
                    "fadeNear", render_tuning::particles.heat_haze_fade.x);
                haze.mi->setParameter(
                    "fadeFar", render_tuning::particles.heat_haze_fade.y);

                // Bind placeholders so no sampler is left undefined
                // before the first depth pre-pass / scene capture. The
                // depth sampler uses the zero-reading depth placeholder so
                // the shader treats it as "far / no geometry" (no occlusion)
                // until a real depth texture exists; the scene
                // colour sampler keeps the white placeholder.
                filament::TextureSampler ph_sampler(
                    filament::TextureSampler::MinFilter::LINEAR,
                    filament::TextureSampler::MagFilter::LINEAR,
                    filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
                const filament::Texture* ph = ctx.resources->placeholder_texture();
                haze.mi->setParameter("sceneDepth",
                                      ctx.resources->placeholder_depth_texture(),
                                      ph_sampler);
                if (scene_capture_tex_) {
                    haze.mi->setParameter("sceneTexture",
                                          scene_capture_tex_, ph_sampler);
                } else {
                    haze.mi->setParameter("sceneTexture", ph, ph_sampler);
                }

                haze.entity = utils::EntityManager::get().create();
                haze.origin = origin;
                haze.floating_group = src.floating_group;
                filament::RenderableManager::Builder(1)
                    .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                              emitters_.back().vb,
                              emitters_.back().ib,
                              0, idx_count)
                    .material(0, haze.mi)
                    .castShadows(false)
                    .receiveShadows(false)
                    .culling(false)
                    .priority(7)
                    .boundingBox(box_from_min_max(
                        {origin.x - halo, origin.y - halo, origin.z - halo},
                        {origin.x + halo, origin.y + halo, origin.z + halo}))
                    .build(*engine_, haze.entity);
                composite_scene_->addEntity(haze.entity);
                tcm.create(haze.entity);
                haze_emitters_.push_back(haze);
            }
        }
    }

    built_ = true;
    in_scene_ = enabled_;
    std::printf("  Particles: built %zu emitters (smoke=%zu, flame=%zu, glow=%zu) +%zu heat-haze\n",
                emitters_.size(), n_smoke, n_flame, n_glow,
                haze_emitters_.size());
}

void ParticleFeature::release_shared_targets(RenderContext& ctx) {
    if (composite_view_) composite_view_->setRenderTarget(nullptr);
    const filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    for (auto& entry : haze_emitters_) {
        if (entry.mi) entry.mi->setParameter("sceneDepth",
            ctx.resources->placeholder_depth_texture(), sampler);
    }
    for (auto& emitter : field_emitters_) {
        if (emitter.material) emitter.material->setParameter("sceneDepth",
            ctx.resources->placeholder_depth_texture(), sampler);
    }
    bound_depth_ = nullptr;
}

void ParticleFeature::update(RenderContext& ctx, const FrameContext& frame) {
    // Lazy first-time build once emitters have been handed over.
    if (!built_) {
        if (!smoke_material_) return;
        if (pending_.empty()) return;
        build_emitters(ctx);
    }

    // Per-frame: push time uniform to every emitter (smoke + heat
    // haze share the same animation clock).
    for (auto& se : emitters_) {
        if (se.mi) se.mi->setParameter("time", frame.time);
    }
    for (auto& he : haze_emitters_) {
        if (he.mi) he.mi->setParameter("time", frame.time);
    }

    // Bind the shared half-res depth pre-pass texture to the heat-haze
    // instances (occlusion test). Done in update() — outside any render
    // pass — and only on change/resize, mirroring how god rays / volfog
    // bind their depth sampler.
    if (frame.depth_texture && frame.depth_texture != bound_depth_) {
        filament::TextureSampler depth_sampler(
            filament::TextureSampler::MinFilter::NEAREST,
            filament::TextureSampler::MagFilter::NEAREST,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
        for (auto& he : haze_emitters_) {
            if (he.mi) he.mi->setParameter("sceneDepth",
                                           frame.depth_texture, depth_sampler);
        }
        bound_depth_ = frame.depth_texture;
    }

    // Floating-island tracking: translate emitters anchored to a
    // floating group by the group's per-frame Z offset. The emitter's
    // POSITION attribute was baked once at the static spawn position,
    // so we move the *entity transform* by Z to follow the island.
    if (!frame.floating_offsets.empty()) {
        auto& tcm = ctx.engine->getTransformManager();
        const auto update_anchors = [&](auto& emitters) {
            for (auto& emitter : emitters) {
                if (emitter.floating_group < 0 || !emitter.entity) continue;
                float z_offset = 0.0f;
                for (const auto& [group, offset] : frame.floating_offsets) {
                    if (group == emitter.floating_group) { z_offset = offset; break; }
                }
                if (const auto instance = tcm.getInstance(emitter.entity); instance)
                    tcm.setTransform(instance, filament::math::mat4f::translation(
                        filament::math::float3{0.0f, 0.0f, z_offset}));
            }
        };
        update_anchors(emitters_);
        update_anchors(haze_emitters_);
    }
}

// ── Scene-capture target management (heat haze) ────────────────

// The main view renders to this offscreen colour texture instead of
// the swap chain, so the heat-haze pass can sample it later. Rebuilt
// whenever the viewport resizes — full screen resolution to keep the
// bent samples sharp. RGBA16F so HDR colour values survive the
// round-trip without clipping.
void ParticleFeature::ensure_scene_capture(RenderContext& ctx, int w, int h) {
    auto* engine_ = ctx.engine;
    if (scene_capture_w_ == w && scene_capture_h_ == h && scene_capture_rt_)
        return;

    if (scene_capture_rt_ && ctx.view && ctx.view->getRenderTarget() == scene_capture_rt_) {
        ctx.view->setRenderTarget(nullptr);
    }
    if (scene_capture_rt_) { engine_->destroy(scene_capture_rt_); scene_capture_rt_ = nullptr; }
    if (scene_capture_tex_) { engine_->destroy(scene_capture_tex_); scene_capture_tex_ = nullptr; }
    if (scene_capture_depth_) { engine_->destroy(scene_capture_depth_); scene_capture_depth_ = nullptr; }

    scene_capture_w_ = w;
    scene_capture_h_ = h;

    // Filament TextureUsage explicitly permits combinations of these bit flags.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto color_usage = static_cast<filament::Texture::Usage>(
        std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
        std::to_underlying(filament::Texture::Usage::SAMPLEABLE));
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto depth_usage = static_cast<filament::Texture::Usage>(
        std::to_underlying(filament::Texture::Usage::DEPTH_ATTACHMENT) |
        std::to_underlying(filament::Texture::Usage::SAMPLEABLE));

    scene_capture_tex_ = filament::Texture::Builder()
        .width(w).height(h).levels(1)
        .usage(color_usage)
        .format(filament::Texture::InternalFormat::RGBA16F)
        .build(*engine_);

    scene_capture_depth_ = filament::Texture::Builder()
        .width(w).height(h).levels(1)
        .usage(depth_usage)
        .format(filament::Texture::InternalFormat::DEPTH32F)
        .build(*engine_);

    scene_capture_rt_ = filament::RenderTarget::Builder()
        .texture(filament::RenderTarget::AttachmentPoint::COLOR0, scene_capture_tex_)
        .texture(filament::RenderTarget::AttachmentPoint::DEPTH,  scene_capture_depth_)
        .build(*engine_);

    // Bind the new colour texture to every material that samples it
    // (blit + every heat-haze MaterialInstance).
    filament::TextureSampler scene_sampler(
        filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    if (blit_mi_)
        blit_mi_->setParameter("sceneTexture", scene_capture_tex_, scene_sampler);
    for (auto& he : haze_emitters_) {
        if (he.mi) he.mi->setParameter("sceneTexture",
                                       scene_capture_tex_, scene_sampler);
    }
}

bool ParticleFeature::wants_scene_capture(const FrameContext& frame) const {
    // The composite is expensive — it captures the whole scene to an
    // RGBA16F texture and re-blits it. Only enable it when at least
    // one chimney is within the haze visibility radius; otherwise skip
    // both the capture and the composite render and stay free.
    if (temporal_distortion_enabled_ && field_target_) return false;
    if (!enabled_ || !composite_view_ || !blit_mi_) return false;
    if (haze_emitters_.empty()) return false;
    constexpr float kHazeFadeFar =
        render_tuning::particles.heat_haze_fade.y;
    const float r2 = kHazeFadeFar * kHazeFadeFar;
    const glm::vec3 cam = frame.camera->position;
    return std::ranges::any_of(haze_emitters_, [cam, r2](const auto& he) {
        float dx = he.origin.x - cam.x;
        float dy = he.origin.y - cam.y;
        float dz = he.origin.z - cam.z;
        return dx * dx + dy * dy + dz * dz < r2;
    });
}

void ParticleFeature::begin_scene_capture(RenderContext& ctx, const FrameContext& frame) {
    auto* engine_ = ctx.engine;
    int viewport_w = frame.viewport_w;
    int viewport_h = frame.viewport_h;
    const Camera& camera = *frame.camera;

    ensure_scene_capture(ctx, viewport_w, viewport_h);
    ctx.view->setRenderTarget(scene_capture_rt_);
    composite_view_->setViewport({0, 0,
        static_cast<uint32_t>(viewport_w),
        static_cast<uint32_t>(viewport_h)});

    // Position the blit quad just inside the far plane so it always
    // covers the full viewport regardless of camera direction.
    float aspect_c = static_cast<float>(viewport_w) /
                     static_cast<float>(viewport_h);
    glm::mat4 view_mat_c = camera.view_matrix();
    glm::vec3 cam_pos_c  = camera.position;
    glm::vec3 cam_fwd_c  = -glm::vec3(view_mat_c[0][2], view_mat_c[1][2], view_mat_c[2][2]);
    glm::vec3 cam_right_c = glm::vec3(view_mat_c[0][0], view_mat_c[1][0], view_mat_c[2][0]);
    glm::vec3 cam_up_c    = glm::vec3(view_mat_c[0][1], view_mat_c[1][1], view_mat_c[2][1]);
    float dist_c   = 1990.0f;
    float half_h_c = dist_c * std::tan(glm::radians(camera.fov * 0.5f));
    float half_w_c = half_h_c * aspect_c;
    glm::vec3 center_c = cam_pos_c + cam_fwd_c * dist_c;
    glm::mat4 q(1.0f);
    q[0] = glm::vec4(cam_right_c * half_w_c, 0.0f);
    q[1] = glm::vec4(cam_up_c    * half_h_c, 0.0f);
    q[2] = glm::vec4(cam_fwd_c,              0.0f);
    q[3] = glm::vec4(center_c, 1.0f);
    auto& tcm = engine_->getTransformManager();
    if (auto ti = tcm.getInstance(blit_entity_); ti) {
        tcm.setTransform(ti, glm_to_filament(q));
    }
}

void ParticleFeature::render_composite(RenderContext& ctx) {
    if (composite_view_) {
        // Honour the backend's final output target: nullptr is the swap chain
        // (SDR), but under HDR display output the backend points this at the
        // linear Rec.2020 fp16 HDR scene target so the encode pass can PQ-encode
        // it to the swap chain.
        composite_view_->setRenderTarget(ctx.output_target);
        ctx.renderer->render(composite_view_);
    }
}

void ParticleFeature::set_temporal_distortion_enabled(bool enabled) {
    temporal_distortion_enabled_ = enabled;
}

bool ParticleFeature::prepare_temporal_distortion(RenderContext& ctx, int width, int height) {
    if (!temporal_distortion_enabled_ || width <= 0 || height <= 0) return false;
    auto& engine = *ctx.engine;
    const int grid_width = std::min(width, 64);
    const int grid_height = std::min(height, 64);
    if (!field_material_) {
        field_material_ = filament::Material::Builder()
            .package(k_heaven_heat_haze_field_filamat, k_heaven_heat_haze_field_filamat_size).build(engine);
        field_scene_ = engine.createScene();
        field_view_ = engine.createView();
        field_clear_ = filament::Skybox::Builder().color({0.0f, 0.0f, 0.0f, 0.0f}).build(engine);
        if (!field_material_ || !field_scene_ || !field_view_ || !field_clear_) return false;
        field_scene_->setSkybox(field_clear_);
        field_view_->setScene(field_scene_);
        field_view_->setCamera(ctx.camera);
        field_view_->setPostProcessingEnabled(false);
        field_view_->setShadowingEnabled(false);
        // The zero skybox must remain visible to replace disabled field data.
        field_view_->setVisibleLayers(0xff, 0xff);
    }
    if (!field_material_ || !field_scene_ || !field_view_ || !field_clear_) return false;
    if (!field_target_ || field_width_ != grid_width || field_height_ != grid_height) {
        field_view_->setRenderTarget(nullptr);
        if (field_target_) engine.destroy(field_target_);
        if (field_texture_) engine.destroy(field_texture_);
        field_target_ = nullptr;
        field_texture_ = nullptr;
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::SAMPLEABLE) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        field_texture_ = filament::Texture::Builder().width(static_cast<uint32_t>(grid_width))
            .height(static_cast<uint32_t>(grid_height)).levels(1).usage(usage)
            .format(filament::Texture::InternalFormat::RGBA16F).build(engine);
        if (!field_texture_) return false;
        field_target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, field_texture_).build(engine);
        if (!field_target_) return false;
        field_width_ = grid_width;
        field_height_ = grid_height;
        field_view_->setRenderTarget(field_target_);
        field_view_->setViewport({0, 0, static_cast<uint32_t>(grid_width), static_cast<uint32_t>(grid_height)});
    }
    if (field_emitters_.size() != haze_emitters_.size()) {
        for (auto& emitter : field_emitters_) {
            field_scene_->remove(emitter.entity);
            engine.destroy(emitter.entity);
            utils::EntityManager::get().destroy(emitter.entity);
            engine.destroy(emitter.material);
        }
        field_emitters_.clear();
        auto& renderables = engine.getRenderableManager();
        for (const auto& haze : haze_emitters_) {
            if (!haze.borrowed_vb || !haze.borrowed_ib || !haze.mi || !haze.entity) return false;
            auto* material = field_material_->createInstance();
            if (!material) return false;
            const auto entity = utils::EntityManager::get().create();
            const auto box = renderables.getAxisAlignedBoundingBox(renderables.getInstance(haze.entity));
            const auto result = filament::RenderableManager::Builder(1)
                .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, haze.borrowed_vb, haze.borrowed_ib)
                .material(0, material).boundingBox(box).culling(false).castShadows(false).receiveShadows(false)
                .build(engine, entity);
            if (result != filament::RenderableManager::Builder::Success) {
                engine.destroy(material);
                utils::EntityManager::get().destroy(entity);
                return false;
            }
            engine.getTransformManager().create(entity);
            field_scene_->addEntity(entity);
            field_emitters_.push_back({entity, material});
        }
    }
    // Every node's component is bounded by a. Bilinear differences therefore
    // give Lip(infinity) <= 2*a*(gridWidth+gridHeight) <= 0.4. Round the shader
    // bound down to an exactly half-representable value, with blending margin.
    const float maximum = 0.1f / static_cast<float>(std::max(grid_width, grid_height));
    int exponent = 0;
    std::frexp(maximum, &exponent);
    const float half_step = std::ldexp(1.0f, exponent - 11);
    const float bound = std::floor(maximum * 0.999f / half_step) * half_step;
    auto& transforms = engine.getTransformManager();
    auto& renderables = engine.getRenderableManager();
    const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST, filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    for (size_t index = 0; index < field_emitters_.size(); ++index) {
        auto& field = field_emitters_[index];
        const auto& haze = haze_emitters_[index];
        for (const auto* name : {"time", "spawnInterval", "strength", "fadeNear", "fadeFar"})
            field.material->setParameter(name, haze.mi->getParameter<float>(name));
        field.material->setParameter("hazeShapeTuning",
            haze.mi->getParameter<filament::math::float4>("hazeShapeTuning"));
        field.material->setParameter("fieldBound", bound);
        field.material->setParameter("fieldGridSize", filament::math::float2{
            static_cast<float>(grid_width), static_cast<float>(grid_height)});
        field.material->setParameter("sourceViewportSize", filament::math::float2{
            static_cast<float>(width), static_cast<float>(height)});
        field.material->setParameter("sceneDepth", bound_depth_ ? bound_depth_ :
            ctx.resources->placeholder_depth_texture(), sampler);
        transforms.setTransform(transforms.getInstance(field.entity),
            transforms.getWorldTransform(transforms.getInstance(haze.entity)));
        renderables.setLayerMask(renderables.getInstance(field.entity), 0xff,
            enabled_ && heat_haze_enabled_ ? 0xff : 0);
    }
    return true;
}

void ParticleFeature::render_temporal_distortion(RenderContext& ctx) {
    if (temporal_distortion_enabled_ && field_target_ && field_view_) ctx.renderer->render(field_view_);
}

filament::Texture* ParticleFeature::temporal_distortion_texture() const noexcept {
    return temporal_distortion_enabled_ ? field_texture_ : nullptr;
}

void ParticleFeature::destroy_temporal_distortion(RenderContext& ctx) {
    auto& engine = *ctx.engine;
    if (field_view_) field_view_->setRenderTarget(nullptr);
    for (auto& emitter : field_emitters_) {
        if (field_scene_) field_scene_->remove(emitter.entity);
        engine.destroy(emitter.entity);
        utils::EntityManager::get().destroy(emitter.entity);
        engine.destroy(emitter.material);
    }
    field_emitters_.clear();
    if (field_target_) engine.destroy(field_target_);
    if (field_texture_) engine.destroy(field_texture_);
    if (field_view_) engine.destroy(field_view_);
    if (field_clear_) engine.destroy(field_clear_);
    if (field_scene_) engine.destroy(field_scene_);
    if (field_material_) engine.destroy(field_material_);
    field_target_ = nullptr;
    field_texture_ = nullptr;
    field_view_ = nullptr;
    field_clear_ = nullptr;
    field_scene_ = nullptr;
    field_material_ = nullptr;
    field_width_ = field_height_ = 0;
    temporal_distortion_enabled_ = false;
}

void ParticleFeature::on_enabled_changed(RenderContext& ctx, bool on) {
    if (!built_) return;   // build_emitters honours enabled_ when it runs
    if (on && !in_scene_) {
        for (auto& se : emitters_) if (se.entity) ctx.scene->addEntity(se.entity);
        in_scene_ = true;
    } else if (!on && in_scene_) {
        for (auto& se : emitters_) if (se.entity) ctx.scene->remove(se.entity);
        in_scene_ = false;
    }
}

void ParticleFeature::destroy(RenderContext& ctx) {
    auto* engine_ = ctx.engine;
    destroy_temporal_distortion(ctx);

    // Retire heat-haze borrowers before the owning smoke VB/IB; only the
    // entity + MaterialInstance are theirs.
    for (auto& he : haze_emitters_) {
        if (he.entity) {
            if (composite_scene_) composite_scene_->remove(he.entity);
            engine_->destroy(he.entity);
            utils::EntityManager::get().destroy(he.entity);
        }
        if (he.mi) engine_->destroy(he.mi);
    }
    haze_emitters_.clear();

    // Smoke/flame/glow/firefly emitters own their VB/IB.
    for (auto& se : emitters_) {
        if (se.entity) {
            if (in_scene_) ctx.scene->remove(se.entity);
            engine_->destroy(se.entity);
            utils::EntityManager::get().destroy(se.entity);
        }
        if (se.vb) engine_->destroy(se.vb);
        if (se.ib) engine_->destroy(se.ib);
        if (se.mi) engine_->destroy(se.mi);
    }
    emitters_.clear();


    // Composite pipeline.
    if (blit_entity_) {
        if (composite_scene_) composite_scene_->remove(blit_entity_);
        engine_->destroy(blit_entity_);
        utils::EntityManager::get().destroy(blit_entity_);
        blit_entity_ = {};
    }
    if (blit_vb_) { engine_->destroy(blit_vb_); blit_vb_ = nullptr; }
    if (blit_ib_) { engine_->destroy(blit_ib_); blit_ib_ = nullptr; }
    if (blit_mi_) { engine_->destroy(blit_mi_); blit_mi_ = nullptr; }
    if (blit_material_) { engine_->destroy(blit_material_); blit_material_ = nullptr; }
    if (composite_view_)  { engine_->destroy(composite_view_); composite_view_ = nullptr; }
    if (composite_scene_) { engine_->destroy(composite_scene_); composite_scene_ = nullptr; }
    if (scene_capture_rt_ && ctx.view && ctx.view->getRenderTarget() == scene_capture_rt_) {
        ctx.view->setRenderTarget(nullptr);
    }
    if (scene_capture_rt_) { engine_->destroy(scene_capture_rt_); scene_capture_rt_ = nullptr; }
    if (scene_capture_tex_) { engine_->destroy(scene_capture_tex_); scene_capture_tex_ = nullptr; }
    if (scene_capture_depth_) { engine_->destroy(scene_capture_depth_); scene_capture_depth_ = nullptr; }

    // Materials.
    if (heat_haze_material_) { engine_->destroy(heat_haze_material_); heat_haze_material_ = nullptr; }
    if (smoke_material_)   { engine_->destroy(smoke_material_);   smoke_material_ = nullptr; }
    if (flame_material_)   { engine_->destroy(flame_material_);   flame_material_ = nullptr; }
    if (glow_material_)    { engine_->destroy(glow_material_);    glow_material_ = nullptr; }
    if (firefly_material_) { engine_->destroy(firefly_material_); firefly_material_ = nullptr; }
    // glow_tex_ is owned by the resource cache.
    glow_tex_ = nullptr;
}


void ParticleFeature::append_temporal_draws(std::vector<TemporalEffectDraw>& draws) const {
    if (!enabled_ || !in_scene_) return;
    const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR, filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    for (const auto& emitter : emitters_) {
        TemporalEffectDraw draw{.identity = emitter.entity.getId(),
            .kind = emitter.temporal_kind, .source_entity = emitter.entity,
            .vertices = emitter.vb, .indices = emitter.ib, .source = emitter.mi};
        if (draw.kind == TemporalEffectKind::Glow || draw.kind == TemporalEffectKind::Firefly)
            draw.textures[0] = {"glowTex", glow_tex_, sampler};
        draws.push_back(draw);
    }
}

} // namespace heaven
