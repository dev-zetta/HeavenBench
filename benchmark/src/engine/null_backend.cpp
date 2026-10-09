/*
 * HeavenBench — Null RHI Backend (used-asset packer only)
 *
 * See null_backend.h for the rationale. Every method here is a trivial stub:
 * the packer never renders, so the only behaviours that matter are
 *   - upload_mesh forwarding the CPU mesh's path/name, and
 *   - load_texture returning a non-zero sentinel,
 * which together let the resolution code (load_scene_rhi + RhiMaterialDatabase)
 * run unchanged with no GPU and no window.
 */
#include "engine/null_backend.h"
#include "engine/skinned_mesh.h"

namespace heaven {

GpuMesh NullBackend::upload_mesh(const Mesh& mesh) {
    // Carry the path/name so the disambiguating material resolver still has the
    // mesh path available; leave surfaces empty because the packer iterates the
    // CPU Mesh's surfaces (with real names) rather than the GPU handles.
    GpuMesh gpu;
    gpu.name = mesh.name;
    gpu.path = mesh.path;
    return gpu;
}

GpuMesh NullBackend::upload_skinned(const SkinnedMesh& mesh) {
    // Same as upload_mesh: forward the path/name only. The packer reads
    // surface names from the CPU side and never renders, so no skinned GPU
    // buffers are needed.
    GpuMesh gpu;
    gpu.name = mesh.name;
    gpu.path = mesh.path;
    return gpu;
}

uint64_t NullBackend::load_texture(
    const std::string& /*path*/, TextureSemantic /*semantic*/) {
    // Fixed non-zero sentinel: try_load only checks whether *something* loaded
    // (handle != 0). The on-disk existence check it performs before calling
    // this is what gates a slot; we never open the file ourselves.
    return 1;
}

Result<void> NullBackend::init_pipelines(const AppConfig& /*config*/) {
    return {};
}

FrameResult NullBackend::render_frame(std::span<const Instance> /*instances*/,
                               std::span<const int> /*dirty_instance_indices*/,
                               std::span<const GpuMesh> /*meshes*/,
                               const MaterialResolver& /*resolve_material*/,
                               const Camera& /*camera*/,
                               const LightingParams& /*lighting*/,
                               const WeatherState& /*weather*/,
                               const TonemapParams& /*tonemap*/,
                               int /*viewport_w*/, int /*viewport_h*/) {
    // No rendering in packer mode.
    return {};
}

void NullBackend::present() {
    // No swapchain in packer mode.
}

Result<void> NullBackend::update_mesh_vertices(uint64_t /*surface_handle*/,
                                       const Vertex* /*vertices*/,
                                       uint32_t /*vertex_count*/) {
    // No GPU mesh to update in packer mode.
    return {};
}

} // namespace heaven
