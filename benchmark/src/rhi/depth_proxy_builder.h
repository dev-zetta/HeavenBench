/*
 * HeavenBench — Depth proxy builder
 *
 * Builds a parallel set of lightweight "proxy" renderables that mirror the
 * visible scene's geometry but render a single depth-output material, used by
 * the DepthPrePass to produce the sampleable linear scene-depth texture.
 *
 * Each proxy renderable SHARES its paired main-scene renderable's vertex and
 * index buffers (no geometry is duplicated — only a lightweight renderable
 * entity is added) and uses one shared `heaven_depth` MaterialInstance. The
 * proxies select EXACTLY the same primitives the visible scene does (via the
 * shared select_renderable_prims() helper), so the depth silhouette matches
 * what is on screen.
 *
 * Like SceneBuilder, proxies are built ONCE and only have their transforms
 * synced to the animated instance list each frame — never regenerated
 * Sharing the main mesh's VertexBuffer handle means skinned /
 * animated meshes that MeshStore re-uploads each frame are reflected in the
 * proxy automatically.
 *
 * The owning depth-output Material/MaterialInstance is created and owned by
 * DepthPrePass (it lives next to the F target); this builder receives the
 * shared MaterialInstance as a parameter to build().
 */
#ifndef HEAVENBENCH_SRC_RHI_DEPTH_PROXY_BUILDER_H_
#define HEAVENBENCH_SRC_RHI_DEPTH_PROXY_BUILDER_H_

#include "rhi/rhi.h"            // Instance, GpuMesh, MaterialResolver
#include "rhi/backend_core.h"
#include "rhi/mesh_store.h"
#include "rhi/prepared_surface_culling.h"
#include "rhi/instance_bounds.h"

#include <utils/Entity.h>
#include <math/mat4.h>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace filament {
class Scene;
class MaterialInstance;
class Material;
class InstanceBuffer;
}

namespace heaven {

class TextureStore;

class DepthProxyBuilder {
public:
    // Build proxies sharing the main scene's VB/IB into `proxy_scene`. Runs
    // once (sets built()). Static bounds and culling match SceneBuilder; a
    // prepared surface has its own independently cullable proxy per placement.

    // Both opaque and alpha-tested surfaces use cached per-surface instances,
    // carrying the same height texture and displacement parameters as visible
    // geometry. Masked instances additionally bind the foliage diffuse alpha.

    // When `gpu_instancing` is true the proxies mirror SceneBuilder's instanced
    // build: Instances that share a `mesh_index` (and are neither skinned nor
    // singletons) are grouped via the SAME helper SceneBuilder uses — identical
    // `instances`, skinned exclusions, and
    // `max_per_group = engine->getMaxAutomaticInstances()` — and each group
    // chunk is drawn as ONE instanced depth-proxy renderable (no Shadow_Split
    // here: the depth pass neither casts nor receives shadows) built with
    // RenderableManager::Builder::instances(chunkSize, InstanceBuffer*). The
    // shared, pure, deterministic helper guarantees the depth chunks/members
    // match the visible instanced scene exactly, so the linear-depth silhouette
    // stays in lockstep. `skinned_mesh_indices` is the SAME set
    // SceneBuilder is given so the two builders cannot drift. Singletons,
    // skinned meshes, and fallbacks go through the per-instance proxy path.
    // Prepared primitives are omitted from groups and get separate per-instance
    // proxies; ordinary neighbours of those primitives remain instanced.
    void build(const BackendCore& core, MeshStore& meshes,
               filament::Material*         depth_material,
               filament::Material*         masked_material,
               TextureStore&               textures,
               filament::Scene* proxy_scene,
               std::span<const Instance> instances,
               std::span<const GpuMesh>  gpu_meshes,
               const MaterialResolver&   resolve_material,
               float tessellation_scale,
               float tessellation_distance_multiplier,
               bool gpu_instancing = false,
               const std::unordered_set<int>& skinned_mesh_indices = {},
               bool gpu_skinning = false,
               bool tessellation_enabled = true);

    // Per-frame: sync each proxy's transform to its paired instance so the
    // depth scene tracks the animated visible scene (floating islands, skinned
    // meshes).
    void update_transforms(const BackendCore& core,
                           std::span<const Instance> instances,
                           std::span<const int> dirty_instance_indices);

    // Remove + destroy all proxy entities from `proxy_scene`; clears built().
    void clear(const BackendCore& core, filament::Scene* proxy_scene);

    [[nodiscard]] bool built() const { return built_; }

    // The depth-proxy renderable entities built with GPU skinning enabled for
    // the given mesh index (an Animated_Object), in build order. A GPU-skinned
    // mesh's visible renderable is deformed from per-frame bone matrices while
    // its vertex buffer stays at the rest pose; the depth proxy shares that
    // rest-pose VB, so it must be GPU-skinned too and receive the SAME bone
    // matrices, or the linear-depth silhouette would sit at the rest pose while
    // the visible mesh animates (and the screen-space depth consumers would
    // composite atmosphere in front of the moving mesh). Used by the per-frame
    // bone update (FilamentBackend::update_skinned_bones) to route the same
    // matrices it pushes to the visible renderables. Empty for an unknown /
    // non-skinned mesh index.
    [[nodiscard]] std::span<const utils::Entity> skinned_proxy_entities(
        int mesh_index) const;

private:
    struct ProxyEntity {
        utils::Entity entity;
        int           instance_index;  // index into the instances span
        bool prepared_culling = false;
        bool base_culling = false;
        bool culling_enabled = false;
        PreparedSurfaceCullingBounds culling_bounds{};
    };
    std::vector<ProxyEntity> proxies_;

    // One instanced depth-proxy chunk: a single renderable (depth has no
    // Shadow_Split) drawing every member of a group via its own InstanceBuffer,
    // plus the member instance indices for the per-frame transform sync. The
    // buffer holds the per-instance local transforms (world placement); the
    // renderable's own transform is identity. clear() destroys the entity and
    // buffer. Mirrors SceneBuilder::InstancedChunk, minus the caster/non-caster
    // split (the depth pass neither casts nor receives shadows).
    struct InstancedProxyChunk {
        utils::Entity              entity;
        filament::InstanceBuffer*  buffer = nullptr;
        std::vector<int>           instance_indices;  // members, ascending
        std::vector<filament::math::mat4f> local_transforms;
        InstanceLocalBounds local_bounds;
    };
    std::vector<InstancedProxyChunk> instanced_chunks_;

    // mesh_index → depth-proxy entities built with.skinning() enabled. The
    // per-frame bone update pushes the SAME bone matrices it sends the visible
    // renderables here so the depth silhouette tracks the animated skinned mesh
    // (otherwise it would sit frozen at the rest pose). Skinned meshes are never
    // instanced, so these are always per-instance proxy renderables.
    std::unordered_map<int, std::vector<utils::Entity>> skinned_proxy_entities_;

    // Mesh indices already warned about degrading from the instanced depth path
    // to the per-instance proxy path (mirrors SceneBuilder's per-group
    // diagnostic, tagged [DepthProxyBuilder] so the logs are distinguishable).
    std::unordered_set<int> instancing_fallback_warned_;

    // One-shot guard for the global "engine cannot instance" depth diagnostic
    // emitted when getMaxAutomaticInstances() < 2 and the whole depth-proxy
    // instancing path degrades to the per-instance loop.
    bool instancing_unavailable_warned_ = false;

    // Per-foliage-diffuse-texture masked depth instances (heaven_depth_masked),
    // created and owned here, deduped by diffuse texture handle so foliage
    // surfaces sharing a map share one instance. Destroyed in clear().
    std::unordered_map<uint64_t, filament::MaterialInstance*> surface_instances_;

    bool built_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_DEPTH_PROXY_BUILDER_H_
