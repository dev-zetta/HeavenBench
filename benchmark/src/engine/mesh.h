/*
 * HeavenBench — Mesh loading (mi08)
 *
 * Pure CPU-side mesh data (Mesh, Surface, Vertex live in
 * engine/types.h) plus the mi08 binary loader. GPU upload is
 * the backend's responsibility — see IRhiBackend::upload_mesh
 * in rhi/rhi.h.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_MESH_H_
#define HEAVENBENCH_SRC_ENGINE_MESH_H_

#include "engine/asset_file_system.h"
#include "engine/types.h"

namespace heaven {

//Load a mesh from the mi08 binary format.
[[nodiscard]] Result<Mesh> load_mi08(
    const std::string& path,
    AssetFileSystemRef assets = std::nullopt);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_MESH_H_
