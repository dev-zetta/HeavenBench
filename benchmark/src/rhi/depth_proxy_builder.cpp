/*
 * HeavenBench — Depth proxy builder
 *
 * Mirrors SceneBuilder's renderable construction, but emits lightweight depth
 * proxies that share the visible scene's geometry buffers and render the
 * shared depth-output material. See depth_proxy_builder.h for the rationale.
 */
#include "rhi/depth_proxy_builder.h"
#include "rhi/filament_bounds.h"
#include "rhi/scene_builder.h"   // select_renderable_prims, RenderablePrim
#include "rhi/instance_grouping.h" // compute_instance_groups
#include "rhi/prepared_surface_culling.h"
#include "rhi/filament_util.h"   // glm_to_filament
#include "rhi/texture_store.h"

#include "rhi/geometry_displacement.h"

#include <filament/Engine.h>
#include <filament/Camera.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/InstanceBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <math/vec3.h>
#include <math/mat4.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace heaven {

void DepthProxyBuilder::build(const BackendCore& core, MeshStore& meshes,
                              filament::Material*         depth_material,
                              filament::Material*         masked_material,
                              TextureStore&               textures,
                              filament::Scene* proxy_scene,
                              std::span<const Instance> instances,
                              std::span<const GpuMesh>  gpu_meshes,
                              const MaterialResolver&   resolve_material,
                              float tessellation_scale,
                              float tessellation_distance_multiplier,
                              bool gpu_instancing,
                              const std::unordered_set<int>& skinned_mesh_indices,
                              bool gpu_skinning,
                              bool tessellation_enabled)
{
    // Without a depth material instance or a proxy scene there is nothing to
    // build; leave built() false so the caller can retry once they exist.
    if (!depth_material || !proxy_scene) return;

    auto* engine_ = core.engine;
    const auto camera_position = core.camera ? core.camera->getPosition() : filament::math::double3{};
    const glm::dvec3 culling_camera(camera_position.x, camera_position.y, camera_position.z);
    const auto grid_origin = core.engine->getFeatureFlag("view.enable_grid_based_world_origin");
    const bool supported_world_origins = core.camera && grid_origin && !*grid_origin;


    // Point/clamp sampler for the foliage diffuse alpha (we only read its
    // alpha for the cutout; LINEAR keeps the cutout edge from aliasing).
    filament::TextureSampler mask_sampler(
        filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);

    // Height geometry uses the same isotropic trilinear reconstruction as the
    // color and shadow materials. Keep the alpha-mask sampler independent.
    filament::TextureSampler geometry_height_sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
    geometry_height_sampler.setAnisotropy(1.0f);

    auto depth_instance_for = [&](const RenderablePrim& prim)
        -> filament::MaterialInstance* {
        if (auto it = surface_instances_.find(prim.surface_handle);
            it != surface_instances_.end()) {
            return it->second;
        }
        const auto& gpu_mat = prim.gpu_mat;
        const bool masked = gpu_mat.alpha_ref > 0.0f && masked_material;
        auto* mi = (masked ? masked_material : depth_material)->createInstance();
        if (!mi) return nullptr;

        if (masked) {
            const filament::Texture* diffuse = textures.get(gpu_mat.diffuse);
            mi->setParameter("baseColorMap",
                diffuse ? diffuse : textures.placeholder(), mask_sampler);
        }
        const filament::Texture* height = textures.get(gpu_mat.height);
        mi->setParameter("geometryHeightMap",
            height ? height : textures.placeholder(), geometry_height_sampler);
        mi->setParameter("baseTransform", filament::math::float4{
            gpu_mat.base_transform[0], gpu_mat.base_transform[1],
            gpu_mat.base_transform[2], gpu_mat.base_transform[3]});
        const auto displacement = geometry_displacement(
            gpu_mat, height != nullptr,
            tessellation_scale, tessellation_distance_multiplier, tessellation_enabled);
        mi->setParameter("geometryDisplacement", displacement.enabled);
        mi->setParameter("displacementParams", filament::math::float4{
            displacement.amplitude,
            displacement.power,
            displacement.fade.threshold,
            displacement.enabled ? displacement.fade.inverse_range() : 0.0f});
        surface_instances_[prim.surface_handle] = mi;
        return mi;
    };

    // Append the geometry + per-prim depth material of a mesh's valid prims to
    // a RenderableManager::Builder. The depth proxy has NO Shadow_Split — every
    // valid prim goes into ONE renderable — and the material selection (opaque
    // depth_mi vs per-diffuse masked instance for alpha-tested foliage) is
    // identical for the per-instance and instanced paths.
    auto add_prim_geometry = [&](filament::RenderableManager::Builder& builder,
                                 const std::vector<RenderablePrim>& valid_prims) {
        for (size_t pi = 0; pi < valid_prims.size(); pi++) {
            const auto& p = valid_prims[pi];
            builder.geometry(pi,
                filament::RenderableManager::PrimitiveType::TRIANGLES,
                p.vb, p.ib, p.index_offset, p.index_count);
            // Alpha-tested foliage (alpha_ref > 0) uses the masked depth
            // instance bound to its diffuse map so its cutout matches the
            // visible foliage; everything else uses the shared opaque instance.
            if (auto* mi = depth_instance_for(p)) builder.material(pi, mi);
        }
    };

    // Build a single per-instance depth proxy renderable sharing the visible
    // scene's VB/IB. Used for singletons, skinned meshes, the fallback path,
    // and every instance when GPU instancing is disabled.
    auto build_proxy = [&](int idx, bool include_ordinary = true) {
        const auto& inst = instances[idx];
        if (inst.mesh_index < 0 ||
            std::cmp_greater_equal(inst.mesh_index, gpu_meshes.size())) return;

        const auto& gpu_mesh = gpu_meshes[inst.mesh_index];
        if (gpu_mesh.surfaces.empty()) return;

        // Select EXACTLY the same primitives the visible scene renders, so the
        // depth silhouette matches what is on screen.
        std::vector<RenderablePrim> valid_prims =
            select_renderable_prims(meshes, gpu_mesh, resolve_material);

        if (valid_prims.empty()) return;

        // A GPU-skinned Animated_Object's visible renderable is deformed on the
        // GPU from per-frame bone matrices while its vertex buffer stays at the
        // rest pose. The depth proxy shares that rest-pose VB (which carries the
        // BONE_INDICES/BONE_WEIGHTS attributes from upload_skinned), so it must
        // ALSO be GPU-skinned and receive the SAME bone matrices each frame —
        // otherwise the depth silhouette would sit at the rest pose while the
        // mesh animates, and the screen-space depth consumers (fog, god rays,
        // SSGI, rain) would composite atmosphere in front of the moving mesh
        // (it would look translucent). Detect skinning per-mesh via MeshStore's
        // marker, independent of the instancing set (skinned meshes are never
        // instanced, so they always take this per-instance proxy path).
        bool     mesh_skinned    = false;
        bool     mesh_skinned_source = false;
        uint32_t mesh_bone_count = 0;
        for (const auto& p : valid_prims) {
            const auto info = meshes.skinned_info(gpu_mesh.surfaces[p.surf_idx].handle);
            mesh_skinned_source |= info.skinned_source;
            if (gpu_skinning && info.skinned && info.bone_count > 0) {
                mesh_skinned = true;
                mesh_bone_count = std::max(mesh_bone_count, info.bone_count);
            }
        }

        std::vector<RenderablePrim> ordinary_prims;
        std::vector<RenderablePrim> prepared_prims;
        for (const auto& p : valid_prims) {
            if (meshes.is_displacement_prepared(p.surface_handle)) {
                prepared_prims.push_back(p);
            } else if (include_ordinary) {
                ordinary_prims.push_back(p);
            }
        }

        auto build_proxy_renderable = [&](const std::vector<RenderablePrim>& prims,
                                          const RenderablePrim* prepared_range = nullptr) {
            if (prims.empty()) return;
            // Mirror the visible split and borrow the exact same VB/IB. The depth
            // pass has no shadow partition; prepared surfaces own separate bounds.
            auto entity = utils::EntityManager::get().create();

            filament::RenderableManager::Builder builder(prims.size());
            builder.castShadows(false);
            builder.receiveShadows(false);

            add_prim_geometry(builder, prims);

            // Match the visible renderable's GPU skinning so the depth proxy
            // deforms in lockstep (seeded to the bind pose; bones pushed per frame
            // by FilamentBackend::update_skinned_bones via skinned_proxy_entities).
            if (mesh_skinned && mesh_bone_count > 0) {
                builder.skinning(mesh_bone_count);
            }

            const auto bounds_min = prepared_range
                ? prepared_range->bounds_min : gpu_mesh.bbox_min;
            const auto bounds_max = prepared_range
                ? prepared_range->bounds_max : gpu_mesh.bbox_max;
            const auto extent = bounds_max - bounds_min;
            const float bbox_volume = extent.x * extent.y * extent.z;
            const bool base_culling = !mesh_skinned_source && std::isfinite(bbox_volume) &&
                bbox_volume <= 100000.0f;
            const filament::math::float3 bmin{bounds_min.x, bounds_min.y, bounds_min.z};
            const filament::math::float3 bmax{bounds_max.x, bounds_max.y, bounds_max.z};
            const filament::math::float3 margin(1.0f);
            const auto outer_box = box_from_min_max(bmin - margin, bmax + margin);
            const PreparedSurfaceCullingBounds culling_bounds{bounds_min, bounds_max,
                {outer_box.center.x, outer_box.center.y, outer_box.center.z},
                {outer_box.halfExtent.x, outer_box.halfExtent.y, outer_box.halfExtent.z}};
            const bool culling = prepared_range
                ? prepared_surface_culling_enabled(base_culling, inst.transform, culling_bounds,
                    culling_camera, supported_world_origins) :
                    base_culling && instance_transform_supports_culling(inst.transform);
            builder.culling(culling);
            builder.boundingBox(outer_box);
            builder.build(*engine_, entity);

            auto& tcm = engine_->getTransformManager();
            tcm.create(entity);
            auto ti = tcm.getInstance(entity);
            tcm.setTransform(ti, glm_to_filament(inst.transform));

            proxy_scene->addEntity(entity);
            proxies_.push_back({entity, idx, prepared_range != nullptr, base_culling, culling, culling_bounds});

            // Record skinned proxy renderables so the per-frame bone update reaches
            // them with the same matrices as the visible mesh (spec  for the
            // animated-mesh depth silhouette).
            if (mesh_skinned && mesh_bone_count > 0) {
                skinned_proxy_entities_[inst.mesh_index].push_back(entity);
            }
        };

        build_proxy_renderable(ordinary_prims);
        for (const auto& p : prepared_prims) {
            build_proxy_renderable({p}, &p);
        }
    };

    // Emit a one-time diagnostic when a depth-proxy group cannot be instanced
    // and falls back to the per-instance proxy loop (mirrors SceneBuilder's
    // per-group fallback, tagged [DepthProxyBuilder]).
    auto warn_instancing_fallback = [&](int mesh_index) {
        if (!instancing_fallback_warned_.insert(mesh_index).second) return;
        const char* path =
            (mesh_index >= 0 && std::cmp_less(mesh_index, gpu_meshes.size()))
                ? gpu_meshes[mesh_index].path.c_str() : "<unknown>";
        std::fprintf(stderr,
            "[DepthProxyBuilder] GPU instancing unavailable for mesh %d (%s); "
            "rendering this depth group via the per-instance path.\n",
            mesh_index, path);
    };

    // Build ONE instanced depth-proxy renderable for a group chunk: every member
    // is placed by the chunk's shared InstanceBuffer local transforms; the
    // renderable's own transform is identity. Returns true if instanced, false
    // if it could not be (caller falls back to the per-instance proxy path).
    auto build_instanced_proxy_chunk = [&](const InstanceGroup& group) -> bool {
        if (group.mesh_index < 0 ||
            std::cmp_greater_equal(group.mesh_index, gpu_meshes.size())) return false;

        const auto& gpu_mesh = gpu_meshes[group.mesh_index];
        if (gpu_mesh.surfaces.empty()) return false;

        std::vector<RenderablePrim> valid_prims =
            select_renderable_prims(meshes, gpu_mesh, resolve_material);
        // Keep the source contract even for direct builder callers that omit
        // the backend's shared exclusion set (CPU skinning also deforms the VB).
        if (std::ranges::any_of(valid_prims, [&](const RenderablePrim& p) {
                return meshes.skinned_info(p.surface_handle).skinned_source;
            })) return false;
        std::erase_if(valid_prims, [&](const RenderablePrim& p) {
            return meshes.is_displacement_prepared(p.surface_handle);
        });
        if (valid_prims.empty()) return false;

        const size_t chunk_size = group.instance_indices.size();
        if (chunk_size < 2) return false;

        // Seed the per-instance local transforms from the member instances'
        // world transforms — the SAME member indices SceneBuilder uses, so the
        // depth placements track the visible instanced scene.
        std::vector<filament::math::mat4f> local_transforms(chunk_size);
        for (size_t i = 0; i < chunk_size; i++) {
            local_transforms[i] =
                glm_to_filament(instances[group.instance_indices[i]].transform);
        }

        filament::InstanceBuffer* ibuf =
            filament::InstanceBuffer::Builder(chunk_size)
                .localTransforms(local_transforms.data())
                .build(*engine_);
        if (!ibuf) {
            warn_instancing_fallback(group.mesh_index);
            return false;
        }

        InstanceLocalBounds local_bounds;
        for (const auto& prim : valid_prims)
            include_instance_local_bounds(local_bounds, prim.bounds_min,
                prim.bounds_max, prim.gpu_mat.geometry_displacement);
        const auto world_bounds = instance_group_world_bounds(
            local_bounds, instances, group.instance_indices);
        const filament::math::float3 bmin{
            world_bounds.min.x, world_bounds.min.y, world_bounds.min.z};
        const filament::math::float3 bmax{
            world_bounds.max.x, world_bounds.max.y, world_bounds.max.z};

        auto entity = utils::EntityManager::get().create();

        filament::RenderableManager::Builder builder(valid_prims.size());
        builder.castShadows(false);
        builder.receiveShadows(false);

        add_prim_geometry(builder, valid_prims);

        // Draw every member of the chunk as a GPU instance placed by the shared
        // InstanceBuffer's local transforms, mirroring SceneBuilder.
        builder.instances(chunk_size, ibuf);

        builder.culling(world_bounds.culling);
        builder.boundingBox(box_from_min_max(bmin, bmax));
        builder.build(*engine_, entity);

        // The renderable's own transform is identity; per-instance world
        // placement comes from the InstanceBuffer.
        auto& tcm = engine_->getTransformManager();
        tcm.create(entity);

        proxy_scene->addEntity(entity);

        InstancedProxyChunk chunk;
        chunk.entity           = entity;
        chunk.buffer           = ibuf;
        chunk.instance_indices = group.instance_indices;
        chunk.local_transforms = std::move(local_transforms);
        chunk.local_bounds = local_bounds;
        instanced_chunks_.push_back(std::move(chunk));
        return true;
    };

    if (gpu_instancing) {
        // If the engine cannot instance at all (max automatic instances < 2),
        // build every proxy via the per-instance path with a single one-time
        // diagnostic, mirroring SceneBuilder's whole-path fallback.
        const size_t max_per_group = engine_->getMaxAutomaticInstances();
        if (max_per_group < 2) {
            if (!instancing_unavailable_warned_) {
                instancing_unavailable_warned_ = true;
                std::fprintf(stderr,
                    "[DepthProxyBuilder] GPU instancing unavailable (engine "
                    "reports max automatic instances = %zu, need >= 2); "
                    "rendering all depth proxies via the per-instance path.\n",
                    max_per_group);
            }
            for (int idx = 0; std::cmp_less(idx, instances.size()); idx++) {
                build_proxy(idx);
            }
            built_ = true;
            return;
        }

        const std::vector<InstanceGroup> groups = compute_instance_groups(
            instances, skinned_mesh_indices, max_per_group);

        // Track which instances were drawn via the instanced path so the
        // per-instance loop does not also draw them (no double-rendering).
        std::unordered_set<int> grouped_instances;
        for (const auto& group : groups) {
            if (build_instanced_proxy_chunk(group)) {
                for (int member : group.instance_indices) {
                    grouped_instances.insert(member);
                }
            }
            // A chunk that failed to instance is left for the per-instance loop.
        }

        for (int idx = 0; std::cmp_less(idx, instances.size()); idx++) {
            build_proxy(idx, !grouped_instances.contains(idx));
        }
    } else {
        for (int idx = 0; std::cmp_less(idx, instances.size()); idx++) {
            build_proxy(idx);
        }
    }

    built_ = true;
}

void DepthProxyBuilder::update_transforms(const BackendCore& core,
                                          std::span<const Instance> instances,
                                          std::span<const int> dirty_instance_indices)
{
    const auto camera_position = core.camera ? core.camera->getPosition() : filament::math::double3{};
    const glm::dvec3 culling_camera(camera_position.x, camera_position.y, camera_position.z);
    const auto grid_origin = core.engine->getFeatureFlag("view.enable_grid_based_world_origin");
    const bool supported_world_origins = core.camera && grid_origin && !*grid_origin;

    const auto is_dirty = [&](int index) {
        return std::binary_search(dirty_instance_indices.begin(),
                                  dirty_instance_indices.end(), index);
    };

    auto& tcm = core.engine->getTransformManager();
    for (auto& pe : proxies_) {
        if (std::cmp_greater_equal(pe.instance_index, instances.size())) continue;
        const bool dirty = is_dirty(pe.instance_index);
        if (!dirty && !pe.prepared_culling) continue;
        const auto& inst = instances[pe.instance_index];
        if (dirty) {
            auto ti = tcm.getInstance(pe.entity);
            if (ti) tcm.setTransform(ti, glm_to_filament(inst.transform));
        }
        if (pe.prepared_culling) {
            const bool culling = prepared_surface_culling_enabled(pe.base_culling, inst.transform,
                pe.culling_bounds, culling_camera, supported_world_origins);
            if (culling != pe.culling_enabled) {
                auto& renderables = core.engine->getRenderableManager();
                const auto ri = renderables.getInstance(pe.entity);
                if (ri) renderables.setCulling(ri, culling);
                pe.culling_enabled = culling;
            }
        } else if (dirty) {
            const bool culling = pe.base_culling &&
                instance_transform_supports_culling(inst.transform);
            if (culling != pe.culling_enabled) {
                auto& renderables = core.engine->getRenderableManager();
                const auto ri = renderables.getInstance(pe.entity);
                if (ri) renderables.setCulling(ri, culling);
                pe.culling_enabled = culling;
            }
        }
    }

    if (dirty_instance_indices.empty()) return;

    // Instanced depth chunks: refresh each chunk's InstanceBuffer local
    // transforms from the CURRENT member instance transforms — never rebuild
    // the renderables or buffers. This uses the SAME member indices and
    // glm_to_filament conversion as SceneBuilder's instanced sync, so the depth
    // silhouette tracks animated instanced placements in lockstep with the
    // visible scene.
    for (auto& chunk : instanced_chunks_) {
        if (!chunk.buffer || chunk.instance_indices.empty()) continue;

        bool changed = false;
        for (size_t i = 0; i < chunk.instance_indices.size(); i++) {
            const int member = chunk.instance_indices[i];
            if (member < 0 || std::cmp_greater_equal(member, instances.size()) ||
                !is_dirty(member)) continue;
            chunk.local_transforms[i] =
                glm_to_filament(instances[member].transform);
            changed = true;
        }

        if (changed) {
            chunk.buffer->setLocalTransforms(
                chunk.local_transforms.data(), chunk.local_transforms.size(),
                /*offset=*/0);
            const auto bounds = instance_group_world_bounds(
                chunk.local_bounds, instances, chunk.instance_indices);
            auto& renderables = core.engine->getRenderableManager();
            for (const auto entity : {chunk.entity}) {
                if (!entity) continue;
                const auto ri = renderables.getInstance(entity);
                if (!ri) continue;
                renderables.setCulling(ri, bounds.culling);
                renderables.setAxisAlignedBoundingBox(ri, box_from_min_max(
                    {bounds.min.x, bounds.min.y, bounds.min.z},
                    {bounds.max.x, bounds.max.y, bounds.max.z}));
            }
        }
    }
}

void DepthProxyBuilder::clear(const BackendCore& core, filament::Scene* proxy_scene)
{
    auto* engine_ = core.engine;
    for (auto& pe : proxies_) {
        if (proxy_scene) proxy_scene->remove(pe.entity);
        engine_->destroy(pe.entity);
        utils::EntityManager::get().destroy(pe.entity);
    }
    proxies_.clear();

    // Tear down the instanced depth renderables and their InstanceBuffers, each
    // chunk owning one buffer (mirrors SceneBuilder::clear).
    for (auto& chunk : instanced_chunks_) {
        if (chunk.entity) {
            if (proxy_scene) proxy_scene->remove(chunk.entity);
            engine_->destroy(chunk.entity);
            utils::EntityManager::get().destroy(chunk.entity);
        }
        if (chunk.buffer) {
            engine_->destroy(chunk.buffer);
        }
    }
    instanced_chunks_.clear();
    instancing_fallback_warned_.clear();
    instancing_unavailable_warned_ = false;
    skinned_proxy_entities_.clear();

    // Destroy the per-foliage masked instances we created. The masked base
    // Material itself is owned by DepthPrePass and outlives these instances
    // (clear() runs before DepthPrePass teardown).
    for (auto& [handle, mi] : surface_instances_) {
        if (mi) engine_->destroy(mi);
    }
    surface_instances_.clear();
    built_ = false;
}

std::span<const utils::Entity> DepthProxyBuilder::skinned_proxy_entities(
    int mesh_index) const
{
    auto it = skinned_proxy_entities_.find(mesh_index);
    if (it == skinned_proxy_entities_.end()) return {};
    return it->second;
}

} // namespace heaven
