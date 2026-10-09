/*
 * HeavenBench — Mesh store
 *
 * Owns GPU mesh upload (tangent-frame quaternion computation via
 * SurfaceOrientation, the interleaved position/UV + tangent vertex
 * buffer layout, the index buffer) and the per-surface CPU-skinning
 * re-upload path. Mesh resources are keyed by surface handle so the
 * scene builder can look up a surface's VB/IB when assembling
 * renderables.
 *
 * Extracted verbatim out of FilamentBackend::upload_mesh /
 * update_mesh_vertices.
 */
#ifndef HEAVENBENCH_SRC_RHI_MESH_STORE_H_
#define HEAVENBENCH_SRC_RHI_MESH_STORE_H_

#include "rhi/rhi.h"               // GpuMesh
#include "engine/types.h"          // Mesh, Vertex
#include "engine/spatial_draw_partition.h"
#include "rhi/packed_static_mesh.h"
#include "rhi/displacement_surface_preparer.h"
#include "engine/skinned_mesh.h"   // SkinnedMesh

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace filament {
class Engine;
class VertexBuffer;
class IndexBuffer;
}

namespace heaven {

class MeshStore {
public:
    explicit MeshStore(filament::Engine& engine) : engine_(engine) {}
    ~MeshStore();

    // Upload a CPU-side mesh; returns the GpuMesh handle bundle.
    [[nodiscard]] GpuMesh upload(const Mesh& mesh);

    // The caller first destroys every renderable/depth proxy borrowing this
    // mesh. Remove its surface records as well as its actual Filament owners;
    // other meshes stay live and stale handles cannot resolve replacements.
    void release(const GpuMesh& mesh);

    // Upload a CPU-side skinned (ms08) mesh in its REST pose for GPU
    // skinning. The vertex buffer carries, per surface:
    //   buffer 0: position (float3) + uv (float2)  — rest-pose positions
    //   buffer 1: tangents (short4)                 — rest-pose SurfaceOrientation
    //   buffer 2: BONE_INDICES (ushort4, integer)   — SkinnedVertex.bone_indices
    //   buffer 3: BONE_WEIGHTS (float4)             — SkinnedVertex.bone_weights
    // The index buffer and per-surface handle/AABB bookkeeping are
    // identical to upload(), so the rest of the pipeline (material
    // resolve, scene build, duplicate hiding) is unchanged. Each
    // uploaded surface is additionally marked skinned with the mesh's
    // bone count so SceneBuilder can enable.skinning() on its
    // renderable and the per-frame bone update can target it.

    // Filament's native skinning supports up to 255 bones. If the mesh
    // exceeds that limit the geometry is still uploaded (so the object
    // can render via the CPU-skinning fallback), but the surfaces are
    // NOT marked skinned (bone_count == 0) and a one-time diagnostic is
    // emitted — the per-object CPU fallback path keys off this missing
    // marker. A mesh with zero bones is likewise left unmarked.
    [[nodiscard]] GpuMesh upload_skinned(const SkinnedMesh& mesh);

    // Per-surface GPU-skinning marker recorded by upload_skinned. A
    // surface is `skinned` only when Filament-native skinning was
    // enabled for it (bone count in range); `bone_count` is then the
    // number of bone transforms the renderable must be built with and
    // updated each frame. Unknown handles and statically-uploaded
    // surfaces report `{false, 0}`.
    struct SkinnedInfo {
        bool     skinned    = false;
        uint32_t bone_count = 0;
        // Animated source identity survives GPU disable and bone-limit fallback;
        // its rest-pose bounds do not bound CPU-deformed vertices.
        bool skinned_source = false;
    };
    [[nodiscard]] SkinnedInfo skinned_info(uint64_t surface_handle) const;

    // Re-upload vertex data for an existing surface (CPU skinning).
    // The incoming count must match the count recorded when the surface
    // was uploaded; a mismatch is rejected with an Error (and no buffer
    // is touched) rather than corrupting the vertex buffer.
    // This is a deformation-only update: UV0 and displacement-normal magnitude
    // remain those of the bind mesh. Changing either, or changing topology,
    // requires a full upload so the shared height-sampling footprint is rebuilt.
    [[nodiscard]] Result<void> update_vertices(uint64_t surface_handle,
                         const Vertex* vertices, uint32_t vertex_count);

    // Startup-only: transfer the original uploaded frame/custom bytes. P/UV
    // fields are checked against the immutable source signature, never re-packed.
    [[nodiscard]] Result<PackedStaticSurface> take_original_packing(
        uint64_t handle, std::span<const Vertex> vertices, std::span<const uint32_t> indices);
    void discard_original_packing();
    [[nodiscard]] std::optional<size_t> source_surface_index(uint64_t handle) const;
    [[nodiscard]] bool is_displacement_prepared(uint64_t handle) const;
    // Stable until the startup replacement or this store is destroyed. Only
    // prepared surfaces publish spatial ranges; all ranges share find()'s VB/IB.
    [[nodiscard]] std::span<const SpatialDrawRange> prepared_draw_ranges(uint64_t handle) const;

    // Call before scene/depth renderables borrow these buffers. The handle is
    // stable, both passes use the replacement, and CPU deformation is rejected.
    [[nodiscard]] Result<uint32_t> replace_displacement_surface(
        GpuSurface& surface, std::span<const Vertex> vertices,
        const PreparedDisplacementSurface& prepared);

    // VB/IB for a surface handle. Both pointers are null when the
    // handle is unknown or has no buffers.
    struct SurfaceBuffers {
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer*  ib = nullptr;
    };
    [[nodiscard]] SurfaceBuffers find(uint64_t surface_handle) const;

private:
    filament::Engine& engine_;
    uint64_t next_mesh_id_ = 1;

    // Each uploaded surface gets a set of Filament resources, keyed by
    // surface handle.
    struct MeshData {
        std::vector<filament::VertexBuffer*> vertex_buffers;
        std::vector<filament::IndexBuffer*>  index_buffers;
        // Vertex count recorded at upload time so update_vertices can
        // reject a mismatched re-upload before it corrupts the buffer.
        uint32_t vertex_count = 0;
        // Compact guard for the immutable UV0 / normal-magnitude attributes.
        // CPU deformation leaves the CUSTOM0 GPU buffer intact; no permanent
        // CPU copy of the static mesh's UVs, indices or footprints is needed.
        uint64_t deformation_signature = 0;
        uint64_t source_signature = 0;
        size_t source_surface_index = 0;
        std::vector<filament::math::short4> original_tangents;
        std::vector<filament::math::float4> original_custom;
        bool prepared_displacement = false;
        std::vector<SpatialDrawRange> prepared_ranges;
        StaticTangentPacking original_tangent_packing = StaticTangentPacking::Unavailable;
        // GPU-skinning marker (set by upload_skinned). `skinned` is true
        // only when Filament-native skinning was enabled for this surface
        // (bone count in [1, 255]); `bone_count` is then the bone-transform
        // count for.skinning()/setBones. Static surfaces leave both default.
        bool     skinned    = false;
        uint32_t bone_count = 0;
        // Retain the source type independently of the GPU bone-count limit:
        // CPU fallback still uploads the skin's UV-aligned tangent frames.
        bool skinned_source = false;
    };
    std::unordered_map<uint64_t, MeshData> mesh_data_;

    // One-time diagnostic guard for skinned meshes whose bone count
    // exceeds Filament's 255-bone native-skinning limit, so the warning
    // is not re-emitted on every tessellation re-upload of the same mesh.
    std::unordered_set<std::string> bone_overflow_warned_;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_MESH_STORE_H_
