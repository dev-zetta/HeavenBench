/*
 * HeavenBench — Scene builder
 *
 * Builds the persistent renderable entities for the scene (one per
 * instance, assembled from its mesh's non-hidden, non-imposter surfaces),
 * keeps their transforms synced to the animated instance list each frame,
 * and tears them down. Entities are built ONCE and only have their
 * transforms updated thereafter (never regenerated per frame).
 *
 * Depends on MeshStore (surface handle → VB/IB) and MaterialInstanceCache
 * (surface → MaterialInstance). It owns the scene-entity list and the
 * "scene built" flag.
 *
 * Extracted verbatim out of FilamentBackend::build_scene_entities /
 * update_animated_transforms / clear_renderables.
 */
#ifndef HEAVENBENCH_SRC_RHI_SCENE_BUILDER_H_
#define HEAVENBENCH_SRC_RHI_SCENE_BUILDER_H_

#include "rhi/rhi.h"            // Instance, GpuMesh, MaterialResolver
#include "rhi/backend_core.h"
#include "rhi/mesh_store.h"
#include "rhi/prepared_surface_culling.h"
#include "rhi/instance_bounds.h"
#include "rhi/material_instance_cache.h"

#include <utils/Entity.h>
#include <math/mat4.h>

#include <cstddef>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace filament {
class VertexBuffer;
class IndexBuffer;
class InstanceBuffer;
}

namespace heaven {

// One primitive selected for rendering: its surface index within the owning
// mesh, the shared vertex/index buffers, and its resolved material.

// Produced by select_renderable_prims() below, which centralises the
// "which surfaces of this mesh do we actually draw" decision so the visible
// scene (SceneBuilder) and the depth-proxy scene (DepthProxyBuilder) select
// EXACTLY the same primitives — keeping the depth silhouette in lockstep with
// what is on screen.
struct RenderablePrim {
    std::size_t              surf_idx;
    uint64_t                 surface_handle;
    filament::VertexBuffer*  vb;
    filament::IndexBuffer*   ib;
    GpuMaterial              gpu_mat;
    std::uint32_t            index_offset = 0;
    std::uint32_t            index_count = 0;
    glm::vec3                bounds_min{};
    glm::vec3                bounds_max{};
};

// Select the valid primitives of `gpu_mesh`: a surface is included only when
// it has uploaded VB/IB, is not hidden (`index_count == 0`, e.g. a duplicate
// of a skinned mesh), is not a simplified LOD imposter, and its resolved
// material is not hidden. Prepared surfaces expand into exact shared-buffer
// spatial index ranges. Shared by SceneBuilder and DepthProxyBuilder.
[[nodiscard]] std::vector<RenderablePrim> select_renderable_prims(
        MeshStore& meshes, const GpuMesh& gpu_mesh,
        const MaterialResolver& resolve_material);

class SceneBuilder {
public:
    // Build the persistent renderable entities. Runs once (sets built()).

    // When `gpu_skinning` is true, a mesh whose surfaces MeshStore marked
    // skinned (an Animated_Object uploaded via upload_skinned) has its
    // renderable(s) built with RenderableManager::Builder::skinning(boneCount)
    // seeded to the bind pose (identity-equivalent bone transforms), and the
    // built entity(ies) are recorded under their mesh index so the per-frame
    // bone update  can drive every renderable of that object. A
    // skinned renderable is never instanced; it goes through the
    // normal per-renderable path with skinning enabled.

    // When `gpu_instancing` is true, Instances that share a `mesh_index` (and
    // are neither skinned nor singletons) are grouped via
    // compute_instance_groups() and drawn as instanced renderables — one per
    // non-empty Shadow_Split subset per chunk — built with
    // RenderableManager::Builder::instances(chunkSize, InstanceBuffer*). The
    // per-instance world placement lives in the InstanceBuffer's local
    // transforms (the renderable's own transform is identity); the caster and
    // non-caster renderables of a chunk SHARE one InstanceBuffer. Each chunk is
    // recorded as an InstancedChunk so the per-frame sync  can refresh
    // its transforms without rebuilding anything. `skinned_mesh_indices` is the
    // set of Animated_Object mesh indices to exclude from instancing (they stay
    // on the per-instance + skinning path); it is forwarded to
    // compute_instance_groups. Instances emitted in a group are NOT also built
    // by the per-instance loop (no double-rendering). Prepared displacement
    // surfaces are omitted from those groups and built once per placement with
    // independent surface bounds; ordinary surfaces of the same mesh stay grouped.
    void build(const BackendCore& core, MeshStore& meshes,
               MaterialInstanceCache& materials,
               std::span<const Instance> instances,
               std::span<const GpuMesh>  gpu_meshes,
               const MaterialResolver&   resolve_material,
               bool gpu_skinning = false,
               bool gpu_instancing = false,
               const std::unordered_set<int>& skinned_mesh_indices = {});

    // Per-frame: sync each entity's transform to its animated instance.
    void update_transforms(const BackendCore& core,
                           std::span<const Instance> instances,
                           std::span<const int> dirty_instance_indices);

    // Remove + destroy all scene entities; clears built().
    void clear(const BackendCore& core);

    [[nodiscard]] bool built() const { return scene_built_; }

    // The renderable entities built with GPU skinning enabled for the given
    // mesh index (an Animated_Object), in build order — a mesh that splits
    // into a shadow-caster + non-caster renderable reports both. Empty for an
    // unknown / non-skinned mesh index. Used by the per-frame bone update
    // (FilamentBackend::update_skinned_bones) to route bone matrices to every
    // renderable of an Animated_Object.
    [[nodiscard]] std::span<const utils::Entity> skinned_entities(int mesh_index) const;

private:
    // Emit the one-time per-group instancing fallback diagnostic for a mesh
    // that degraded from the instanced path to the per-instance path,
    // deduplicated via instancing_fallback_warned_. Defined out-of-line so the
    // shipping-path build() body carries no debug-output call (codebase-hardening
    // structural rule).
    void warn_instancing_fallback(int mesh_index,
                                  std::span<const GpuMesh> gpu_meshes);

    // Emit the one-time global "engine cannot instance" diagnostic when
    // getMaxAutomaticInstances() < 2 and the whole instancing path degrades to
    // the per-instance loop. Also kept out-of-line per.
    void warn_instancing_unavailable(std::size_t max_automatic_instances);

    struct SceneEntity {
        utils::Entity entity;
        int           instance_index;  // index into the instances span
        bool prepared_culling = false;
        bool base_culling = false;
        bool culling_enabled = false;
        PreparedSurfaceCullingBounds culling_bounds{};
    };
    std::vector<SceneEntity> scene_entities_;

    // mesh_index → renderable entities built with.skinning() enabled
    // (caster + non-caster). Per-frame bone updates look this up so every
    // renderable of an Animated_Object receives the same bone matrices.
    std::unordered_map<int, std::vector<utils::Entity>> skinned_entities_;

    // One instanced group chunk: up to two renderables (the Shadow_Split
    // caster + non-caster) sharing one InstanceBuffer, plus the member instance
    // indices. The buffer holds the per-instance local transforms (world
    // placement); the renderables' own transforms are identity. The per-frame
    // sync (update_transforms) refreshes the buffer's transforms from these
    // member indices; clear() destroys the buffer and entities. A null entity
    // means that Shadow_Split subset was empty for this chunk.
    struct InstancedChunk {
        utils::Entity              caster;     // null if no casting prims
        utils::Entity              noncaster;  // null if no non-casting prims
        utils::Entity              foliage_shadow; // null if no shadow-only foliage prims
        filament::InstanceBuffer*  buffer = nullptr; // shared by both entities
        std::vector<int>           instance_indices;  // members, ascending
        std::vector<filament::math::mat4f> local_transforms;
        InstanceLocalBounds local_bounds;
    };
    std::vector<InstancedChunk> instanced_chunks_;

    // Mesh indices already warned about degrading from the instanced path to
    // the per-instance path (e.g. an InstanceBuffer failed to build). Used to
    // emit the per-group fallback diagnostic exactly once per mesh so a failing
    // group does not spam stderr every build.
    std::unordered_set<int> instancing_fallback_warned_;

    // One-shot guard for the global "engine cannot instance" diagnostic emitted
    // when getMaxAutomaticInstances() < 2 and the whole instancing path degrades
    // to the per-instance loop.
    bool instancing_unavailable_warned_ = false;

    bool scene_built_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_SCENE_BUILDER_H_
