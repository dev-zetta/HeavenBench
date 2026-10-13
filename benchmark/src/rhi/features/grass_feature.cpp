#include "benchmark/microprofiler.h"
/*
 * HeavenBench — Procedural grass feature implementation
 *
 * Moved verbatim out of FilamentBackend::init_grass / update_grass and
 * the deferred grass-generation block in render_frame(). Per-frame the
 * feature only updates the `time` uniform (GPU wind) and syncs each
 * patch's transform with its floating-island instance — no geometry
 * regeneration.
 */
#include "rhi/features/grass_feature.h"
#include "rhi/frame_generation/temporal_grass.h"
#include "rhi/render_test_scene_assets.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"
#include "rhi/grass_tangent_upload.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "engine/grass_cpu.h"
#include "engine/grass_spatial_partition.h"
#include "engine/feature_assets.h"
#include "engine/path_resolver.h"
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/Renderer.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/TextureSampler.h>
#include <filament/Texture.h>

#include <utils/EntityManager.h>

#include <math/vec4.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>

namespace heaven {

void GrassFeature::init(RenderContext& ctx, const AppConfig& config) {
    auto* engine_ = ctx.engine;

    material_ = filament::Material::Builder()
        .package(k_heaven_grass_filamat, k_heaven_grass_filamat_size)
        .build(*engine_);
    if (!material_) {
        std::printf("  Grass: material failed to load\n");
        return;
    }

    // Generation is deferred to the first update() because it needs the
    // scene instance transforms to position grass on floating islands
    // correctly. The resolver lives on RenderContext and is also available
    // at generation time, so no per-feature data_dir copy is needed.
    enabled_   = config.features.grass;
    std::printf("  Grass: deferred to first frame (needs instance transforms)%s\n",
                enabled_ ? "" : " (disabled)");

    // Load the grass depth-proxy material now (it has no per-patch state). The
    // proxies themselves are built lazily in update() once the patches and the
    // depth proxy scene both exist.
    if (ctx.test_scene_assets) {
        depth_proxy_.init(engine_, ctx.test_scene_assets->feature_texture(FeatureAsset::GrassDiffuse));
    } else {
        depth_proxy_.init(engine_, *ctx.resources, ctx.path_resolver());
    }
}

void GrassFeature::generate(RenderContext& ctx, const FrameContext& frame) {
    auto* engine_ = ctx.engine;
    auto  instances = frame.instances;
    auto  meshes    = frame.meshes;

    generated_ = true;
    ++generation_count_;

    struct GrassSource {
        FeatureAsset mesh;
        std::string surface;
        int blade_count;
        std::string id;
    };
    const std::array srcs = {
        GrassSource{
            FeatureAsset::GrassIsland1Mesh,
            "grass", render_tuning::grass.island1_blades, "island1"},
        GrassSource{
            FeatureAsset::GrassIsland2Mesh,
            "ground", render_tuning::grass.island2_blades, "island2"},
        GrassSource{
            FeatureAsset::GrassIsland3Mesh,
            "ground", render_tuning::grass.island3_blades, "island3"},
    };
    mi_ = material_->createInstance();
    material_tuning::bind_grass_visible(*mi_);

    // Bind the diffuse atlas + variation noise so the fragment
    // shader can colour each blade (the original Heaven shader
    // pulls these same two textures, see
    // extracted/core/.../grass/fragment_base_deferred.shader).
    // Loading is best-effort: if the textures aren't available
    // the material falls back to its default white sampler and
    // the result is a solid-grey grass — still better than a
    // crash, and trivially fixable by re-extracting assets.
    {
        const filament::Texture* diffuse_tex = resolve_feature_texture(
            ctx, FeatureAsset::GrassDiffuse, TextureSemantic::ColorSRGB);
        const filament::Texture* variation_tex = resolve_feature_texture(
            ctx, FeatureAsset::GrassIrregularity, TextureSemantic::ColorSRGB);

        // Fall back to the engine placeholder (white 1×1) if a
        // texture is missing so binding never fails.
        if (!diffuse_tex)   diffuse_tex   = ctx.resources->placeholder_texture();
        if (!variation_tex) variation_tex = ctx.resources->placeholder_texture();

        filament::TextureSampler diffuse_sampler(
            filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            filament::TextureSampler::MagFilter::LINEAR,
            filament::TextureSampler::WrapMode::REPEAT);
        filament::TextureSampler variation_sampler(
            filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            filament::TextureSampler::MagFilter::LINEAR,
            filament::TextureSampler::WrapMode::REPEAT);

        mi_->setParameter("grassDiffuse",   diffuse_tex,   diffuse_sampler);
        diffuse_texture_ = diffuse_tex;
        mi_->setParameter("grassVariation", variation_tex, variation_sampler);

        // Tuned values: variation noise tiles roughly every 20m
        // on the ground (scale 0.05) and modulates colour by
        // ±25% — strong enough to break the "carpet" look,
        // gentle enough that the patch keeps a coherent hue.
        mi_->setParameter(
            "variationScale", render_tuning::grass.variation_scale);
        mi_->setParameter(
            "variationStrength", render_tuning::grass.variation_strength);
        mi_->setParameter(
            "grassDiffuseUv",
            filament::math::float4{
                render_tuning::grass.grass_diffuse_uv.x,
                render_tuning::grass.grass_diffuse_uv.y,
                render_tuning::grass.grass_diffuse_uv.z,
                render_tuning::grass.grass_diffuse_uv.w});
        mi_->setParameter("time", 0.0f);
    }
    std::size_t total = 0;
    const auto add_patch = [&](const std::vector<GrassRoot>& roots,
                               std::size_t instance_index) {
        if (roots.empty()) return;
        const std::size_t vertex_count =
            roots.size() * static_cast<std::size_t>(GRASS_VERTS_PER_BLADE);
        if (vertex_count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("grass patch exceeds Filament's vertex limit");
        }
        if (instance_index >= instances.size()) {
            throw std::logic_error("procedural grass references an absent scene instance");
        }
        const auto& instance = instances[instance_index];
        const auto verts = generate_grass_blades(roots, 0.0f);
        auto partition = partition_grass_geometry(verts, GRASS_VERTS_PER_BLADE);
        if (!partition) {
            std::printf("  Grass: %s; keeping one uncropped draw\n",
                        partition.error().message.c_str());
            partition = GrassSpatialPartition{};
            partition->indices.resize(verts.size());
            std::iota(partition->indices.begin(), partition->indices.end(), 0u);
            partition->ranges.push_back({0, static_cast<std::uint32_t>(verts.size()),
                                         grass_geometry_bounds(verts)});
        }
        GrassGeometry geometry;
        geometry.vb = filament::VertexBuffer::Builder()
            .vertexCount(static_cast<std::uint32_t>(vertex_count))
            .bufferCount(2)
            .attribute(
                filament::VertexAttribute::POSITION, 0,
                filament::VertexBuffer::AttributeType::FLOAT3,
                offsetof(GrassVertex, px), sizeof(GrassVertex))
            .attribute(
                filament::VertexAttribute::UV0, 0,
                filament::VertexBuffer::AttributeType::FLOAT2,
                offsetof(GrassVertex, u), sizeof(GrassVertex))
            .attribute(
                filament::VertexAttribute::COLOR, 0,
                filament::VertexBuffer::AttributeType::FLOAT4,
                offsetof(GrassVertex, r), sizeof(GrassVertex))
            .attribute(
                filament::VertexAttribute::TANGENTS, 1,
                filament::VertexBuffer::AttributeType::SHORT4, 0, 0)
            .normalized(filament::VertexAttribute::TANGENTS)
            .build(*engine_);
        if (!geometry.vb) throw std::runtime_error("Grass vertex buffer could not be built");
        geometry.ib = filament::IndexBuffer::Builder()
            .indexCount(static_cast<std::uint32_t>(vertex_count))
            .bufferType(filament::IndexBuffer::IndexType::UINT)
            .build(*engine_);
        if (!geometry.ib) {
            engine_->destroy(geometry.vb);
            throw std::runtime_error("Grass index buffer could not be built");
        }
        geometries_.push_back(geometry);
        auto indices =
            std::make_unique<std::uint32_t[]>(vertex_count);
        std::ranges::copy(partition->indices, indices.get());
        geometry.ib->setBuffer(
            *engine_,
            filament::IndexBuffer::BufferDescriptor(
                indices.release(),
                vertex_count * sizeof(std::uint32_t),
                [](void* buffer, std::size_t, void*) {
                    delete[] static_cast<std::uint32_t*>(buffer);
                }));
        const std::size_t size = verts.size() * sizeof(GrassVertex);
        auto buffer = std::make_unique<std::uint8_t[]>(size);
        std::memcpy(buffer.get(), verts.data(), size);
        geometry.vb->setBufferAt(*engine_, 0,
            filament::VertexBuffer::BufferDescriptor(
                buffer.release(), size,
                [](void* memory, std::size_t, void*) {
                    delete[] static_cast<std::uint8_t*>(memory);
                }));
        upload_grass_tangent_frames(*engine_, *geometry.vb, verts);
        for (const auto& range : partition->ranges) {
            GrassPatch patch;
            patch.instance_index = instance_index;
            patch.vertex_count = range.index_count;
            patch.index_offset = range.index_offset;
            patch.vb = geometry.vb;
            patch.ib = geometry.ib;
            patch.bounds.geometry = range.geometry;
            patch.bounds.update(instance.transform);
            patch.entity = utils::EntityManager::get().create();
            const auto built = filament::RenderableManager::Builder(1)
                .geometry(
                    0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                    patch.vb, patch.ib, patch.index_offset, patch.vertex_count)
            // Dense blades receive scene shadows, but casting every masked
            // ribbon into all sun cascades makes the meadow behave like an
            // opaque canopy and self-occlude to black.
            .material(0, mi_)
            .castShadows(false)
            .receiveShadows(true)
            .culling(patch.bounds.local.has_value())
            .boundingBox(patch.bounds.box())
            .build(*engine_, patch.entity);
            if (built != filament::RenderableManager::Builder::Success) {
                engine_->destroy(patch.entity);
                utils::EntityManager::get().destroy(patch.entity);
                throw std::runtime_error("Grass spatial range could not be built");
            }
            auto& transform_manager = engine_->getTransformManager();
            transform_manager.create(patch.entity);
            transform_manager.setTransform(
                transform_manager.getInstance(patch.entity),
                glm_to_filament(instance.transform));
            if (enabled_) ctx.scene->addEntity(patch.entity);
            patch.bounds.log("grass", patch.instance_index, patch.vertex_count,
                             vertex_count, patch.index_offset);
            patches_.push_back(patch);
        }
        total += roots.size();
    };
    if (ctx.test_scene_assets) {
        const auto patches = ctx.test_scene_assets->grass_patches();
        if (patches.empty()) {
            throw std::logic_error("grass render test has no explicit ground roots");
        }
        for (const auto& patch : patches) add_patch(patch.roots, patch.instance_index);
    } else {
        for (const GrassSource& source : srcs) {
            auto roots = generate_grass_roots(
                feature_asset_path(ctx.path_resolver(), source.mesh),
                source.blade_count, source.surface, ctx.assets);
            if (roots.empty()) continue;
            const std::size_t vertex_count =
                roots.size() * static_cast<std::size_t>(GRASS_VERTS_PER_BLADE);
            if (vertex_count > std::numeric_limits<std::uint32_t>::max()) {
                std::fprintf(stderr, "Grass: vertex count exceeds Filament's limit\n");
                continue;
            }
            for (std::size_t instance_index = 0;
                 instance_index < instances.size(); ++instance_index) {
                const auto& instance = instances[instance_index];
                if (instance.mesh_index < 0 ||
                    static_cast<std::size_t>(instance.mesh_index) >= meshes.size()) continue;
                const auto& surfaces = meshes[static_cast<std::size_t>(instance.mesh_index)].surfaces;
                if (std::ranges::any_of(surfaces, [&](const GpuSurface& surface) {
                        return surface.name.contains(source.id);
                    })) add_patch(roots, instance_index);
            }
        }
    }
    in_scene_ = enabled_;
    std::printf("  Grass: %zu blades across %zu patches\n",
                total, patches_.size());

}

void GrassFeature::on_enabled_changed(RenderContext& ctx, bool on) {
    if (!generated_) return;   // generate() honours enabled_ when it runs
    for (size_t i = 0; i < patches_.size(); ++i) {
        const auto& patch = patches_[i];
        const bool visible = on && patch.lod_visible;
        if (patch.entity) {
            if (visible) ctx.scene->addEntity(patch.entity);
            else ctx.scene->remove(patch.entity);
        }
        if (depth_proxy_built_) depth_proxy_.set_visible(i, visible);
    }
    in_scene_ = on;
}

void GrassFeature::update(RenderContext& ctx, const FrameContext& frame) {
    // Deferred one-time build once the scene instances exist.
    if (!generated_) {
        if (frame.instances.empty() || frame.meshes.empty()) return;
        generate(ctx, frame);
    }

    auto& tcm = ctx.engine->getTransformManager();

    // Build the depth proxies once both the patches and the depth proxy scene
    // exist (the depth pass only provides the scene when an enabled consumer
    // needs depth). They share each patch's VB/IB, so there is no extra
    // geometry and no per-frame regeneration.
    if (!depth_proxy_built_ && frame.depth_proxy_scene && depth_proxy_.ready()) {
        for (auto& patch : patches_) {
            if (!patch.vb || !patch.ib) continue;
            glm::mat4 xf(1.0f);
            if (patch.instance_index < frame.instances.size()) {
                xf = frame.instances[patch.instance_index].transform;
            }
            const size_t proxy_index = depth_proxy_.size();
            depth_proxy_.add_patch(ctx.engine, frame.depth_proxy_scene,
                                   patch.vb, patch.ib, patch.index_offset,
                                   patch.vertex_count, xf, patch.bounds);
            if (depth_proxy_.size() > proxy_index) {
                depth_proxy_.set_visible(proxy_index, enabled_ && patch.lod_visible);
            }
        }
        depth_proxy_built_ = true;
    }

    // Sync grass patch transforms with their island instances so the
    // blades bob with the floating-island animation.
    for (std::size_t i = 0; i < patches_.size(); ++i) {
        auto& patch = patches_[i];
        if (patch.instance_index >= frame.instances.size()) {
            continue;
        }
        const auto& inst = frame.instances[patch.instance_index];
        if (patch.bounds.update(inst.transform)) {
            patch.bounds.apply(ctx.engine->getRenderableManager(), patch.entity);
            if (depth_proxy_built_) depth_proxy_.set_bounds(ctx.engine, i, patch.bounds);
        }
        const glm::vec3 island_pos = glm::vec3(inst.transform[3]);
        const glm::vec3 to_camera = island_pos - frame.camera->position;
        const bool lod_visible =
            glm::dot(to_camera, to_camera) <=
            render_tuning::grass.lod_distance *
                render_tuning::grass.lod_distance;
        if (lod_visible != patch.lod_visible) {
            if (enabled_ && lod_visible) ctx.scene->addEntity(patch.entity);
            else             ctx.scene->remove(patch.entity);
            patch.lod_visible = lod_visible;
            if (depth_proxy_built_) {
                depth_proxy_.set_visible(i, enabled_ && lod_visible);
            }
        }
        auto ti = tcm.getInstance(patch.entity);
        if (ti) {
            tcm.setTransform(ti, glm_to_filament(inst.transform));
        }
        // Keep the matching depth proxy aligned with the visible patch.
        if (depth_proxy_built_) {
            depth_proxy_.set_transform(ctx.engine, i, inst.transform);
        }
    }

    // Wind animation is GPU-driven: only the `time` uniform changes per frame.
    // Filament's lit material consumes scene lights, IBL, and shadows directly.
    if (mi_) {
        mi_->setParameter("time", frame.time);
    }
    // Drive the depth proxies' wind from the same clock so their silhouette
    // tracks the swaying blades.
    if (depth_proxy_built_) {
        depth_proxy_.set_time(frame.time);
    }
    if (auto* recorder = profile::recorder(); recorder && recorder->active()) {
        recorder->counter("GrassFeature ranges total", static_cast<double>(patches_.size()), "Grass workload");
        const auto enabled_ranges = std::count_if(patches_.begin(), patches_.end(), [](const auto& patch) { return patch.lod_visible; });
        recorder->counter("GrassFeature distance-enabled ranges", static_cast<double>(enabled_ranges), "Grass workload");
    }
}

void GrassFeature::append_temporal_patches(std::vector<TemporalGrassPatch>& output) const {
    if (!mi_) return;
    const auto uv = mi_->getParameter<filament::math::float4>("grassDiffuseUv");
    const auto wind = mi_->getParameter<filament::math::float4>("grassWindTuning");
    for (const auto& patch : patches_) {
        output.push_back({patch.entity, patch.vb, patch.ib, diffuse_texture_, patch.instance_index,
            patch.index_offset, patch.vertex_count, patch.bounds.box(),
            {uv.x, uv.y, uv.z, uv.w}, {wind.x, wind.y, wind.z, wind.w},
            patch.bounds.local.has_value(), enabled_ && patch.lod_visible});
    }
}

void GrassFeature::destroy(RenderContext& ctx) {
    auto* engine_ = ctx.engine;
    // Depth proxies first (they live in the depth pre-pass scene, which is torn
    // down after the features — see FilamentBackend dtor ordering).
    depth_proxy_.destroy(engine_);
    for (auto& patch : patches_) {
        if (patch.entity) {
            if (in_scene_) ctx.scene->remove(patch.entity);
            engine_->destroy(patch.entity);
            utils::EntityManager::get().destroy(patch.entity);
        }
    }
    patches_.clear();
    for (const auto& geometry : geometries_) {
        if (geometry.vb) engine_->destroy(geometry.vb);
        if (geometry.ib) engine_->destroy(geometry.ib);
    }
    geometries_.clear();
    if (mi_) { engine_->destroy(mi_); mi_ = nullptr; }
    diffuse_texture_ = nullptr;
    if (material_) { engine_->destroy(material_); material_ = nullptr; }
}

} // namespace heaven
