#include "benchmark/microprofiler.h"
/*
 * HeavenBench — Massive foliage and GPU-driven geometry feature
 *
 * See massive_foliage_feature.h for the implementation. Geometry is built ONCE
 * (deferred to the first frame with scene instances) and animated through
 * the heaven_grass.mat vertex shader thereafter; the per-frame path only
 * pushes the `time` uniform and syncs each patch's transform with its
 * floating island. No per-frame geometry regeneration (AGENTS.md).
 */
#include "rhi/features/massive_foliage_feature.h"
#include "rhi/frame_generation/temporal_grass.h"
#include "rhi/render_test_scene_assets.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"
#include "rhi/grass_tangent_upload.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "engine/grass_cpu.h"
#include "engine/feature_assets.h"
#include "engine/path_resolver.h"
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/TextureSampler.h>
#include <filament/Texture.h>

#include <utils/EntityManager.h>

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace heaven {

// Foliage source islands, mirroring GrassFeature's per-island patches. The
// blade budget per island below is the Shipping_Look baseline distribution
// (~466k total grass blades); the scaled build multiplies the total by the
// density and re-derives the per-island share, so the relative density of
// each island is preserved as the multiplier grows.
namespace {
struct FoliageSource {
    FeatureAsset mesh;       // extracted mesh sampled for blade roots
    const char* surface;     // surface-name filter for root sampling
    int         baseline;    // baseline blade share for this island
    const char* id;          // instance-mesh surface marker
};

const FoliageSource k_sources[] = {
    {FeatureAsset::GrassIsland1Mesh, "grass",  render_tuning::grass.island1_blades, "island1"},
    {FeatureAsset::GrassIsland2Mesh, "ground", render_tuning::grass.island2_blades, "island2"},
    {FeatureAsset::GrassIsland3Mesh, "ground", render_tuning::grass.island3_blades, "island3"},
};
constexpr int k_baseline_total =
    render_tuning::grass.island1_blades +
    render_tuning::grass.island2_blades +
    render_tuning::grass.island3_blades;

// Flower clutter is a small fraction of the grass count — enough to read as a
// distinct ground-clutter category without doubling the geometry cost.
constexpr float k_flower_fraction = render_tuning::grass.flower_fraction;
constexpr std::uint32_t k_flower_root_seed = 0xF10A3u;

// Bind the selected diffuse atlas plus the shared variation texture. The UV
// transform lets grass use its full texture while flower cards address one
// authored plant inside grass_variation_d.dds without changing wind height.
const filament::Texture* bind_grass_textures(RenderContext& ctx, filament::MaterialInstance& mi,
                         FeatureAsset diffuse_asset,
                         const render_tuning::Float4& diffuse_uv) {
    const filament::Texture* diffuse_tex = resolve_feature_texture(
        ctx, diffuse_asset, TextureSemantic::ColorSRGB);
    const filament::Texture* variation_tex = resolve_feature_texture(
        ctx, FeatureAsset::GrassIrregularity, TextureSemantic::ColorSRGB);
    if (!diffuse_tex)   diffuse_tex   = ctx.resources->placeholder_texture();
    if (!variation_tex) variation_tex = ctx.resources->placeholder_texture();

    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);

    mi.setParameter("grassDiffuse",   diffuse_tex,   sampler);
    mi.setParameter("grassVariation", variation_tex, sampler);
    mi.setParameter("variationScale", render_tuning::grass.variation_scale);
    mi.setParameter(
        "variationStrength", render_tuning::grass.variation_strength);
    mi.setParameter(
        "grassDiffuseUv",
        filament::math::float4{
            diffuse_uv.x, diffuse_uv.y, diffuse_uv.z, diffuse_uv.w});
    mi.setParameter("time", 0.0f);
    return diffuse_tex;
}
} // namespace

void MassiveFoliageFeature::init(RenderContext& ctx, const AppConfig& config) {
    auto* engine_ = ctx.engine;

    material_ = filament::Material::Builder()
        .package(k_heaven_grass_filamat, k_heaven_grass_filamat_size)
        .build(*engine_);
    if (!material_) {
        std::printf("  MassiveFoliage: grass material failed to load\n");
        return;
    }

    // Build is deferred to the first update() — it needs the scene instance
    // transforms to place foliage on the floating islands (GrassFeature model).
    // The PathResolver on RenderContext is available at both init and build
    // time, so no per-feature data_dir copy is needed.
    quality_  = config.quality;
    enabled_  = config.features.massive_foliage;
    std::printf("  MassiveFoliage: deferred to first frame (needs instance transforms)%s\n",
                enabled_ ? "" : " (disabled)");

    // Grass depth-proxy material (shared atlas + time, no per-patch state). The
    // proxies are built lazily in update() once the patches + depth proxy scene
    // exist, so the foliage occludes the screen-space depth consumers.
    if (ctx.test_scene_assets) {
        depth_proxy_.init(engine_, ctx.test_scene_assets->feature_texture(FeatureAsset::GrassDiffuse));
    } else {
        depth_proxy_.init(engine_, *ctx.resources, ctx.path_resolver());
    }
}

bool MassiveFoliageFeature::make_patch(RenderContext& ctx, const FrameContext& frame,
                                       int instance_index, Category category,
                                       size_t geometry_index,
                                       filament::MaterialInstance& material_instance) {
    if (geometry_index >= geometries_.size()) return false;
    auto* engine_ = ctx.engine;
    const auto& geometry = geometries_[geometry_index];

    const glm::mat4 transform =
        instance_index >= 0 && std::cmp_less(instance_index, frame.instances.size())
            ? frame.instances[instance_index].transform : glm::mat4(1.0f);
    const size_t first_patch = patches_.size();
    for (const auto& range : geometry.ranges) {
        FoliagePatch gp;
        gp.instance_index = instance_index;
        gp.category = category;
        gp.vb = geometry.vb;
        gp.ib = geometry.ib;
        gp.vertex_count = static_cast<int>(range.index_count);
        gp.index_offset = range.index_offset;
        gp.bounds.geometry = range.geometry;
        gp.bounds.update(transform);
        gp.entity = utils::EntityManager::get().create();
        const auto built = filament::RenderableManager::Builder(1)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                      gp.vb, gp.ib, gp.index_offset, range.index_count)
            // Keep tree/building shadow reception without turning millions of
            // overlapping blades into an opaque cascade-shadow caster.
            .material(0, &material_instance)
            .castShadows(false).receiveShadows(true).culling(gp.bounds.local.has_value())
            .boundingBox(gp.bounds.box()).build(*engine_, gp.entity);
        if (built != filament::RenderableManager::Builder::Success) {
            engine_->destroy(gp.entity);
            utils::EntityManager::get().destroy(gp.entity);
            // Never leave a partial island if any range could not be built.
            while (patches_.size() > first_patch) {
                const auto entity = patches_.back().entity;
                ctx.scene->remove(entity);
                engine_->destroy(entity);
                utils::EntityManager::get().destroy(entity);
                patches_.pop_back();
            }
            return false;
        }
        auto& tcm = engine_->getTransformManager();
        tcm.create(gp.entity);
        tcm.setTransform(tcm.getInstance(gp.entity), glm_to_filament(transform));
        if (enabled_) ctx.scene->addEntity(gp.entity);
        gp.bounds.log(category == Category::Flower ? "flower" : "massive-grass",
                      static_cast<size_t>(instance_index), static_cast<size_t>(gp.vertex_count),
                      static_cast<size_t>(geometry.vertex_count), gp.index_offset);
        patches_.push_back(gp);
    }
    return true;
}

std::optional<size_t> MassiveFoliageFeature::upload_geometry(
        RenderContext& ctx, const std::vector<GrassRoot>& roots, Category category) {
    const size_t vertices_per_root = category == Category::Flower
        ? FLOWER_VERTS_PER_CARD : GRASS_VERTS_PER_BLADE;
    if (roots.empty() || roots.size() >
        static_cast<size_t>(std::numeric_limits<int>::max()) / vertices_per_root) {
        return std::nullopt;
    }
    auto* engine_ = ctx.engine;
    auto verts = category == Category::Flower
        ? generate_flower_cards(roots)
        : generate_grass_blades(roots, 0.0f);
    FoliageGeometry geometry;
    geometry.vertex_count = static_cast<int>(verts.size());
    auto partition = partition_grass_geometry(verts, vertices_per_root);
    if (!partition) {
        std::printf("  MassiveFoliage: %s; keeping one uncropped draw\n",
                    partition.error().message.c_str());
        partition = GrassSpatialPartition{};
        partition->indices.resize(verts.size());
        for (size_t i = 0; i < verts.size(); ++i)
            partition->indices[i] = static_cast<std::uint32_t>(i);
        partition->ranges.push_back({0, static_cast<std::uint32_t>(verts.size()),
                                     grass_geometry_bounds(verts)});
    }
    geometry.ranges = std::move(partition->ranges);
    geometry.vb = filament::VertexBuffer::Builder().vertexCount(geometry.vertex_count).bufferCount(2)
        .attribute(filament::VertexAttribute::POSITION, 0, filament::VertexBuffer::AttributeType::FLOAT3, offsetof(GrassVertex, px), sizeof(GrassVertex))
        .attribute(filament::VertexAttribute::UV0, 0, filament::VertexBuffer::AttributeType::FLOAT2, offsetof(GrassVertex, u), sizeof(GrassVertex))
        .attribute(filament::VertexAttribute::COLOR, 0, filament::VertexBuffer::AttributeType::FLOAT4, offsetof(GrassVertex, r), sizeof(GrassVertex))
        .attribute(filament::VertexAttribute::TANGENTS, 1, filament::VertexBuffer::AttributeType::SHORT4, 0, 0)
        .normalized(filament::VertexAttribute::TANGENTS).build(*engine_);
    if (!geometry.vb) return std::nullopt;
    geometry.ib = filament::IndexBuffer::Builder().indexCount(geometry.vertex_count)
        .bufferType(filament::IndexBuffer::IndexType::UINT).build(*engine_);
    if (!geometry.ib) {
        engine_->destroy(geometry.vb);
        return std::nullopt;
    }
    // Store ownership before queuing uploads. Patches and depth proxies only
    // borrow the pointers, and teardown destroys every borrower first.
    const size_t index = geometries_.size();
    geometries_.push_back(std::move(geometry));
    {
        auto indices = std::make_unique<uint32_t[]>(verts.size());
        std::ranges::copy(partition->indices, indices.get());
        geometries_[index].ib->setBuffer(*engine_, filament::IndexBuffer::BufferDescriptor(
            indices.release(), verts.size() * sizeof(uint32_t),
            [](void* b, size_t, void*) { delete[] static_cast<uint32_t*>(b); }));
    }
    size_t size = verts.size() * sizeof(GrassVertex);
    auto buffer = std::make_unique<uint8_t[]>(size);
    std::memcpy(buffer.get(), verts.data(), size);
    geometries_[index].vb->setBufferAt(*engine_, 0, filament::VertexBuffer::BufferDescriptor(
        buffer.release(), size,
        [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
    upload_grass_tangent_frames(*engine_, *geometries_[index].vb, verts);
    return index;
}

void MassiveFoliageFeature::build_grass(RenderContext& ctx, const FrameContext& frame,
                                        int target_total_blades) {
    auto instances = frame.instances;
    auto meshes    = frame.meshes;

    if (!grass_mi_) {
        grass_mi_ = material_->createInstance();
        if (!grass_mi_) return;
        material_tuning::bind_grass_visible(*grass_mi_);
        grass_diffuse_texture_ = bind_grass_textures(
            ctx, *grass_mi_, FeatureAsset::GrassDiffuse,
            render_tuning::grass.grass_diffuse_uv);
    }

    if (ctx.test_scene_assets) {
        const auto patches = ctx.test_scene_assets->grass_patches();
        if (patches.empty()) {
            throw std::logic_error("massive foliage render test has no explicit ground roots");
        }
        std::size_t total = 0;
        for (const auto& patch : patches) {
            if (patch.instance_index >= instances.size() ||
                patch.instance_index > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::logic_error("procedural foliage references an absent scene instance");
            }
            const auto geometry = upload_geometry(ctx, patch.roots, Category::Grass);
            if (!geometry || !make_patch(ctx, frame, static_cast<int>(patch.instance_index),
                                        Category::Grass, *geometry, *grass_mi_)) {
                throw std::runtime_error("procedural massive grass could not be built");
            }
            total += patch.roots.size();
        }
        std::printf("  MassiveFoliage: %zu explicit procedural grass roots\n", total);
        return;
    }
    int total = 0;
    for (const auto& s : k_sources) {
        // Per-island blade share scaled to hit the requested grand total while
        // preserving each island's relative density.
        const int island_count = static_cast<int>(std::lround(
            static_cast<double>(target_total_blades) *
            (static_cast<double>(s.baseline) / static_cast<double>(k_baseline_total))));
        if (island_count <= 0) continue;

        auto roots = generate_grass_roots(
            feature_asset_path(ctx.path_resolver(), s.mesh), island_count,
            s.surface, ctx.assets);
        if (roots.empty()) continue;

        std::optional<size_t> geometry_index;
        for (int ii = 0; std::cmp_less(ii, instances.size()); ii++) {
            const auto& inst = instances[ii];
            if (inst.mesh_index < 0 || std::cmp_greater_equal(inst.mesh_index, meshes.size())) continue;
            bool match = false;
            for (auto& sf : meshes[inst.mesh_index].surfaces)
                if (sf.name.find(s.id) != std::string::npos) { match = true; break; }
            if (!match) continue;
            if (!geometry_index) geometry_index = upload_geometry(ctx, roots, Category::Grass);
            if (!geometry_index) continue;
            if (make_patch(
                    ctx, frame, ii, Category::Grass, *geometry_index, *grass_mi_))
                total += static_cast<int>(roots.size());
        }
    }
    std::printf("  MassiveFoliage: %d grass blades built (target %d)\n",
                total, target_total_blades);
}

void MassiveFoliageFeature::build_flowers(RenderContext& ctx, const FrameContext& frame,
                                          int target_flower_count) {
    if (target_flower_count <= 0) return;
    auto instances = frame.instances;
    auto meshes    = frame.meshes;

    if (!flower_mi_) {
        flower_mi_ = material_->createInstance();
        if (!flower_mi_) return;
        material_tuning::bind_grass_visible(*flower_mi_);
        flower_diffuse_texture_ = bind_grass_textures(
            ctx, *flower_mi_, FeatureAsset::GrassClutter,
            render_tuning::grass.flower_diffuse_uv);
        // A warm tint nudges the flower clutter away from the grass green so
        // the new category is visually distinct from the Shipping_Look.
        flower_mi_->setParameter(
            "variationStrength",
            render_tuning::grass.flower_variation_strength);
        const auto flower_tint = render_tuning::grass.flower_tint_shape;
        flower_mi_->setParameter(
            "flowerTintTuning",
            filament::math::float4{
                flower_tint.x, flower_tint.y,
                flower_tint.z, flower_tint.w});
    }

    if (ctx.test_scene_assets) {
        std::size_t total = 0;
        for (const auto& patch : ctx.test_scene_assets->grass_patches()) {
            if (patch.flowers.empty()) continue;
            if (patch.instance_index >= instances.size() ||
                patch.instance_index > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::logic_error("procedural flowers reference an absent scene instance");
            }
            const auto geometry = upload_geometry(ctx, patch.flowers, Category::Flower);
            if (!geometry || !make_patch(ctx, frame, static_cast<int>(patch.instance_index),
                                        Category::Flower, *geometry, *flower_mi_)) {
                throw std::runtime_error("procedural flower cards could not be built");
            }
            total += patch.flowers.size();
        }
        std::printf("  MassiveFoliage: %zu explicit procedural flower cards\n", total);
        return;
    }
    int total = 0;
    for (const auto& s : k_sources) {
        const int island_count = static_cast<int>(std::lround(
            static_cast<double>(target_flower_count) *
            (static_cast<double>(s.baseline) / static_cast<double>(k_baseline_total))));
        if (island_count <= 0) continue;

        // A category-specific deterministic seed keeps flowers on the authored
        // ground surfaces without planting them at the same roots as grass.
        auto roots = generate_grass_roots(
            feature_asset_path(ctx.path_resolver(), s.mesh), island_count,
            s.surface, ctx.assets, k_flower_root_seed);
        if (roots.empty()) continue;

        std::optional<size_t> geometry_index;
        for (int ii = 0; std::cmp_less(ii, instances.size()); ii++) {
            const auto& inst = instances[ii];
            if (inst.mesh_index < 0 || std::cmp_greater_equal(inst.mesh_index, meshes.size())) continue;
            bool match = false;
            for (auto& sf : meshes[inst.mesh_index].surfaces)
                if (sf.name.find(s.id) != std::string::npos) { match = true; break; }
            if (!match) continue;
            if (!geometry_index) geometry_index = upload_geometry(ctx, roots, Category::Flower);
            if (!geometry_index) continue;
            if (make_patch(
                    ctx, frame, ii, Category::Flower, *geometry_index, *flower_mi_))
                total += static_cast<int>(roots.size());
        }
    }
    std::printf("  MassiveFoliage: %d authored flower cards built\n", total);
}

Result<void> MassiveFoliageFeature::build_foliage(RenderContext& ctx, const FrameContext& frame) {
    // Scaled blade count from the foliage density knob, capped at the
    // configured maximum.
    const int target_blades = foliage_blade_count(quality_.foliage_density, quality_);

    // Build-failure seam: if the requested blade count cannot be
    // built, return an error so generate() falls back to the baseline.
    if (!can_build_foliage(target_blades)) {
        return std::unexpected(Error{
            "foliage build failed at " + std::to_string(target_blades) + " blades"});
    }

    build_grass(ctx, frame, target_blades);
    build_flowers(ctx, frame,
                  static_cast<int>(std::lround(static_cast<float>(target_blades) * k_flower_fraction)));
    return {};
}

void MassiveFoliageFeature::generate(RenderContext& ctx, const FrameContext& frame) {
    generated_ = true;
    ++generation_count_;
    geometries_.reserve(std::size(k_sources) * 2);

    auto result = build_foliage(ctx, frame);
    if (!result) {
        // fall back to the Shipping_Look grass baseline, emit a
        // diagnostic, and continue rendering. The baseline build uses density
        // 1.0 (the ~466k-blade shipping count) with no flowers and no raised
        // terrain overlay, so the scene degrades to exactly the grass baseline.
        std::printf("  MassiveFoliage: %s — falling back to Shipping_Look grass baseline\n",
                    result.error().message.c_str());
        build_grass(ctx, frame, quality_.foliage_blade_baseline);
    }

    in_scene_ = enabled_;
    const auto stats = geometry_stats();
    constexpr size_t bytes_per_vertex = sizeof(GrassVertex) +
        sizeof(filament::math::short4) + sizeof(uint32_t);
    const size_t saved_vertices = stats.instanced_vertices > stats.uploaded_vertices
        ? stats.instanced_vertices - stats.uploaded_vertices : 0;
    std::printf("  MassiveFoliage buffers: %zu source/category owners, %zu source ranges, %zu patches, "
                "%zu uploaded vertices / %zu instantiated vertices, "
                "%zu bytes uploaded (%zu bytes saved by sharing)\n",
                stats.geometry_buffers, stats.geometry_ranges, stats.patches,
                stats.uploaded_vertices, stats.instanced_vertices,
                stats.uploaded_vertices * bytes_per_vertex,
                saved_vertices * bytes_per_vertex);
}

MassiveFoliageFeature::GeometryStats MassiveFoliageFeature::geometry_stats() const {
    GeometryStats stats;
    stats.geometry_buffers = geometries_.size();
    stats.patches = patches_.size();
    for (const auto& geometry : geometries_) {
        stats.uploaded_vertices += static_cast<size_t>(geometry.vertex_count);
        stats.geometry_ranges += geometry.ranges.size();
        for (const auto& range : geometry.ranges)
            stats.largest_range_vertices = std::max(
                stats.largest_range_vertices, static_cast<size_t>(range.index_count));
    }
    for (const auto& patch : patches_) {
        stats.instanced_vertices += static_cast<size_t>(patch.vertex_count);
        const auto owners = std::count_if(geometries_.begin(), geometries_.end(),
            [&](const FoliageGeometry& geometry) {
                return geometry.vb == patch.vb && geometry.ib == patch.ib &&
                    std::ranges::any_of(geometry.ranges, [&](const GrassDrawRange& range) {
                        return range.index_offset == patch.index_offset &&
                               std::cmp_equal(range.index_count, patch.vertex_count);
                    });
            });
        stats.shared_bindings_valid &= owners == 1;
    }
    return stats;
}

void MassiveFoliageFeature::on_enabled_changed(RenderContext& ctx, bool on) {
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

void MassiveFoliageFeature::update(RenderContext& ctx, const FrameContext& frame) {
    // Deferred one-time build once the scene instances exist.
    if (!generated_) {
        if (frame.instances.empty() || frame.meshes.empty()) return;
        generate(ctx, frame);
    }
    auto& tcm = ctx.engine->getTransformManager();

    // Build depth proxies once the patches + the depth proxy scene exist (the
    // depth pass only provides the scene when an enabled consumer needs depth).
    // One proxy per patch, in patches_ order, sharing each patch's VB/IB.
    if (!depth_proxy_built_ && frame.depth_proxy_scene && depth_proxy_.ready()) {
        for (auto& patch : patches_) {
            if (!patch.vb || !patch.ib) continue;
            glm::mat4 xf(1.0f);
            if (patch.instance_index >= 0 &&
                std::cmp_less(patch.instance_index, frame.instances.size())) {
                xf = frame.instances[patch.instance_index].transform;
            }
            const size_t proxy_index = depth_proxy_.size();
            depth_proxy_.add_patch(ctx.engine, frame.depth_proxy_scene,
                                   patch.vb, patch.ib, patch.index_offset,
                                   static_cast<std::uint32_t>(patch.vertex_count), xf, patch.bounds);
            if (depth_proxy_.size() > proxy_index) {
                depth_proxy_.set_visible(proxy_index, enabled_ && patch.lod_visible);
            }
        }
        depth_proxy_built_ = true;
    }

    // Sync each patch's transform with its floating island so foliage bobs
    // with the island animation (no geometry regeneration).
    for (size_t i = 0; i < patches_.size(); ++i) {
        auto& patch = patches_[i];
        if (patch.instance_index < 0 ||
            std::cmp_greater_equal(patch.instance_index, frame.instances.size())) continue;
        const auto& xf = frame.instances[patch.instance_index].transform;
        if (patch.bounds.update(xf)) {
            patch.bounds.apply(ctx.engine->getRenderableManager(), patch.entity);
            if (depth_proxy_built_) depth_proxy_.set_bounds(ctx.engine, i, patch.bounds);
        }
        const glm::vec3 island_pos = glm::vec3(xf[3]);
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
        if (ti) tcm.setTransform(ti, glm_to_filament(xf));
        // Keep the matching depth proxy aligned with the visible patch.
        if (depth_proxy_built_) {
            depth_proxy_.set_transform(ctx.engine, i, xf);
        }
    }
    // Wind animation is GPU-driven: only the `time` uniform changes per frame,
    // sourced from the frame-rate-independent Frame_Time_Source.
    // Filament evaluates the live scene lighting and shadows.
    if (grass_mi_)  {
        grass_mi_->setParameter("time", frame.time);
    }
    if (flower_mi_) {
        flower_mi_->setParameter("time", frame.time);
    }
    // Drive the depth proxies' wind from the same clock.
    if (depth_proxy_built_) {
        depth_proxy_.set_time(frame.time);
    }
    if (auto* recorder = profile::recorder(); recorder && recorder->active()) {
        recorder->counter("MassiveFoliageFeature ranges total", static_cast<double>(patches_.size()), "Grass workload");
        const auto enabled_ranges = std::count_if(patches_.begin(), patches_.end(), [](const auto& patch) { return patch.lod_visible; });
        recorder->counter("MassiveFoliageFeature distance-enabled ranges", static_cast<double>(enabled_ranges), "Grass workload");
    }
}

void MassiveFoliageFeature::append_temporal_patches(std::vector<TemporalGrassPatch>& output) const {
    for (const auto& patch : patches_) {
        const auto* mi = patch.category == Category::Grass ? grass_mi_ : flower_mi_;
        if (!mi || patch.instance_index < 0) continue;
        const auto uv = mi->getParameter<filament::math::float4>("grassDiffuseUv");
        const auto wind = mi->getParameter<filament::math::float4>("grassWindTuning");
        const auto* diffuse = patch.category == Category::Grass
            ? grass_diffuse_texture_ : flower_diffuse_texture_;
        output.push_back({patch.entity, patch.vb, patch.ib, diffuse,
            static_cast<size_t>(patch.instance_index), patch.index_offset,
            static_cast<uint32_t>(patch.vertex_count), patch.bounds.box(),
            {uv.x, uv.y, uv.z, uv.w}, {wind.x, wind.y, wind.z, wind.w},
            patch.bounds.local.has_value(), enabled_ && patch.lod_visible});
    }
}

void MassiveFoliageFeature::destroy(RenderContext& ctx) {
    auto* engine_ = ctx.engine;
    // Depth proxies first (they live in the depth pre-pass scene, torn down
    // after the features — see FilamentBackend dtor ordering).
    depth_proxy_.destroy(engine_);
    for (auto& patch : patches_) {
        if (patch.entity) {
            if (in_scene_) ctx.scene->remove(patch.entity);
            engine_->destroy(patch.entity);
            utils::EntityManager::get().destroy(patch.entity);
        }
    }
    patches_.clear();
    for (auto& geometry : geometries_) {
        if (geometry.vb) engine_->destroy(geometry.vb);
        if (geometry.ib) engine_->destroy(geometry.ib);
    }
    geometries_.clear();

    if (grass_mi_)    { engine_->destroy(grass_mi_);    grass_mi_ = nullptr; }
    if (flower_mi_)   { engine_->destroy(flower_mi_);   flower_mi_ = nullptr; }
    grass_diffuse_texture_ = nullptr;
    flower_diffuse_texture_ = nullptr;
    if (material_)    { engine_->destroy(material_);    material_ = nullptr; }
}

} // namespace heaven
