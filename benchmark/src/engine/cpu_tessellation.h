/*
 * HeavenBench — CPU tessellation topology generation
 *
 * Builds crack-free high-poly topology from the authored tessellation-density
 * map. Height displacement remains in the vertex shader so its distance fade
 * and mip selection can follow the camera.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_CPU_TESSELLATION_H_
#define HEAVENBENCH_SRC_ENGINE_CPU_TESSELLATION_H_

#include "engine/asset_file_system.h"
#include "engine/types.h"
#include "engine/skinned_mesh.h"
#include <cstdint>
#include <array>
#include <string>

namespace heaven {

inline constexpr uint32_t kTessellationCacheVersion = 34;
inline constexpr int kMaxTessellationSubdivisions = 3;

// TessellationTopologyParameters is the typed, pointer-free description of
// one CPU subdivision request, including its material UV and seam policy.
struct TessellationTopologyParameters {
    float density_factor = 1.0f;
    float edge_scale = 1.0f;
    int subdivisions = 2;
    std::array<float, 4> base_transform{1.0f, 1.0f, 0.0f, 0.0f};
    float boundary_fade = 0.0f;
};

//CPU-tessellate a mesh surface using its authored density map.

//@param surface       The surface to tessellate (modified in place)
//@param density_path  Path to the DDS tessellation-density map
//@param density_factor Authored maximum per-edge tessellation factor
//@param edge_scale    Authored edge-length multiplier
//@param subdivisions  Number of subdivision levels (each level 4x triangles)
//@param base_transform Per-material UV transform applied before density
//sampling: `uv = uv*xy + zw`. Mirrors the
//`base_transform` parameter Heaven authors on
//many materials (e.g. ruins_merlon =
//vec4(2,1,-1,0)). Default identity {1,1,0,0}.
bool cpu_tessellate_surface(Surface& surface,
                            const std::string& density_path,
                            const TessellationTopologyParameters& topology,
                            AssetFileSystemRef assets = std::nullopt);

//CPU-tessellate a SKINNED surface. Same algorithm as the static
//version but barycentrically interpolates bone weights & indices
//at each new midpoint vertex so the runtime per-frame skinning
//follows the right bone(s). No seam handling — skinned objects
//in this scene (gears, weathercock, mill wings) are standalone
//items, not tiled with neighbouring static surfaces.

//Operates on the smesh's bind-pose vertex data; the calling
//pipeline must re-skin the new vertex set on the next frame.
//@param surface       Skinned surface to tessellate (modified in place)
//@param density_path  Path to the DDS tessellation-density map
//@param density_factor Authored maximum per-edge tessellation factor
//@param edge_scale    Authored edge-length multiplier
//@param subdivisions  Number of subdivision levels (each = 4x triangles)
//@param base_transform See cpu_tessellate_surface for semantics.
bool cpu_tessellate_skinned_surface(SkinnedSurface& surface,
                                    const std::string& density_path,
                                    const TessellationTopologyParameters& topology,
                                    AssetFileSystemRef assets = std::nullopt);

//Check if a cached tessellated mesh exists on disk.
//Cache key includes source-content fingerprints and every topology parameter.
bool has_tessellation_cache(const std::string& mesh_path,
                            const std::string& surface_name,
                            const std::string& density_path,
                            float density_factor,
                            float edge_scale,
                            int subdivisions,
                            const std::string& cache_dir,
                            const float base_transform[4] = nullptr);

//Save a tessellated surface to the disk cache.
void save_tessellation_cache(const Surface& surface,
                             const std::string& mesh_path,
                             const std::string& surface_name,
                             const std::string& density_path,
                             float density_factor,
                             float edge_scale,
                             int subdivisions,
                             const std::string& cache_dir,
                             const float base_transform[4] = nullptr,
                             uint64_t source_vertex_count = 0,
                             uint64_t source_index_count = 0);

//Load a tessellated surface from the disk cache.
//Returns true if loaded successfully.
bool load_tessellation_cache(Surface& surface,
                             const std::string& mesh_path,
                             const std::string& surface_name,
                             const std::string& density_path,
                             float density_factor,
                             float edge_scale,
                             int subdivisions,
                             const std::string& cache_dir,
                             const float base_transform[4] = nullptr);

//Remove cached tessellation files left over from previous algorithm
//versions. Files are named "tess_v<N>_<hash>.bin"; anything whose
//version prefix doesn't match the current one is deleted.
//Returns the number of files deleted.
int prune_stale_tessellation_cache(const std::string& cache_dir);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_CPU_TESSELLATION_H_
