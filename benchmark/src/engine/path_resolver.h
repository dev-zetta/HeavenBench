/*
 * HeavenBench — Asset path resolution
 *
 * PathResolver is the single component that owns every asset-path
 * construction rule in the renderer. It is constructed once from the
 * configured Data_Root (AppConfig::data_dir) and exposes named methods
 * for each asset category — textures, the ordered texture search-dir
 * list and suffix family, meshes, scene files, the tessellation cache
 * directory, and the camera-track directory.
 *
 * Every method returns a string byte-for-byte identical to the inline
 * concatenation the corresponding call site produces today: the
 * resolver reproduces the legacy concatenation verbatim and never
 * trims, deduplicates, collapses separators, changes case, or adds or
 * removes a trailing separator. This is a pure CPU-side value type with
 * no Filament/GPU dependency — the same tier as engine/types.h — so it
 * is fully host-testable.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_PATH_RESOLVER_H_
#define HEAVENBENCH_SRC_ENGINE_PATH_RESOLVER_H_

#include "engine/types.h"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace heaven {

class PathResolver;
using PathResolverRef =
    std::optional<std::reference_wrapper<const PathResolver>>;

// The seven texture categories the renderer scans, in the fixed order used by
// convention-based auto-discovery. The enum is the single definition of "which
// texture directories exist"; the fragment strings live in the.cpp.
enum class TextureCategory {
    Environment,   // heaven/textures/environment/
    Buildings,     // heaven/textures/buildings/
    Ruins,         // heaven/textures/ruins/
    Props,         // heaven/textures/props/
    Airship,       // heaven/textures/airship/
    Sfx,           // heaven/textures/sfx/
    Core,          // core/textures/
};

// The one mesh subdirectory referenced by explicit (non-parsed) call sites.
enum class MeshCategory {
    Environment,   // heaven/meshes/environment
};

// Renderer-owned assets that are not part of the extracted Heaven archives.
// The asset packer places these under Data_Root/sky in release bundles.
enum class SkyAsset {
    MoonDisk,
    MoonNormal,
    MilkyWay,
};

inline constexpr std::array<SkyAsset, 3> kSkyAssets = {
    SkyAsset::MoonDisk,
    SkyAsset::MoonNormal,
    SkyAsset::MilkyWay,
};

// Renderer-owned startup image installed by the asset packer under Data_Root.
inline constexpr std::string_view kLoadingScreenRelPath =
    "loading/loading_screen.png";

class PathResolver {
public:
    // ── Construction ──
    // Reject an empty or whitespace-only Data_Root with an Error; otherwise
    // store the value verbatim as the base prefix for every Resolved_Path.
    [[nodiscard]] static Result<PathResolver> create(std::string data_root);

    [[nodiscard]] const std::string& data_root() const noexcept { return data_root_; }

    // ── Textures ──
    // Data_Root + category fragment + filename, ensuring a single trailing
    // ".dds" (appended only when the final 4 chars are not already ".dds",
    // matched case-sensitively).
    [[nodiscard]] std::string texture_path(TextureCategory category,
                                           std::string_view filename) const;

    // ── Texture search-directory enumeration ──
    // The seven resolved search directories (each ending in "/"), in fixed
    // order, identical across invocations for the same Data_Root.
    [[nodiscard]] const std::array<std::string, 7>& texture_search_dirs() const noexcept;

    // ── Texture suffix family ──
    // The seven role suffixes in fixed order: _d _n _s _h _t _e _dd.
    [[nodiscard]] static std::span<const std::string_view> texture_suffixes() noexcept;

    // ── Auto-discovery candidate ──
    // search_dir + base + suffix + ".dds". `search_dir` is one of the resolved
    // strings from texture_search_dirs(); no separator is inserted (the search
    // dir already ends in "/"), matching the legacy `dir + name + suffix +
    // ".dds"` form exactly.
    [[nodiscard]] std::string texture_candidate(std::string_view search_dir,
                                                std::string_view base,
                                                std::string_view suffix) const;

    // ── Meshes ──
    // Data_Root + mesh subdirectory + mesh_name, mesh_name used verbatim
    // (extension neither appended nor stripped), no existence check.
    [[nodiscard]] std::string mesh_path(MeshCategory subdir,
                                        std::string_view mesh_name) const;

    // ── Scene files ──
    // Data_Root + "/heaven/heaven.world".
    [[nodiscard]] std::string world_path() const;

    // ── Music ──
    // Data_Root + "/" + k_music_rel_path (== relative_path(k_music_rel_path)),
    // so the background-music track obeys the same verbatim Data_Root join as
    // every other relative asset and resolves identically in the renderer and
    // the packer.
    [[nodiscard]] std::string music_path() const;

    // Data_Root + "/sky/" + the canonical filename for the renderer-owned sky
    // asset. These files are installed by --pack-assets; runtime code never
    // searches the source repository for them.
    [[nodiscard]] std::string sky_asset_path(SkyAsset asset) const;
    [[nodiscard]] static std::string_view sky_asset_filename(
        SkyAsset asset) noexcept;

    // Data_Root + "/heaven/nodes/" + filename; rejects empty/whitespace
    // filename with an Error and emits no partial path.
    [[nodiscard]] Result<std::string> node_path(std::string_view filename) const;

    // ── Cache / track directories ──
    // Data_Root + "/cache/tessellation" — verbatim, no separator handling.
    [[nodiscard]] std::string cache_dir() const;
    // Data_Root + "/heaven/tracks" — verbatim, no separator handling.
    [[nodiscard]] std::string track_dir() const;

    // ── Data-root-relative joins ──
    // Data_Root + "/" + rel. For asset references already stored as paths
    // relative to Data_Root (texture paths in MaterialDef::textures, the mesh
    // `rel` parsed from a node, a cloth source-node path). No extension logic.
    [[nodiscard]] std::string relative_path(std::string_view rel) const;

    // Data_Root + "/" + rel, then ensure a single trailing ".dds" (same rule
    // as texture_path). Used by displacement-map resolution in the material DB.
    [[nodiscard]] std::string displacement_path(std::string_view rel) const;

private:
    explicit PathResolver(std::string data_root);

    std::string data_root_;
    std::array<std::string, 7> search_dirs_;  // precomputed once in the ctor
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_PATH_RESOLVER_H_
