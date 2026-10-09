/*
 * HeavenBench — Grass depth-proxy helper
 *
 * Procedural grass (GrassFeature / MassiveFoliageFeature) is generated as the
 * features' own vertex/index buffers and rendered straight into the main view.
 * It is NOT a scene instance, so DepthProxyBuilder never writes it into the
 * linear scene-depth texture. As a result the screen-space depth consumers
 * (volumetric clouds, volumetric fog, god rays, SSGI, rain splash) read the
 * terrain/sky depth BEHIND the blades and composite in front of them — most
 * visibly, distant clouds drawing over the grass silhouette at the horizon.
 *
 * This helper closes that gap: given a grass patch's existing VB/IB and world
 * transform, it builds a depth-only proxy renderable (heaven_grass_depth
 * material) into the depth pre-pass proxy Scene, REUSING the visible patch's
 * buffers (no extra geometry, no per-frame regeneration — AGENTS.md). The
 * depth material replicates heaven_grass.mat's wind animation so the silhouette
 * tracks the swaying blades. Shared by both grass features so they stay in
 * lockstep.
 */
#ifndef HEAVENBENCH_SRC_RHI_GRASS_DEPTH_PROXY_H_
#define HEAVENBENCH_SRC_RHI_GRASS_DEPTH_PROXY_H_

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <utils/Entity.h>

namespace filament {
class Engine;
class Scene;
class VertexBuffer;
class IndexBuffer;
class Material;
class MaterialInstance;
class Texture;
}

namespace heaven {

class IResourceProvider;
class PathResolver;
struct GrassPatchBounds;

class GrassDepthProxy {
public:
    // Load the heaven_grass_depth material and bind the grass diffuse atlas
    // (its alpha is the cutout). Best-effort: on failure ready() stays false
    // and the caller simply gets no depth proxies (the prior behaviour).
    void init(filament::Engine* engine, IResourceProvider& resources,
              const PathResolver& paths);

    // Explicit atlas for a procedural scene. Borrowed for the proxy lifetime,
    // exactly as the ordinary resource-store atlas; no texture is owned here.
    void init(filament::Engine* engine, const filament::Texture* diffuse);

    [[nodiscard]] bool ready() const { return material_ != nullptr && mi_ != nullptr; }
    [[nodiscard]] size_t size() const { return entities_.size(); }

    // Build a depth-proxy renderable for one grass patch, sharing its vb/ib,
    // added to `depth_scene` with the given world transform. The proxy index
    // equals the call order (so set_transform(i,...) matches the i-th add).
    // No-op when not ready() or depth_scene is null.
    // A renderable build failure throws before appending a proxy, so callers
    // cannot continue with a shifted visible/depth patch correspondence.
    void add_patch(filament::Engine* engine, filament::Scene* depth_scene,
                   filament::VertexBuffer* vb, filament::IndexBuffer* ib,
                   std::uint32_t index_offset, std::uint32_t index_count,
                   const glm::mat4& transform, const GrassPatchBounds& bounds);

    // Keep a proxy's bounds identical to its visible patch after scale/rotation.
    void set_bounds(filament::Engine* engine, size_t i, const GrassPatchBounds& bounds);

    // Per-frame: refresh the shared wind `time` uniform (matches heaven_grass).
    void set_time(float time);

    // Per-frame: sync proxy `i`'s transform with its island instance.
    void set_transform(filament::Engine* engine, size_t i, const glm::mat4& transform);

    // Toggle a patch for distance LOD without rebuilding its renderable.
    void set_visible(size_t i, bool visible);

    // Remove + destroy all proxy entities (and the material/instance). Safe to
    // call with a null engine; uses the depth Scene captured at add_patch time.
    void destroy(filament::Engine* engine);

private:
    filament::Material*         material_    = nullptr;
    filament::MaterialInstance* mi_          = nullptr;  // shared: atlas + time
    filament::Scene*            depth_scene_ = nullptr;  // captured on first add
    std::vector<utils::Entity>  entities_;
    std::vector<bool>           visible_;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_GRASS_DEPTH_PROXY_H_
