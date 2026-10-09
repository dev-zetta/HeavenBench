#include "benchmark/microprofiler.h"
/*
 * HeavenBench — Scene builder
 *
 * Extracted verbatim out of FilamentBackend::build_scene_entities /
 * update_animated_transforms / clear_renderables.
 */
#include "rhi/scene_builder.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"   // glm_to_filament, is_lod_imposter
#include "rhi/instance_grouping.h" // compute_instance_groups
#include "rhi/prepared_surface_culling.h"
#include "engine/material_model_ref.h"

#include <filament/Engine.h>
#include <filament/Camera.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/InstanceBuffer.h>
#include <filament/MaterialInstance.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <math/vec3.h>
#include <math/mat4.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace heaven {
namespace {

bool is_extracted_sky_primitive(const GpuMaterial& mat) {
    if (mat.material_name == "heaven_sky") return true;
    return std::find(mat.parent_chain.begin(), mat.parent_chain.end(), "sky_base")
        != mat.parent_chain.end();
}

} // namespace

std::vector<RenderablePrim> select_renderable_prims(
        MeshStore& meshes, const GpuMesh& gpu_mesh,
        const MaterialResolver& resolve_material)
{
    std::vector<RenderablePrim> valid_prims;

    for (size_t i = 0; i < gpu_mesh.surfaces.size(); i++) {
        const auto& surf = gpu_mesh.surfaces[i];
        auto bufs = meshes.find(surf.handle);
        if (!bufs.vb || !bufs.ib) continue;
        if (surf.index_count == 0) continue; // hidden (duplicate of skinned mesh)

        // Render only the highest-detail LOD; skip the simplified
        // far-distance imposters (see is_lod_imposter()).
        if (is_lod_imposter(surf.name)) continue;

        auto gpu_mat = resolve_material(gpu_mesh.path, surf.name);
        if (gpu_mat.hidden) {
            continue;
        }
        if (is_extracted_sky_primitive(gpu_mat)) {
            continue;
        }

        const auto ranges = meshes.prepared_draw_ranges(surf.handle);
        if (ranges.empty()) {
            valid_prims.push_back({i, surf.handle, bufs.vb, bufs.ib, std::move(gpu_mat),
                                  0, surf.index_count, surf.bbox_min, surf.bbox_max});
        } else {
            for (const auto& range : ranges)
                valid_prims.push_back({i, surf.handle, bufs.vb, bufs.ib, gpu_mat,
                                      range.index_offset, range.index_count,
                                      range.bounds_min, range.bounds_max});
        }
    }

    return valid_prims;
}

void SceneBuilder::build(const BackendCore& core, MeshStore& meshes,
                         MaterialInstanceCache& materials,
                         std::span<const Instance> instances,
                         std::span<const GpuMesh>  gpu_meshes,
                         const MaterialResolver&   resolve_material,
                         bool gpu_skinning,
                         bool gpu_instancing,
                         const std::unordered_set<int>& skinned_mesh_indices)
{
    profile::Scope profile_scope("Build persistent scene", "Scene");
    auto* engine_ = core.engine;
    const auto camera_position = core.camera ? core.camera->getPosition() : filament::math::double3{};
    const glm::dvec3 culling_camera(camera_position.x, camera_position.y, camera_position.z);
    const auto grid_origin = core.engine->getFeatureFlag("view.enable_grid_based_world_origin");
    const bool supported_world_origins = core.camera && grid_origin && !*grid_origin;

    auto* scene_  = core.scene;

    // Partition a mesh's valid prims into the Shadow_Split caster / non-caster
    // groups. Filament's castShadows is per-renderable, but thin rope/rigging
    // surfaces live INSIDE larger meshes whose other surfaces SHOULD cast, so we
    // build up to two renderables: one (castShadows=true) for the normal
    // surfaces and one (castShadows=false) for the rope/rigging surfaces — the
    // latter still render and still receive shadows, they just don't cast the
    // aliased thin streak. A whole mesh flagged non-casting sends every surface
    // to the non-casting group. This decision depends only on the mesh + the
    // resolved materials, so it is identical for every instance of a mesh —
    // which is exactly why an Instance_Group can share one renderable per
    // subset.
    auto is_transparent_foliage_shadow_caster_for_surface =
        [&](const RenderablePrim& p, const GpuMesh& mesh) {
            const std::string& surface_name = mesh.surfaces[p.surf_idx].name;
            return materials.transparent_shadows() && p.gpu_mat.alpha_ref > 0.0f &&
                material_model_ref::is_foliage_shadow_caster(
                    surface_name, p.gpu_mat.material_name, p.gpu_mat.parent_chain);
        };

    auto partition_shadow_split = [&](const std::vector<RenderablePrim>& valid_prims,
                                      const GpuMesh& gpu_mesh,
                                      std::vector<const RenderablePrim*>& caster_prims,
                                      std::vector<const RenderablePrim*>& noncaster_prims,
                                      std::vector<const RenderablePrim*>& foliage_shadow_prims) {
        for (const auto& p : valid_prims) {
            const bool foliage_shadow =
                gpu_mesh.cast_shadows && p.gpu_mat.casts_shadow &&
                is_transparent_foliage_shadow_caster_for_surface(p, gpu_mesh);
            if (foliage_shadow) {
                foliage_shadow_prims.push_back(&p);
            }
            const bool casts =
                gpu_mesh.cast_shadows && p.gpu_mat.casts_shadow && !foliage_shadow;
            (casts ? caster_prims : noncaster_prims).push_back(&p);
        }
    };

    // Build a single per-instance renderable assembled from the mesh's valid
    // prims (the current, non-instanced path). Used for singletons, skinned
    // Animated_Objects, and every instance when GPU instancing is disabled.
    auto build_instance = [&](int idx, bool include_ordinary = true) {
        const auto& inst = instances[idx];
        if (inst.mesh_index < 0 ||
            std::cmp_greater_equal(inst.mesh_index, gpu_meshes.size())) return;

        const auto& gpu_mesh = gpu_meshes[inst.mesh_index];
        if (gpu_mesh.surfaces.empty()) return;

        // Select the valid (non-hidden, non-imposter) primitives. Shared with
        // DepthProxyBuilder so the depth silhouette matches the visible scene.
        std::vector<RenderablePrim> valid_prims =
            select_renderable_prims(meshes, gpu_mesh, resolve_material);

        if (valid_prims.empty()) {
            return;
        }

        // Determine whether this mesh is an Animated_Object uploaded for GPU
        // skinning. MeshStore marks each skinned surface with the mesh's bone
        // count (upload_skinned); skinning is per-renderable, so all primitives
        // of the renderable share one bone set. A skinned renderable is never
        // instanced  — it stays on the normal per-renderable path with
        // .skinning() enabled, seeded to the bind pose (identity-equivalent),
        // and its entity(ies) are recorded for the per-frame bone update.
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
        std::vector<const RenderablePrim*> prepared_prims;
        for (const auto& p : valid_prims) {
            if (meshes.is_displacement_prepared(p.surface_handle)) {
                prepared_prims.push_back(&p);
            } else if (include_ordinary) {
                ordinary_prims.push_back(p);
            }
        }

        std::vector<const RenderablePrim*> caster_prims;
        std::vector<const RenderablePrim*> noncaster_prims;
        std::vector<const RenderablePrim*> foliage_shadow_prims;
        partition_shadow_split(ordinary_prims, gpu_mesh, caster_prims, noncaster_prims,
                               foliage_shadow_prims);
        std::unordered_map<uint64_t, filament::MaterialInstance*> prepared_materials;
        std::unordered_map<uint64_t, filament::MaterialInstance*> prepared_foliage_materials;

        // Build one renderable from a subset of prims with the given shadow-
        // casting flag, wire its transform, add it to the scene, and record it
        // for per-frame transform sync (keyed by instance index, so a second
        // entity for the same instance tracks the same transform).
        auto build_renderable = [&](const std::vector<const RenderablePrim*>& prims,
                                    bool cast_shadows,
                                    bool foliage_shadow_role = false,
                                    const RenderablePrim* prepared_range = nullptr) {
            if (prims.empty()) return;
            auto entity = utils::EntityManager::get().create();

            filament::RenderableManager::Builder builder(prims.size());
            builder.castShadows(cast_shadows);
            builder.receiveShadows(!foliage_shadow_role);
            // Filament requires receiver opt-in as well as the sun's live
            // contact-shadow toggle. Shadow-only duplicates never receive.
            builder.screenSpaceContactShadows(!foliage_shadow_role);

            for (size_t pi = 0; pi < prims.size(); pi++) {
                const auto& p = *prims[pi];
                builder.geometry(pi,
                    filament::RenderableManager::PrimitiveType::TRIANGLES,
                    p.vb, p.ib, p.index_offset, p.index_count);
                auto get_material = [&] {
                    return foliage_shadow_role
                        ? materials.get_or_create_foliage_shadow(
                            gpu_mesh.path, gpu_mesh.surfaces[p.surf_idx].name, p.gpu_mat)
                        : materials.get_or_create(
                            gpu_mesh.path, gpu_mesh.surfaces[p.surf_idx].name, p.gpu_mat);
                };
                const filament::MaterialInstance* mi = nullptr;
                if (prepared_range) {
                    // A spatial leaf is a draw subdivision, not another
                    // semantic material usage or surface placement.
                    auto& cache = foliage_shadow_role
                        ? prepared_foliage_materials : prepared_materials;
                    const auto [entry, inserted] = cache.try_emplace(p.surface_handle, nullptr);
                    if (inserted) entry->second = get_material();
                    mi = entry->second;
                } else {
                    mi = get_material();
                }
                if (mi) builder.material(pi, mi);
            }

            // Enable GPU skinning for an Animated_Object. skinning(boneCount)
            // seeds all bone transforms to identity — the bind pose — so the
            // mesh renders at its rest position until the first per-frame bone
            // update  advances the animation. The renderable's VB
            // already carries BONE_INDICES/BONE_WEIGHTS (MeshStore::upload_skinned).
            if (mesh_skinned && mesh_bone_count > 0) {
                builder.skinning(mesh_bone_count);
            }

            // A prepared surface owns an independent conservative envelope;
            // ordinary primitives retain the existing whole-mesh bounds.
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

            scene_->addEntity(entity);
            scene_entities_.push_back({entity, idx, prepared_range != nullptr, base_culling, culling, culling_bounds});

            // Record skinned renderables under their mesh index so the
            // per-frame bone update reaches every renderable of the object
            // (the caster + non-caster split means up to two entities).
            if (mesh_skinned && mesh_bone_count > 0) {
                skinned_entities_[inst.mesh_index].push_back(entity);
            }
        };

        build_renderable(caster_prims, true);
        build_renderable(noncaster_prims, false);
        build_renderable(foliage_shadow_prims, true, /*foliage_shadow_role=*/true);

        // Keep every prepared placement independently cullable in the camera
        // and shadow passes. Its ordinary neighbours can still be instanced.
        for (const auto* p : prepared_prims) {
            const bool foliage_shadow = gpu_mesh.cast_shadows && p->gpu_mat.casts_shadow &&
                is_transparent_foliage_shadow_caster_for_surface(*p, gpu_mesh);
            const bool casts = gpu_mesh.cast_shadows && p->gpu_mat.casts_shadow &&
                !foliage_shadow;
            const std::vector<const RenderablePrim*> single_prim{p};
            build_renderable(single_prim, casts, false, p);
            if (foliage_shadow) build_renderable(single_prim, true, true, p);
        }
    };

    // Emit a one-time diagnostic when an instance group cannot be drawn via the
    // instanced path and falls back to the per-instance loop. The
    // emission itself lives in the out-of-line warn_instancing_fallback() member
    // so build()'s body carries no debug-output call (codebase-hardening);
    // the per-mesh dedup is guarded there via instancing_fallback_warned_.
    auto warn_instancing_fallback = [&](int mesh_index) {
        this->warn_instancing_fallback(mesh_index, gpu_meshes);
    };

    // Build the instanced renderables for one group chunk: up to two renderables
    // (the Shadow_Split caster + non-caster subsets) sharing ONE InstanceBuffer
    // whose local transforms place each member instance. The renderables' own
    // transforms are identity — the world placement lives entirely in the
    // instance buffer. Returns true if the chunk was instanced; false if it
    // could not be (caller falls back to the per-instance path for its members).
    auto build_instanced_chunk = [&](const InstanceGroup& group) -> bool {
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
        // world transforms. The renderable transform stays identity, so each
        // instance is placed by its own local transform. The per-frame
        // sync  refreshes these from the same member indices.
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
            // The InstanceBuffer failed to build — degrade this group to the
            // per-instance path with a one-time diagnostic.
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

        std::vector<const RenderablePrim*> caster_prims;
        std::vector<const RenderablePrim*> noncaster_prims;
        std::vector<const RenderablePrim*> foliage_shadow_prims;
        partition_shadow_split(valid_prims, gpu_mesh, caster_prims, noncaster_prims,
                               foliage_shadow_prims);

        // Build one instanced renderable from a Shadow_Split subset, sharing the
        // chunk's InstanceBuffer. Returns the entity (null if the subset is
        // empty). The renderable transform is left at identity (the instance
        // buffer holds the world placements).
        auto build_instanced_renderable =
            [&](const std::vector<const RenderablePrim*>& prims,
                bool cast_shadows,
                bool foliage_shadow_role = false) -> utils::Entity {
            if (prims.empty()) return {};
            auto entity = utils::EntityManager::get().create();

            filament::RenderableManager::Builder builder(prims.size());
            builder.castShadows(cast_shadows);
            builder.receiveShadows(!foliage_shadow_role);
            builder.screenSpaceContactShadows(!foliage_shadow_role);

            for (size_t pi = 0; pi < prims.size(); pi++) {
                const auto& p = *prims[pi];
                builder.geometry(pi,
                    filament::RenderableManager::PrimitiveType::TRIANGLES,
                    p.vb, p.ib, p.index_offset, p.index_count);
                auto* mi = foliage_shadow_role
                    ? materials.get_or_create_foliage_shadow(
                        gpu_mesh.path, gpu_mesh.surfaces[p.surf_idx].name, p.gpu_mat)
                    : materials.get_or_create(
                        gpu_mesh.path, gpu_mesh.surfaces[p.surf_idx].name,
                        p.gpu_mat, chunk_size);
                if (mi) builder.material(pi, mi);
            }

            // Draw every member of the chunk as a GPU instance placed by the
            // shared InstanceBuffer's local transforms.
            builder.instances(chunk_size, ibuf);

            builder.culling(world_bounds.culling);
            builder.boundingBox(box_from_min_max(bmin, bmax));
            builder.build(*engine_, entity);

            // The renderable's own transform is identity; per-instance world
            // placement comes from the InstanceBuffer.
            auto& tcm = engine_->getTransformManager();
            tcm.create(entity);

            scene_->addEntity(entity);
            return entity;
        };

        InstancedChunk chunk;
        chunk.buffer           = ibuf;
        chunk.instance_indices = group.instance_indices;
        chunk.local_transforms = std::move(local_transforms);
        chunk.local_bounds = local_bounds;
        chunk.caster           = build_instanced_renderable(caster_prims, true);
        chunk.noncaster        = build_instanced_renderable(noncaster_prims, false);
        chunk.foliage_shadow   = build_instanced_renderable(
            foliage_shadow_prims, true, /*foliage_shadow_role=*/true);

        // If neither subset produced a renderable, the buffer is unused — the
        // group degrades to the per-instance path with a one-time diagnostic.
        if (!chunk.caster && !chunk.noncaster && !chunk.foliage_shadow) {
            engine_->destroy(ibuf);
            warn_instancing_fallback(group.mesh_index);
            return false;
        }

        instanced_chunks_.push_back(std::move(chunk));
        return true;
    };

    if (gpu_instancing) {
        // If the engine cannot instance at all (max automatic instances < 2),
        // skip grouping entirely and build every instance via the per-instance
        // path, with a single one-time diagnostic. This avoids
        // building 1-element InstanceBuffers that would each fail the chunk_size
        // >= 2 guard and degrade silently/wastefully.
        const size_t max_per_group = engine_->getMaxAutomaticInstances();
        if (max_per_group < 2) {
            warn_instancing_unavailable(max_per_group);
            for (int idx = 0; std::cmp_less(idx, instances.size()); idx++) {
                build_instance(idx);
            }
            scene_built_ = true;
            return;
        }

        const std::vector<InstanceGroup> groups = compute_instance_groups(
            instances, skinned_mesh_indices, max_per_group);

        // Track which instances were drawn via the instanced path so the
        // per-instance loop does not also draw them (no double-rendering).
        std::unordered_set<int> grouped_instances;
        for (const auto& group : groups) {
            if (build_instanced_chunk(group)) {
                for (int member : group.instance_indices) {
                    grouped_instances.insert(member);
                }
            }
            // A chunk that failed to instance is intentionally left for the
            // per-instance loop (its members stay un-grouped), so the per-group
            // fallback  renders them on the existing path.
        }

        for (int idx = 0; std::cmp_less(idx, instances.size()); idx++) {
            build_instance(idx, !grouped_instances.contains(idx));
        }
    } else {
        for (int idx = 0; std::cmp_less(idx, instances.size()); idx++) {
            build_instance(idx);
        }
    }

    scene_built_ = true;
}

void SceneBuilder::warn_instancing_fallback(int mesh_index,
                                            std::span<const GpuMesh> gpu_meshes)
{
    // Deduplicate per mesh so a persistently-failing group warns once rather
    // than every build; the rest of the scene is unaffected.
    if (!instancing_fallback_warned_.insert(mesh_index).second) return;
    const char* path =
        (mesh_index >= 0 && std::cmp_less(mesh_index, gpu_meshes.size()))
            ? gpu_meshes[mesh_index].path.c_str() : "<unknown>";
    std::fprintf(stderr,
        "[SceneBuilder] GPU instancing unavailable for mesh %d (%s); "
        "rendering this group via the per-instance path.\n",
        mesh_index, path);
}

void SceneBuilder::warn_instancing_unavailable(size_t max_automatic_instances)
{
    if (instancing_unavailable_warned_) return;
    instancing_unavailable_warned_ = true;
    std::fprintf(stderr,
        "[SceneBuilder] GPU instancing unavailable (engine reports "
        "max automatic instances = %zu, need >= 2); rendering the "
        "entire scene via the per-instance path.\n",
        max_automatic_instances);
}

void SceneBuilder::update_transforms(const BackendCore& core,
                                     std::span<const Instance> instances,
                                     std::span<const int> dirty_instance_indices)
{
    profile::Scope profile_scope("Synchronize transforms and bounds", "Scene");
    const auto camera_position = core.camera ? core.camera->getPosition() : filament::math::double3{};
    const glm::dvec3 culling_camera(camera_position.x, camera_position.y, camera_position.z);
    const auto grid_origin = core.engine->getFeatureFlag("view.enable_grid_based_world_origin");
    const bool supported_world_origins = core.camera && grid_origin && !*grid_origin;

    const auto is_dirty = [&](int index) {
        return std::binary_search(dirty_instance_indices.begin(),
                                  dirty_instance_indices.end(), index);
    };

    auto& tcm = core.engine->getTransformManager();
    for (auto& se : scene_entities_) {
        if (std::cmp_greater_equal(se.instance_index, instances.size())) continue;
        const bool dirty = is_dirty(se.instance_index);
        if (!dirty && !se.prepared_culling) continue;
        const auto& inst = instances[se.instance_index];
        if (dirty) {
            auto ti = tcm.getInstance(se.entity);
            if (ti) tcm.setTransform(ti, glm_to_filament(inst.transform));
        }
        if (se.prepared_culling) {
            const bool culling = prepared_surface_culling_enabled(se.base_culling, inst.transform,
                se.culling_bounds, culling_camera, supported_world_origins);
            if (culling != se.culling_enabled) {
                auto& renderables = core.engine->getRenderableManager();
                const auto ri = renderables.getInstance(se.entity);
                if (ri) renderables.setCulling(ri, culling);
                se.culling_enabled = culling;
            }
        } else if (dirty) {
            const bool culling = se.base_culling &&
                instance_transform_supports_culling(inst.transform);
            if (culling != se.culling_enabled) {
                auto& renderables = core.engine->getRenderableManager();
                const auto ri = renderables.getInstance(se.entity);
                if (ri) renderables.setCulling(ri, culling);
                se.culling_enabled = culling;
            }
        }
    }

    if (dirty_instance_indices.empty()) return;

    // Instanced placements: the only per-frame work is refreshing each chunk's
    // InstanceBuffer local transforms from the CURRENT member instance
    // transforms (— never rebuild the renderables or buffers). Each
    // member is placed by its own local transform, using the SAME member
    // indices and glm_to_filament conversion as the singleton per-entity sync
    // above, so animated instanced placements (floating-island
    // bob, bridge-driven offsets) track the scene identically to the
    // per-entity TransformManager path. The caster + non-caster renderables of
    // a chunk share one buffer, so a single update covers both.
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
            for (const auto entity : {chunk.caster, chunk.noncaster, chunk.foliage_shadow}) {
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

void SceneBuilder::clear(const BackendCore& core)
{
    auto* engine_ = core.engine;
    auto* scene_  = core.scene;
    for (auto& se : scene_entities_) {
        scene_->remove(se.entity);
        engine_->destroy(se.entity);
        utils::EntityManager::get().destroy(se.entity);
    }
    scene_entities_.clear();

    // Tear down the instanced renderables and their shared InstanceBuffers.
    // Each chunk owns one buffer (shared by its caster + non-caster entities),
    // so destroy the buffer once after both entities are gone — mirroring how
    // the VB/IB are owned by the engine and released on clear.
    for (auto& chunk : instanced_chunks_) {
        for (utils::Entity e : {chunk.caster, chunk.noncaster, chunk.foliage_shadow}) {
            if (!e) continue;
            scene_->remove(e);
            engine_->destroy(e);
            utils::EntityManager::get().destroy(e);
        }
        if (chunk.buffer) {
            engine_->destroy(chunk.buffer);
        }
    }
    instanced_chunks_.clear();

    instancing_fallback_warned_.clear();
    instancing_unavailable_warned_ = false;

    skinned_entities_.clear();
    scene_built_ = false;
}

std::span<const utils::Entity> SceneBuilder::skinned_entities(int mesh_index) const
{
    auto it = skinned_entities_.find(mesh_index);
    if (it == skinned_entities_.end()) return {};
    return it->second;
}

} // namespace heaven
