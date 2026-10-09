/*
 * HeavenBench — Asset path resolution (implementation)
 *
 * PathResolver is the single component that owns every asset-path
 * construction rule in the renderer. This translation unit is also the
 * single source of truth for the asset subdirectory fragments: the
 * texture category -> directory fragment map, the mesh subdirectory
 * fragment, the scene-file fragments, and the cache/track directory
 * suffixes are defined here and nowhere else. Every method reproduces
 * the legacy inline concatenation verbatim — it never trims,
 * deduplicates, collapses separators, changes case, or adds/removes a
 * trailing separator.
 */
#include "engine/path_resolver.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace heaven {

namespace {

// ── Single source of truth: texture category -> directory fragment ──

// Each fragment begins with "/" and ends with "/", so a Resolved_Path is
// the verbatim concatenation `data_root + fragment + filename` with no
// inserted separator — matching the legacy call sites exactly. This map is
// also what seeds the precomputed texture search-directory list (one entry
// per category, in enum order).
[[nodiscard]] std::string_view texture_category_fragment(TextureCategory category) {
    switch (category) {
        case TextureCategory::Environment: return "/heaven/textures/environment/";
        case TextureCategory::Buildings:   return "/heaven/textures/buildings/";
        case TextureCategory::Ruins:       return "/heaven/textures/ruins/";
        case TextureCategory::Props:       return "/heaven/textures/props/";
        case TextureCategory::Airship:     return "/heaven/textures/airship/";
        case TextureCategory::Sfx:         return "/heaven/textures/sfx/";
        case TextureCategory::Core:        return "/core/textures/";
    }
    return {};  // unreachable: every enumerator is handled above
}

// ── Single source of truth: mesh subdirectory fragment ──

// Like the texture fragments, this begins and ends with "/" so mesh_path
// is the verbatim concatenation `data_root + fragment + mesh_name`,
// reproducing the legacy `data_dir + "/heaven/meshes/environment/" + name`.
[[nodiscard]] std::string_view mesh_category_fragment(MeshCategory subdir) {
    switch (subdir) {
        case MeshCategory::Environment: return "/heaven/meshes/environment/";
    }
    return {};  // unreachable: every enumerator is handled above
}

// ── Single source of truth: scene-file fragments ──

// world_path() = data_root + k_world_fragment.
// node_path(filename) = data_root + k_nodes_fragment + filename.
constexpr std::string_view k_world_fragment = "/heaven/heaven.world";
constexpr std::string_view k_nodes_fragment = "/heaven/nodes/";
constexpr std::string_view k_sky_asset_fragment = "/sky/";

// ── Single source of truth: cache / track directory suffixes ──

// cache_dir() = data_root + k_cache_suffix; track_dir() = data_root +
// k_track_suffix. Appended verbatim with no separator normalization.
constexpr std::string_view k_cache_suffix = "/cache/tessellation";
constexpr std::string_view k_track_suffix = "/heaven/tracks";

// ── Single source of truth: the texture-role suffix family  ──

// The seven role suffixes in fixed order. This is the ONLY definition of the
// suffix family in the renderer; texture_suffixes() hands callers a span over
// it so convention-based auto-discovery iterates exactly these values, in
// this order, with none added or omitted.
constexpr std::array<std::string_view, 7> k_texture_suffixes = {
    "_d", "_n", "_s", "_h", "_t", "_e", "_dd",
};

// True iff `s` is empty or made up entirely of whitespace characters.
[[nodiscard]] bool is_blank(std::string_view s) {
    return s.empty() ||
           std::all_of(s.begin(), s.end(), [](unsigned char c) {
               return std::isspace(c) != 0;
           });
}

// ── Single source of truth: the ".dds" extension rule  ──

// This is the ONLY place in the renderer that names the ".dds" extension.
// Append ".dds" iff the final four characters are not already ".dds"
// (matched case-sensitively), mirroring the legacy displacement-path logic.
// The rule is idempotent: a path that already ends in ".dds" is returned
// unchanged, so a single extension is guaranteed regardless of the input.
[[nodiscard]] std::string ensure_dds(std::string path) {
    if (path.size() < 4 || path.substr(path.size() - 4) != ".dds")
        path += ".dds";
    return path;
}

} // namespace

// ── Construction  ──

// Reject an empty or whitespace-only Data_Root: such a value cannot name a
// real asset tree, so no resolver is produced. Otherwise store the value
// verbatim — it becomes the base prefix of every Resolved_Path, preserved
// exactly (including any trailing separators) per.
Result<PathResolver> PathResolver::create(std::string data_root) {
    if (is_blank(data_root))
        return std::unexpected(Error{"PathResolver: Data_Root is empty"});
    return PathResolver(std::move(data_root));
}

// Precompute the seven texture search directories once, in fixed enum order
// (environment, buildings, ruins, props, airship, sfx, core), so
// texture_search_dirs() returns a stable, repeatably-ordered list for the
// same Data_Root. data_root_ is initialized first (it is the
// first-declared member) and is therefore already set when search_dirs_ is
// populated below.
PathResolver::PathResolver(std::string data_root)
    : data_root_(std::move(data_root)) {
    search_dirs_ = {
        data_root_ + std::string(texture_category_fragment(TextureCategory::Environment)),
        data_root_ + std::string(texture_category_fragment(TextureCategory::Buildings)),
        data_root_ + std::string(texture_category_fragment(TextureCategory::Ruins)),
        data_root_ + std::string(texture_category_fragment(TextureCategory::Props)),
        data_root_ + std::string(texture_category_fragment(TextureCategory::Airship)),
        data_root_ + std::string(texture_category_fragment(TextureCategory::Sfx)),
        data_root_ + std::string(texture_category_fragment(TextureCategory::Core)),
    };
}

// ── Textures  ──

// Verbatim concatenation of Data_Root + category fragment + filename, routed
// through ensure_dds so the result carries exactly one trailing ".dds". No
// trimming, deduplication, separator collapsing, or case change is performed:
// the category fragment already begins and ends with "/", so the output is
// byte-for-byte identical to the legacy `data_dir + fragment + filename` form
// (with the same single-".dds" guarantee).
std::string PathResolver::texture_path(TextureCategory category,
                                       std::string_view filename) const {
    return ensure_dds(data_root_ +
                      std::string(texture_category_fragment(category)) +
                      std::string(filename));
}

// ── Displacement maps  ──

// Verbatim Data_Root + "/" + rel, then routed through the same ensure_dds rule
// as texture_path. Reproduces the legacy `data_dir + "/" + rel` (+ conditional
// ".dds") displacement-map resolution in the material database exactly.
std::string PathResolver::displacement_path(std::string_view rel) const {
    return ensure_dds(data_root_ + "/" + std::string(rel));
}

// ── Texture search-directory enumeration  ──

// Return the seven resolved search directories precomputed once in the ctor,
// in fixed enum order (environment, buildings, ruins, props, airship, sfx,
// core). The list is stable and repeatably ordered for the same Data_Root —
// every invocation returns a reference to the same precomputed array.
const std::array<std::string, 7>& PathResolver::texture_search_dirs() const noexcept {
    return search_dirs_;
}

// ── Texture suffix family  ──

// Hand callers a span over the single k_texture_suffixes definition, so the
// suffix family is enumerated in exactly the fixed order _d _n _s _h _t _e _dd
// with none added or omitted.
std::span<const std::string_view> PathResolver::texture_suffixes() noexcept {
    return k_texture_suffixes;
}

// ── Auto-discovery candidate  ──

// Verbatim concatenation of search_dir + base + suffix + ".dds". `search_dir`
// is one of the resolved strings from texture_search_dirs() and already ends
// in "/", so no separator is inserted — reproducing the legacy
// `dir + name + suffix + ".dds"` form exactly. The function is total and
// deterministic: identical inputs always yield an identical Resolved_Path.
std::string PathResolver::texture_candidate(std::string_view search_dir,
                                            std::string_view base,
                                            std::string_view suffix) const {
    return std::string(search_dir) + std::string(base) + std::string(suffix) + ".dds";
}

// ── Meshes  ──

// Verbatim concatenation of Data_Root + mesh subdirectory fragment + mesh_name.
// The mesh subdirectory fragment begins and ends with "/", so the output is
// byte-for-byte identical to the legacy `data_dir + "/heaven/meshes/environment/"
// + name` form. The mesh name is used verbatim — no extension is appended or
// stripped — and the function performs no filesystem access, so it is total and
// never fails.
std::string PathResolver::mesh_path(MeshCategory subdir,
                                    std::string_view mesh_name) const {
    return data_root_ +
           std::string(mesh_category_fragment(subdir)) +
           std::string(mesh_name);
}

// ── Scene files: world  ──

// Verbatim Data_Root + "/heaven/heaven.world", reproducing the legacy
// `data_dir + "/heaven/heaven.world"` form exactly.
std::string PathResolver::world_path() const {
    return data_root_ + std::string(k_world_fragment);
}

// ── Music  ──

// Delegate to relative_path with the shared k_music_rel_path constant
// (defined in engine/types.h), so the Music_Asset_Path is the verbatim
// Data_Root + "/" + "heaven/heaven.oga" join. Routing through relative_path
// keeps the music track on the exact same Data_Root rule as every other
// relative asset and guarantees the renderer and the packer resolve it
// identically.
std::string PathResolver::music_path() const {
    return relative_path(k_music_rel_path);
}

std::string_view PathResolver::sky_asset_filename(SkyAsset asset) noexcept {
    switch (asset) {
        case SkyAsset::MoonDisk:   return "moon_disk.png";
        case SkyAsset::MoonNormal: return "moon_normal.png";
        case SkyAsset::MilkyWay:   return "milkyway.png";
    }
    return {};
}

std::string PathResolver::sky_asset_path(SkyAsset asset) const {
    return data_root_ + std::string(k_sky_asset_fragment) +
           std::string(sky_asset_filename(asset));
}

// ── Scene files: node  ──

// Reject an empty or whitespace-only filename with an Error, emitting no
// partial path. Otherwise return the verbatim concatenation
// Data_Root + "/heaven/nodes/" + filename, reproducing the legacy
// `data_dir + "/heaven/nodes/" + filename` form exactly.
Result<std::string> PathResolver::node_path(std::string_view filename) const {
    if (is_blank(filename))
        return std::unexpected(Error{"PathResolver: node filename is missing"});
    return data_root_ + std::string(k_nodes_fragment) + std::string(filename);
}

// ── Cache / track directories  ──

// Append the literal suffix to Data_Root verbatim — no separator
// normalization, case change, or trailing-separator handling — so the result
// is byte-for-byte identical to the legacy `data_dir + "/cache/tessellation"`
// and `data_dir + "/heaven/tracks"` forms. Both are total functions that
// return an identical string on every invocation for the same Data_Root.
std::string PathResolver::cache_dir() const {
    return data_root_ + std::string(k_cache_suffix);
}

std::string PathResolver::track_dir() const {
    return data_root_ + std::string(k_track_suffix);
}

// ── Data-root-relative joins  ──

// Verbatim Data_Root + "/" + rel, reproducing the legacy `data_dir + "/" + rel`
// form exactly for asset references already stored as paths relative to
// Data_Root. No extension logic is applied.
std::string PathResolver::relative_path(std::string_view rel) const {
    return data_root_ + "/" + std::string(rel);
}

} // namespace heaven
