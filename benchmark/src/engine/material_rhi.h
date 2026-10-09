/*
 * HeavenBench — RHI-aware material resolver
 *
 * Wraps MaterialDatabase to produce GpuMaterial with opaque texture handles
 * loaded through the RHI backend.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_MATERIAL_RHI_H_
#define HEAVENBENCH_SRC_ENGINE_MATERIAL_RHI_H_

#include "engine/asset_file_system.h"
#include "engine/types.h"
#include "engine/path_resolver.h"
#include "rhi/rhi.h"

#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace heaven {

class AssetUsageRecorder;
[[nodiscard]] TextureSemantic texture_semantic_for_binding(
    std::string_view suffix,
    std::string_view authored_format,
    bool auto_discovered);

class RhiMaterialDatabase {
public:
    RhiMaterialDatabase() = default;

    //Build the material database (parses.mat files, resolves inheritance).
    //The trailing AssetUsageRecorder* is an optional, default-null tap used by
    //the used-asset packer: when non-null, build records each parsed material
    //name → its defining.mat file in `material_source_file_` so a later
    //resolve() can report the provenance of only the USED subset. build itself
    //marks NO file as used, and a null recorder leaves behavior (and cost)
    //byte-for-byte unchanged for a normal render run.
    void build(const PathResolver& paths,
               AssetUsageRecorder* rec = nullptr,
               AssetFileSystemRef assets = std::nullopt);

    // Drop resolved GPU texture handles while retaining parsed material and
    // surface ownership. Texture-quality reloads invalidate TextureStore
    // handles and force lazy resolution through the newly selected tier.
    void clear_resolved_cache() noexcept;

    //Resolve a surface name to a GpuMaterial using the RHI backend for textures.
    //The trailing AssetUsageRecorder* defaults to null (render-run behavior is
    //unchanged); when supplied it records the resolved material's source.mat
    //plus every.mat in its parent chain, and each texture file that loads.
    [[nodiscard]] GpuMaterial resolve(const std::string& surface_name,
                                      const PathResolver& paths,
                                      IRhiBackend& backend,
                                      AssetUsageRecorder* rec = nullptr);

    //Mesh-scoped overload: many Heaven sub-meshes share placeholder
    //surface names (Object04, Box05,...) and binding to material is
    //only unambiguous given the specific (mesh_path, surface_name)
    //pair. The plain `resolve(surface_name,...)` overload above
    //falls back to the global map and may return the wrong material
    //for shared names — prefer this overload from scene loading.
    [[nodiscard]] GpuMaterial resolve(const std::string& mesh_path,
                                      const std::string& surface_name,
                                      const PathResolver& paths,
                                      IRhiBackend& backend,
                                      AssetUsageRecorder* rec = nullptr);

    [[nodiscard]] int hits()   const { return tex_hit_; }
    [[nodiscard]] int misses() const { return tex_miss_; }
    [[nodiscard]] const std::vector<std::string>& missed_surfaces() const { return tex_misses_; }

    //Query the displacement texture path for a surface (for CPU tessellation).
    //Returns empty string if no height/tessellation map exists.
    //@param suffix "_h" for height maps, "_t" for tessellation maps
    [[nodiscard]] std::string get_displacement_path(const std::string& surface_name,
                                                     const PathResolver& paths,
                                                     const std::string& suffix) const;

    //Mesh-scoped overload — disambiguates surfaces that share names
    //across meshes. Falls back to the surface-only lookup when no
    //mesh-scoped binding is registered.
    [[nodiscard]] std::string get_displacement_path(const std::string& mesh_path,
                                                     const std::string& surface_name,
                                                     const PathResolver& paths,
                                                     const std::string& suffix) const;

    //Get the resolved per-material tessellation parameters for a surface,
    //walking the parent inheritance chain. Returns the default-constructed
    //MaterialDef (all tess fields == -1) when the surface has no mapping.
    [[nodiscard]] const MaterialDef* get_material_for_surface(
            const std::string& surface_name) const;

    //Mesh-scoped overload — see notes on the resolve(...) overload.
    [[nodiscard]] const MaterialDef* get_material_for_surface(
            const std::string& mesh_path,
            const std::string& surface_name) const;

    // Pure visibility query for source-geometry constraints. Unresolved
    // auto-discovered materials cannot establish a known active neighbor.
    [[nodiscard]] bool is_surface_active(const std::string& mesh_path,
                                        const std::string& surface_name) const;

private:
    bool try_load(GpuMaterial& mat, const MaterialDef& def,
                  const PathResolver& paths, IRhiBackend& backend,
                  AssetUsageRecorder* rec = nullptr,
                  const std::string& referring_mat = "",
                  bool auto_discovered = false);

    // Record a resolved material's source.mat plus every.mat in its parent
    // inheritance chain into the recorder. No-op when rec is
    // null. Provenance comes from material_source_file_, populated by build().
    void record_material_chain(const std::string& mat_name,
                               AssetUsageRecorder* rec) const;

    std::map<std::string, MaterialDef>    all_materials_;
    // Material name → its defining.mat file (on-disk path as scanned). Populated
    // by build() only when a recorder is supplied; lets resolve() report the
    // provenance of the USED subset of materials without marking anything used at
    // build time.
    std::map<std::string, std::string>    material_source_file_;
    std::map<std::string, std::string>    surface_to_material_;
    // Mesh-scoped surface→material map. The key is (mesh_path,
    // surface_name). This is preferred over `surface_to_material_`
    // because surface names in the Heaven assets are not unique
    // across meshes (placeholders like "Object04", "Box05",...).
    std::map<std::pair<std::string, std::string>, std::string>
                                          mesh_surface_to_material_;
    // Mesh-scoped disabled and far-LOD surfaces. The renderer deliberately
    // keeps the authored near/high-detail surfaces active at every distance,
    // so these must not overlap them.
    std::set<std::pair<std::string, std::string>> inactive_surfaces_;
    // Cache keyed by (mesh_path, surface_name) so the per-frame
    // resolve() in the render loop is a single hash lookup, not a
    // string concat + ordered-map walk. The previous design did
    // `cache_[mesh_path + "::" + surface_name]` which allocated a
    // fresh std::string on every call — at 1000+ surfaces per
    // frame × 3000 FPS that was millions of allocations per
    // second, halving overall FPS.
    struct PairHash {
        size_t operator()(const std::pair<std::string, std::string>& p) const noexcept {
            size_t h = std::hash<std::string>{}(p.first);
            h ^= std::hash<std::string>{}(p.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<std::pair<std::string, std::string>, GpuMaterial, PairHash>
                                          cache_;
    // Surface-only fallback cache (used by the plain
    // resolve(surface_name) overload).
    std::unordered_map<std::string, GpuMaterial>
                                          cache_surface_;

    int tex_hit_ = 0, tex_miss_ = 0;
    std::vector<std::string> tex_misses_;
    AssetFileSystemRef assets_;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_MATERIAL_RHI_H_
