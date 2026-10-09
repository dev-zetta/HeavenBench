/*
 * HeavenBench — Null RHI Backend (used-asset packer only)
 *
 * A minimal IRhiBackend that satisfies the renderer's resolution code paths
 * (load_scene_rhi → RhiMaterialDatabase::resolve via try_load) with no GPU and
 * no window. The used-asset packer drives the real world/node/mesh/material/
 * texture resolution through this backend plus an AssetUsageRecorder, so the
 * packed set is byte-for-byte what a render run would load.
 *
 * The backend is deliberately dumb: it does NOT touch the filesystem and it
 * captures nothing itself. upload_mesh forwards the CPU mesh's path/name so the
 * packer can read surface names from the CPU Mesh; load_texture returns a fixed
 * non-zero sentinel so try_load's "did anything load?" check passes; every
 * frame/pipeline method is a trivial no-op. The recorder, threaded into the
 * resolution entry points, performs all capture.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_NULL_BACKEND_H_
#define HEAVENBENCH_SRC_ENGINE_NULL_BACKEND_H_

#include "rhi/rhi.h"

namespace heaven {

class NullBackend final : public IRhiBackend {
public:
    NullBackend() = default;
    ~NullBackend() override = default;

    // ── Identification ──────────────────────────────────────────
    // Only GraphicsApi::Filament exists in the enum; the human-readable
    // identification strings report "null" so a stray diagnostic is obvious.
    [[nodiscard]] GraphicsApi api() const noexcept override { return GraphicsApi::Filament; }
    [[nodiscard]] std::string device_name() const override { return "null"; }
    [[nodiscard]] std::string api_version() const override { return "null"; }
    [[nodiscard]] RendererFeatureStats renderer_feature_stats() const override {
        return {};
    }

    // ── Resource management ─────────────────────────────────────
    // Forward the CPU mesh's path/name with no surface handles — the packer
    // reads surface names from the CPU Mesh, not the GPU handles.
    [[nodiscard]] GpuMesh upload_mesh(const Mesh& mesh) override;

    // Forward the skinned mesh's path/name with no surface handles, exactly
    // like upload_mesh — the packer never enables GPU skinning, it only needs
    // the mesh path available to the disambiguating material resolver.
    [[nodiscard]] GpuMesh upload_skinned(const SkinnedMesh& mesh) override;

    // Return a fixed non-zero sentinel handle without touching the file, so
    // try_load's "did anything load?" check passes for every existing slot.
    [[nodiscard]] uint64_t load_texture(
        const std::string& path,
        TextureSemantic semantic = TextureSemantic::ScalarLinear) override;

    [[nodiscard]] Result<void> init_pipelines(const AppConfig& config) override;

    // ── Frame rendering (no-ops) ────────────────────────────────
    FrameResult render_frame(std::span<const Instance> instances,
                      std::span<const int> dirty_instance_indices,
                      std::span<const GpuMesh> meshes,
                      const MaterialResolver& resolve_material,
                      const Camera& camera,
                      const LightingParams& lighting,
                      const WeatherState& weather,
                      const TonemapParams& tonemap,
                      int viewport_w, int viewport_h) override;

    void present() override;

    Result<void> update_mesh_vertices(uint64_t surface_handle,
                              const Vertex* vertices,
                              uint32_t vertex_count) override;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_NULL_BACKEND_H_
