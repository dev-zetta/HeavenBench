/* HeavenBench used-asset discovery, validation, and ZIP writer. */
#include "asset_packer/asset_packer.h"

#include "engine/asset_usage_recorder.h"
#include "engine/camera_path.h"
#include "engine/feature_assets.h"
#include "engine/ground_relief_generator.h"
#include "engine/material_rhi.h"
#include "engine/null_backend.h"
#include "engine/runtime_paths.h"
#include "engine/scene_rhi.h"
#include "engine/sha256.h"
#include "engine/zip_archive.h"
#include "dds_loader_cpu.h"
#include "benchmark/scene_markers.h"
#include "xml0_parser.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace heaven {

namespace {
constexpr std::array<std::string_view, 2> k_sky_metadata_files = {
    "README.md",
    "LICENSE.md",
};
constexpr std::string_view k_ground_relief_attribution =
    "attribution/DeepBump.md";
constexpr std::string_view k_bcdec_attribution =
    "attribution/bcdec.md";

bool is_readable_regular_file(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    std::ifstream probe(path, std::ios::binary);
    return probe.is_open();
}
}

// Worklist traversal over the abstract GraphView. The visited-set (keyed by
// AssetId) provides both dedup and cycle termination: a resolvable node's
// references are expanded at most once, so repeated edges to an already-seen
// node add no duplicate membership and cyclic references terminate.

// The worklist carries (node, referrer) pairs so an unresolvable target can be
// recorded against the node that pointed at it. Roots start the worklist with
// an empty referrer; an unresolvable root therefore records an empty referrer.

// A node whose references() returns std::nullopt is unresolvable: it is excluded
// from `reachable`, recorded once in `unresolved` as {missing, referrer}, and
// traversal continues past it. A node whose references() yields a value joins
// `reachable` and has each of its out-edges enqueued.
TraversalResult
traverse_reference_graph(const std::vector<AssetId>& roots, const GraphView& graph) {
    TraversalResult result;

    // Resolvable nodes whose out-edges have already been expanded. Membership
    // here both dedups `reachable` and guarantees cycle termination.
    std::set<AssetId> visited;

    // Dedup the unresolved list by the (missing, referrer) edge so duplicate
    // references to the same missing target from the same referrer record once,
    // while distinct referrers of the same missing target are each preserved.
    std::set<std::pair<AssetId, AssetId>> unresolved_seen;

    // Worklist of (node, referrer) pairs used as a stack. Roots have no referrer.
    std::vector<std::pair<AssetId, AssetId>> worklist;
    worklist.reserve(roots.size());
    for (const AssetId& root : roots) {
        worklist.emplace_back(root, std::string{});
    }

    while (!worklist.empty()) {
        std::pair<AssetId, AssetId> item = std::move(worklist.back());
        worklist.pop_back();
        const AssetId& node = item.first;
        const AssetId& referrer = item.second;

        // Already-expanded resolvable node: nothing more to do (dedup + cycles).
        if (visited.contains(node)) {
            continue;
        }

        std::optional<std::vector<AssetId>> edges = graph.references(node);
        if (!edges.has_value()) {
            // Unresolvable target: exclude from reachable, record once against
            // the referrer that pointed at it, and continue past it.
            std::pair<AssetId, AssetId> key{node, referrer};
            if (unresolved_seen.insert(key).second) {
                result.unresolved.push_back(Unresolved{node, referrer});
            }
            continue;
        }

        // Resolvable node joins the reachable set and is expanded exactly once.
        visited.insert(node);
        result.reachable.insert(node);

        for (const AssetId& child : *edges) {
            // Skip children already expanded; unresolved children are never in
            // `visited`, so each distinct referring edge to a missing target is
            // still enqueued and recorded.
            if (!visited.contains(child)) {
                worklist.emplace_back(child, node);
            }
        }
    }

    return result;
}

// Drive the renderer's own resolution path with a NullBackend + recorder so the
// discovered Used_Asset_Set is byte-for-byte what a render run would load
// No traversal, parsing, inheritance, or auto-discovery logic
// is duplicated here — each step delegates to the existing engine code, tapping
// it through the AssetUsageRecorder threaded in as a trailing pointer.

// The recorder is constructed over the canonical Source_Tree root — the
// canonical parent of Data_Root — matching how AssetUsageRecorder and
// material_rhi.cpp normalize on-disk paths to stable Relative_Asset_Paths
// ("core/x", "heaven/x").
Result<DiscoveryResult>
discover_used_assets(const AppConfig& config, const PathResolver& paths,
                     AssetFileSystemRef assets) {
    namespace fs = std::filesystem;

    // Canonical Source_Tree root = canonical parent of Data_Root. weakly_canonical
    // does not require the path to exist; fall back to a lexically-normalized form
    // so the recorder always has a usable root.
    std::error_code ec;
    fs::path src_root = fs::path(paths.data_root()).parent_path();
    fs::path src_canon = fs::weakly_canonical(src_root, ec);
    if (ec) src_canon = src_root.lexically_normal();

    AssetUsageRecorder rec(src_canon.generic_string());
    NullBackend null;

    // 1. Parse all materials, recording each material name → its defining.mat so
    //    a later resolve() can report the provenance of only the USED subset.
    //    build() marks no file as used.
    RhiMaterialDatabase materials;
    materials.build(paths, &rec, assets);

    // 2. Run the real world/bridges/node walk and the real mesh loads through the
    //    materials-free overload, recording scene files + mesh files. The
    //    materials-free overload skips CPU tessellation and collision (render-time
    //    concerns that add no files to the used set — every height/tessellation
    //    texture is also a material texture slot recorded in step 3).
    Result<RhiScene> scene = load_scene_rhi(config, null, &rec, assets);
    if (!scene) return std::unexpected(scene.error());

    // 3. For each surface of each loaded mesh, resolve its material — recording
    //    the resolved.mat, its parent chain, and every texture slot (including
    //    convention-based auto-discovered slots), exactly as a render frame would.
    for (const Mesh& mesh : scene->cpu_meshes) {
        for (const Surface& surface : mesh.surfaces) {
            [[maybe_unused]] const GpuMaterial resolved =
                materials.resolve(mesh.path, surface.name, paths, null, &rec);
        }
    }

    // 4. Record extracted assets loaded directly by enabled render features.
    //    These paths do not appear in scene XML or material inheritance, so
    //    the shared feature inventory is the only authoritative discovery
    //    source for them.
    for (const FeatureAsset asset : enabled_feature_assets(config)) {
        const FeatureAssetInfo info = feature_asset_info(asset);
        const std::string path = feature_asset_path(paths, asset);
        if (is_readable_regular_file(path)) {
            if (info.texture_role.empty()) {
                rec.record_used(path);
            } else {
                rec.record_texture(path, info.texture_role, info.wrap);
            }
        } else {
            rec.record_unresolved(
                std::string(info.relative_path), std::string(info.referrer));
        }
    }

    // 5. Record the benchmark.track files (camera path + scene markers).
    const std::string track_dir = paths.track_dir();
    [[maybe_unused]] const CameraPath camera_path =
        load_camera_path(track_dir, &rec, assets);
    [[maybe_unused]] const std::optional<SceneMarkers> scene_markers =
        load_scene_markers(track_dir, &rec, assets);

    // 6. Record the background Music_Track exactly as a render run would load it
    //    The packer's effective configuration is the same
    //    AppConfig main() resolves (CLI > file > default), so --no-music flows in
    //    as config.music.enabled == false.

    //    - enabled + the track exists and is a readable regular file → record it
    //      as used. record_used normalizes the resolved Music_Asset_Path
    //      (Data_Root + "/heaven/heaven.oga") to the stable Relative_Asset_Path
    //      k_music_rel_path and dedups via its std::set, so this adds exactly one
    //      entry no matter how often it is recorded. The existing
    //      write_used_assets/pack_target_path then mirrors it into the
    //      Pack_Target byte-identically at the same relative path.
    //    - enabled but the track cannot be located or read → record it as an
    //      Unresolved_Reference against the synthetic "<music config>" referrer
    //      and continue; the orchestrator already renders the entry and exits
    //      EXIT_UNRESOLVED.
    //    - disabled → record nothing referencing the music path, so no entry of
    //      the Used_Asset_Set references it.
    if (config.music.enabled) {
        const std::string music_abs = paths.music_path();

        if (is_readable_regular_file(music_abs)) {
            rec.record_used(music_abs);
        } else {
            rec.record_unresolved(std::string(k_music_rel_path), "<music config>");
        }
    }

    return DiscoveryResult{
        rec.used(), rec.unresolved(), rec.texture_inventory()};
}

// True when `name` ends in `ext` compared case-insensitively. Pure: no I/O, no
// allocation. A name shorter than the extension can never end in it; otherwise
// the trailing `ext.size()` characters of `name` are compared to `ext` byte by
// byte under std::tolower (cast through unsigned char to avoid UB on negative
// chars). This lets the Source_Tree scan treat the music track's `.oga`
// extension case-insensitively (".oga"/".OGA"/".Oga" all match) while the
// underlying scan_dir_recursive only does exact, case-sensitive suffix matches

bool has_extension_ci(std::string_view name, std::string_view ext) {
    if (name.size() < ext.size()) {
        return false;
    }
    const std::string_view tail = name.substr(name.size() - ext.size());
    for (std::size_t i = 0; i < ext.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(tail[i]);
        const unsigned char b = static_cast<unsigned char>(ext[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

// Enumerate every Asset_File under the Source_Tree as a Relative_Asset_Path,
// forming the denominator for the Pack_Report consistency identity
// The Source_Tree has two on-disk roots — Data_Root and its
// sibling core directory (Data_Root/../core) — exactly the pair the renderer's
// material build and node scan already enumerate. Each root is walked once per
// Asset_File extension via the existing scan_dir_recursive helper, which takes a
// single extension suffix, so the extension set is iterated explicitly.

// Each absolute hit is normalized the same way AssetUsageRecorder::record_used
// normalizes a resolved file: weakly_canonical collapses the "../core" spelling
// (and any symlinks), then the path is made relative to the canonical
// Source_Tree root. This guarantees the scanned set and the discovered
// Used_Asset_Set share one Relative_Asset_Path vocabulary ("heaven/...",
// "core/...") so their intersection in compute_pack_report is meaningful.
std::set<std::string>
scan_source_tree(const PathResolver& paths) {
    namespace fs = std::filesystem;

    // Source_Tree root = canonical parent of Data_Root, matching discovery and
    // the recorder. weakly_canonical does not require the path to exist; fall
    // back to a lexically-normalized form so a usable root is always produced.
    std::error_code ec;
    fs::path data_root_canon = fs::weakly_canonical(fs::path(paths.data_root()), ec);
    if (ec) data_root_canon = fs::path(paths.data_root()).lexically_normal();
    const fs::path source_root = data_root_canon.parent_path();

    // The two on-disk roots of the Source_Tree.
    const std::array<std::string, 2> roots{
        paths.data_root(),
        paths.data_root() + "/../core",
    };

    std::set<std::string> result;

    // Normalize an absolute hit to its Relative_Asset_Path against the canonical
    // Source_Tree root and insert it. The result std::set dedups by key, so a
    // file reached by more than one pass is counted exactly once.
    const auto insert_hit = [&](const std::string& hit) {
        std::error_code hec;
        fs::path canonical = fs::weakly_canonical(fs::path(hit), hec);
        if (hec) canonical = fs::path(hit).lexically_normal();

        const fs::path rel = canonical.lexically_relative(source_root);
        std::string key = rel.empty() ? canonical.generic_string()
                                      : rel.generic_string();
        result.insert(std::move(key));
    };

    // Asset_File extensions matched by exact, case-sensitive suffix — the file
    // kinds the renderer can load. ".mesh" and ".smesh" are scanned separately;
    // scan_dir_recursive's suffix match is exact, so a ".smesh" file is not
    // matched by the ".mesh" pass.
    static constexpr std::array<const char*, 7> kExtensions{
        ".world", ".node", ".mesh", ".smesh", ".mat", ".dds", ".track",
    };

    for (const std::string& root : roots) {
        for (const char* ext : kExtensions) {
            std::vector<std::string> hits;
            scan_dir_recursive(root, ext, hits);
            for (const std::string& hit : hits) {
                insert_hit(hit);
            }
        }

        // The music track's `.oga` extension is matched case-insensitively
        // unlike the exact suffix match above. Because
        // scan_dir_recursive only does an exact, case-sensitive match it would
        // miss ".OGA"/".Oga" spellings, so this pass casts the widest net — an
        // empty suffix matches every regular file under the root — and keeps
        // only those whose filename ends in ".oga" compared case-insensitively
        // via has_extension_ci. The result std::set dedups by
        // Relative_Asset_Path, so a matching file is counted exactly once
        // regardless of its case spelling.
        std::vector<std::string> all_files;
        scan_dir_recursive(root, "", all_files);
        for (const std::string& hit : all_files) {
            const std::string filename = fs::path(hit).filename().string();
            if (has_extension_ci(filename, ".oga")) {
                insert_hit(hit);
            }
        }
    }
    return result;
}

// Pack-report arithmetic over Relative_Asset_Path sets. `scanned` is every
// Asset_File found in the Source_Tree (the denominator); `used` is the
// discovered Used_Asset_Set. The written count is the size of their
// intersection — the used files that actually live under the Source_Tree — and
// omitted is the remainder of the scanned set. Computing omitted as a
// subtraction (rather than counting `scanned \ used` directly) guarantees the
// identity written + omitted == scanned holds by construction
// for any inputs, even were `used` to contain a path absent from `scanned`.
PackReport
compute_pack_report(const std::set<std::string>& used,
                    const std::set<std::string>& scanned,
                    uintmax_t written_bytes) {
    PackReport report;

    report.scanned = static_cast<int>(scanned.size());

    // written = |used ∩ scanned|. Both inputs are sorted std::sets, so a single
    // linear set-intersection counts the shared members without materializing
    // the intersection. The omitted paths are then exactly scanned \ used.
    std::vector<std::string> intersection;
    std::set_intersection(used.begin(), used.end(),
                          scanned.begin(), scanned.end(),
                          std::back_inserter(intersection));
    report.written = static_cast<int>(intersection.size());

    report.omitted = report.scanned - report.written;
    report.written_bytes = written_bytes;

    return report;
}

// Render the success Pack_Report exactly as run_asset_packer prints it to
// stdout. Kept as a pure std::string builder (no I/O) so the rendered text is
// host-testable without driving the orchestrator. The written-bytes value is a
// uintmax_t and is always non-negative; the counts come from
// compute_pack_report. The trailing newline matches the previous line-by-line
// printf output so stdout is byte-for-byte unchanged.
std::string
render_pack_report(const PackReport& report) {
    std::string out;
    out += "Pack report:\n";
    out += "  written: " + std::to_string(report.written) + " files\n";
    out += "  omitted: " + std::to_string(report.omitted) + " files\n";
    out += "  scanned: " + std::to_string(report.scanned) + " files\n";
    out += "  written bytes: " +
           std::to_string(static_cast<uintmax_t>(report.written_bytes)) + "\n";
    out += "  archive bytes: " +
           std::to_string(static_cast<uintmax_t>(report.archive_bytes)) + "\n";
    const double ratio = report.written_bytes == 0
        ? 0.0
        : static_cast<double>(report.archive_bytes) /
          static_cast<double>(report.written_bytes);
    char ratio_text[64];
    std::snprintf(ratio_text, sizeof(ratio_text),
                  "  archive ratio: %.3f\n", ratio);
    out += ratio_text;
    return out;
}

// Render the unresolved-reference list run_asset_packer prints to stderr when
// discovery recorded broken edges. A count header is followed by one line per
// entry naming the missing Relative_Asset_Path and the referring Asset_File
// An empty list renders as an empty string so the caller
// prints nothing; a non-empty list ends in a newline.
std::string
render_unresolved_list(const std::vector<Unresolved>& unresolved) {
    if (unresolved.empty()) {
        return std::string{};
    }

    std::string out;
    out += "Asset packer: " + std::to_string(unresolved.size()) +
           " unresolved reference(s):\n";
    for (const Unresolved& u : unresolved) {
        out += "  " + u.missing_rel_path + " (referenced by " +
               u.referring_rel_path + ")\n";
    }
    return out;
}

// Render the pre-completion write-failure indication run_asset_packer prints to
// stderr in place of the success report when packing fails before completion.
// The message names the Pack_Target and reports the written count as zero
// The trailing newline matches the previous fprintf output.
std::string
render_write_failure(const std::string& target) {
    return "Asset packer: packing failed before completion; 0 files written to '" +
           target + "'\n";
}

// Layout-preserving join of a Relative_Asset_Path onto the Pack_Target root.
// This is a purely lexical operation — no filesystem access — so that a file's
// position under the Pack_Target mirrors its position under the Source_Tree and
// the result is host-testable without a real directory tree. operator/ on
// std::filesystem::path performs exactly the lexical append the implementation calls
// for, normalizing the directory separator for the host platform.
std::filesystem::path
pack_target_path(const std::filesystem::path& root, const std::string& rel) {
    return root / rel;
}

// Inverse of pack_target_path against the Source_Tree root: expresses an
// absolute Asset_File path as a Relative_Asset_Path. Using lexically_relative
// keeps this pure (no filesystem probing) and the exact lexical inverse of the
// operator/ join above, so source_relative(root, pack_target_path(root, rel))
// recovers rel. The generic_string form normalizes to '/' separators, giving a
// stable Relative_Asset_Path regardless of host platform.
std::string
source_relative(const std::filesystem::path& root, const std::filesystem::path& abs) {
    return abs.lexically_relative(root).generic_string();
}

// Classify the Pack_Target pre-state and prepare it for writing, implementing
// the decision table. Every std::filesystem probe uses the
// std::error_code overload so no exception crosses out of the packer; a
// filesystem error is folded into the appropriate fatal status. The five
// outcomes:

//   - absent path, or an existing empty directory  → create dirs, ReadyCreated
//   - existing non-empty directory, overwrite off   → ConflictNonEmpty (fatal)
//   - existing non-empty directory, overwrite on    → ReadyOverwrite
//   - existing path that is not a directory          → NotADirectory (fatal)
//   - create_directories failure                     → CreateFailed (fatal)

// The message is a human-readable diagnostic the orchestrator prints; it names
// the conflicting path so the developer can act on it.
TargetDecision
prepare_pack_target(const std::filesystem::path& target, bool overwrite) {
    namespace fs = std::filesystem;

    const std::string target_str = target.generic_string();

    // Probe existence. A status error here (e.g. a permission failure on a path
    // component) is treated as inability to use the target — surface it as a
    // CreateFailed so the orchestrator stops before writing.
    std::error_code ec;
    const bool exists = fs::exists(target, ec);
    if (ec) {
        return TargetDecision{TargetStatus::CreateFailed,
                              "cannot stat Pack_Target '" + target_str +
                                  "': " + ec.message()};
    }

    if (exists) {
        // An existing path that is not a directory is a hard conflict: writing
        // assets underneath it is impossible.
        const bool is_dir = fs::is_directory(target, ec);
        if (ec) {
            return TargetDecision{TargetStatus::CreateFailed,
                                  "cannot stat Pack_Target '" + target_str +
                                      "': " + ec.message()};
        }
        if (!is_dir) {
            return TargetDecision{TargetStatus::NotADirectory,
                                  "Pack_Target '" + target_str +
                                      "' exists but is not a directory"};
        }

        // Existing directory: empty vs non-empty decides the outcome.
        const bool empty = fs::is_empty(target, ec);
        if (ec) {
            return TargetDecision{TargetStatus::CreateFailed,
                                  "cannot inspect Pack_Target '" + target_str +
                                      "': " + ec.message()};
        }
        if (empty) {
            // An existing empty directory is usable as-is.
            return TargetDecision{TargetStatus::ReadyCreated,
                                  "Pack_Target '" + target_str +
                                      "' is an empty directory; using as-is"};
        }

        // Non-empty directory: only proceed when overwrite was requested,
        // otherwise refuse to mix into stale contents.
        if (overwrite) {
            return TargetDecision{TargetStatus::ReadyOverwrite,
                                  "Pack_Target '" + target_str +
                                      "' is non-empty; overwriting per --pack-overwrite"};
        }
        return TargetDecision{TargetStatus::ConflictNonEmpty,
                              "Pack_Target '" + target_str +
                                  "' already exists and is not empty; pass "
                                  "--pack-overwrite to write into it"};
    }

    // Absent path: create the directory and any missing parents. A
    // create_directories failure is fatal.
    fs::create_directories(target, ec);
    if (ec) {
        return TargetDecision{TargetStatus::CreateFailed,
                              "failed to create Pack_Target '" + target_str +
                                  "': " + ec.message()};
    }
    return TargetDecision{TargetStatus::ReadyCreated,
                          "created Pack_Target '" + target_str + "'"};
}

// Write (Copy) or relocate (Move) each used Asset_File into the Pack_Target at
// its mirrored Relative_Asset_Path. Every std::filesystem call uses the
// std::error_code overload so no exception crosses out of the writer; each error
// is folded into the WriteOutcome (a per-file diagnostic, or the fatal flag).

// The source on-disk path is reconstructed exactly as discovery/scan produced
// the Relative_Asset_Path: the rel is joined onto the canonical Source_Tree root
// (the canonical parent of Data_Root), so src = source_root / rel mirrors the
// rel = lexically_relative(source_root) the recorder and the scan computed. The
// destination is the layout-preserving pack_target_path(target, rel).

// The reconciliation between  ("writing a file fails → stop") and
// ("transferring a file fails → continue") is by scope: a
// directory-creation failure under the Pack_Target is the fatal  case
// (it would prevent writing this and likely every subsequent file), so the
// operation stops with `fatal` set; a per-file content-transfer failure
// (copy/move of one asset's bytes) is recorded and skipped, leaving that source
// file present and unchanged, and the remaining files are still processed.

// `used` is a std::set, so iteration is already in sorted Relative_Asset_Path
// order, giving a deterministic write order.
WriteOutcome
write_used_assets(const std::set<std::string>& used,
                  const PathResolver& paths,
                  const std::filesystem::path& target,
                  TransferMode mode) {
    namespace fs = std::filesystem;

    WriteOutcome outcome;

    // Canonical Source_Tree root = canonical parent of Data_Root, matching how
    // discovery and scan_source_tree derive the Relative_Asset_Paths. Joining a
    // rel back onto this root recovers the on-disk source file. weakly_canonical
    // does not require the path to exist; fall back to a lexically-normalized
    // form so a usable root is always produced.
    std::error_code ec;
    fs::path data_root_canon = fs::weakly_canonical(fs::path(paths.data_root()), ec);
    if (ec) data_root_canon = fs::path(paths.data_root()).lexically_normal();
    const fs::path source_root = data_root_canon.parent_path();

    for (const std::string& rel : used) {
        const fs::path dst = pack_target_path(target, rel);
        const fs::path src = source_root / rel;

        // Create the destination's parent directories. A failure here is the
        // fatal  case: stop the operation with `fatal` set.
        const fs::path parent = dst.parent_path();
        if (!parent.empty()) {
            std::error_code dir_ec;
            fs::create_directories(parent, dir_ec);
            if (dir_ec) {
                outcome.failures.push_back(
                    "failed to create directory for '" + rel +
                    "': " + dir_ec.message());
                outcome.fatal = true;
                break;
            }
        }

        bool placed = false;
        if (mode == TransferMode::Copy) {
            // Copy the bytes, replacing any file already at the destination so a
            // repeated overwrite run is byte-identical. On error,
            // record a per-file diagnostic, leave the source unchanged, and
            // continue with the remaining files.
            std::error_code copy_ec;
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, copy_ec);
            if (copy_ec) {
                outcome.failures.push_back(
                    "failed to copy '" + rel + "': " + copy_ec.message());
                continue;
            }
            placed = true;
        } else {
            // Move: rename relocates within a filesystem in one step. Across
            // filesystems rename reports an error (cross-device), so fall back to
            // copy-then-remove. On any failure, record a diagnostic, leave the
            // source present and unchanged, and continue.
            std::error_code rename_ec;
            fs::rename(src, dst, rename_ec);
            if (rename_ec) {
                std::error_code copy_ec;
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing, copy_ec);
                if (copy_ec) {
                    outcome.failures.push_back(
                        "failed to move '" + rel + "': " + rename_ec.message());
                    continue;
                }
                // The bytes are now in the target; remove the source to complete
                // the relocation. A best-effort remove failure does not undo the
                // successful placement.
                std::error_code rm_ec;
                fs::remove(src, rm_ec);
            }
            placed = true;
        }

        if (placed) {
            ++outcome.written;
            std::error_code size_ec;
            const uintmax_t size = fs::file_size(dst, size_ec);
            if (!size_ec) outcome.bytes += size;
        }
    }

    return outcome;
}

namespace {

namespace fs = std::filesystem;

[[nodiscard]] ZipCompression
asset_zip_compression(std::string_view path) {
    static constexpr std::array<std::string_view, 6> kStoredExtensions{
        ".dds", ".png", ".jpg", ".jpeg", ".oga", ".ttf",
    };
    for (std::string_view extension : kStoredExtensions) {
        if (asset_path_has_suffix(path, extension)) {
            return ZipCompression::Store;
        }
    }
    return ZipCompression::Deflate;
}

struct ArchivePackOutcome {
    int files = 0;
    uintmax_t uncompressed_bytes = 0;
    uintmax_t archive_bytes = 0;
};

[[nodiscard]] constexpr std::string_view
fallback_texture_role(std::string_view path) noexcept {
    const std::size_t dot = path.rfind('.');
    std::string_view stem = path;
    if (dot != std::string_view::npos) stem.remove_suffix(stem.size() - dot);
    if (stem.ends_with("_n") || stem.ends_with("_dn")) return "normal";
    if (stem.ends_with("_h")) return "height";
    if (stem.ends_with("_t")) return "tessellation";
    if (stem.ends_with("_s") || stem.ends_with("_ds")) return "specular";
    if (stem.ends_with("_e")) return "emissive";
    if (stem.ends_with("_d") || stem.ends_with("_dd")) return "diffuse";
    return "data";
}

[[nodiscard]] constexpr std::string_view
dds_format_id(DDSFormatCPU format) noexcept {
    switch (format) {
        case DDSFormatCPU::BC1: return "BC1";
        case DDSFormatCPU::BC3: return "BC3";
        case DDSFormatCPU::BC4: return "BC4";
        case DDSFormatCPU::BC5: return "BC5";
        case DDSFormatCPU::BC7: return "BC7";
        case DDSFormatCPU::RGBA8: return "RGBA8";
        case DDSFormatCPU::RGB8: return "RGB8";
        case DDSFormatCPU::Unknown: break;
    }
    return "unknown";
}

[[nodiscard]] uint32_t read_be_u32(const std::byte* bytes) noexcept {
    return (uint32_t(std::to_integer<uint8_t>(bytes[0])) << 24u) |
           (uint32_t(std::to_integer<uint8_t>(bytes[1])) << 16u) |
           (uint32_t(std::to_integer<uint8_t>(bytes[2])) << 8u) |
           uint32_t(std::to_integer<uint8_t>(bytes[3]));
}

[[nodiscard]] Result<nlohmann::json> build_texture_inventory_json(
        const std::set<std::string>& used,
        const std::map<std::string, TextureInventoryHint>& hints,
        const fs::path& source_root,
        const fs::path& bundled_root,
        std::string_view data_prefix) {
    nlohmann::json textures = nlohmann::json::object();
    const auto add_dds = [&](const std::string& logical,
                             const fs::path& source,
                             std::string_view role,
                             std::string_view wrap) -> Result<void> {
        const DDSTextureCPU dds = load_dds_cpu(source.string());
        auto fingerprint = sha256_file(source);
        if (!fingerprint) return std::unexpected(fingerprint.error());
        if (!dds) {
            std::error_code ec;
            const uint64_t bytes = fs::file_size(source, ec);
            if (ec) {
                return std::unexpected(Error{
                    "failed to inventory DDS texture: " + source.string()});
            }
            // Preserve the packer's historical byte-faithful behavior for a
            // malformed source. A real hi-res bake still fails closed because
            // the generator requires a parseable DDS before processing it.
            textures[logical] = {
                {"role", role},
                {"wrap", wrap},
                {"source_format", "unknown"},
                {"width", 0},
                {"height", 0},
                {"mip_count", 0},
                {"has_alpha", false},
                {"gpu_bytes", bytes},
                {"source_sha256", *fingerprint},
            };
            return {};
        }
        uint64_t gpu_bytes = 0;
        for (const DDSMipLevelCPU& mip : dds.mip_data) gpu_bytes += mip.size();
        textures[logical] = {
            {"role", role},
            {"wrap", wrap},
            {"source_format", dds_format_id(dds.format)},
            {"width", dds.width},
            {"height", dds.height},
            {"mip_count", dds.mip_count},
            {"has_alpha", dds.format == DDSFormatCPU::BC1 ||
                          dds.format == DDSFormatCPU::BC3 ||
                          dds.format == DDSFormatCPU::RGBA8},
            {"gpu_bytes", gpu_bytes},
            {"source_sha256", *fingerprint},
        };
        return {};
    };

    for (const std::string& logical : used) {
        if (!asset_path_has_suffix(logical, ".dds")) continue;
        const auto hint = hints.find(logical);
        const std::string_view role = hint == hints.end()
            ? fallback_texture_role(logical) : std::string_view{hint->second.role};
        const std::string_view wrap = hint == hints.end()
            ? std::string_view{"repeat"} : std::string_view{hint->second.wrap};
        auto added = add_dds(logical, source_root / logical, role, wrap);
        if (!added) return std::unexpected(added.error());
    }

    for (std::string_view filename : {
            std::string_view{"moon_disk.png"},
            std::string_view{"moon_normal.png"},
            std::string_view{"milkyway.png"}}) {
        const fs::path source = bundled_root / "sky" / filename;
        std::ifstream stream(source, std::ios::binary);
        std::array<std::byte, 32> header{};
        if (!stream.read(reinterpret_cast<char*>(header.data()), header.size()) ||
            std::memcmp(header.data(), "\x89PNG\r\n\x1a\n", 8) != 0) {
            return std::unexpected(Error{
                "failed to inventory PNG texture: " + source.string()});
        }
        const uint32_t width = read_be_u32(header.data() + 16);
        const uint32_t height = read_be_u32(header.data() + 20);
        const uint8_t color_type = std::to_integer<uint8_t>(header[25]);
        auto fingerprint = sha256_file(source);
        if (!fingerprint) return std::unexpected(fingerprint.error());
        uint64_t gpu_bytes = 0;
        for (uint32_t w = width, h = height;;
             w = std::max(1u, w / 2u), h = std::max(1u, h / 2u)) {
            gpu_bytes += uint64_t(w) * h * 4u;
            if (w == 1 && h == 1) break;
        }
        const std::string logical = std::string(data_prefix) + "/sky/" +
                                    std::string(filename);
        textures[logical] = {
            {"role", filename == "moon_normal.png" ? "normal" : "color"},
            {"wrap", filename == "milkyway.png" ? "repeat" : "clamp"},
            {"source_format", "PNG_RGBA8"},
            {"width", width},
            {"height", height},
            {"mip_count", std::bit_width(std::max(width, height))},
            {"has_alpha", color_type == 4 || color_type == 6},
            {"gpu_bytes", gpu_bytes},
            {"source_sha256", *fingerprint},
        };
    }
    return nlohmann::json{
        {"schema_version", 1},
        {"textures", std::move(textures)},
    };
}

struct ValidatedTextureVariant {
    std::string original_path;
    fs::path source_path;
    fs::path variant_source_path;
    std::string archive_path;
    std::string source_sha256;
    std::string output_sha256;
    std::string role;
    std::string wrap;
    std::string format;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t source_gpu_bytes = 0;
    uint64_t gpu_bytes_2k = 0;
    uint64_t gpu_bytes_4k = 0;
    bool alias = false;
};

struct ValidatedTextureArtifact {
    fs::path source_path;
    std::string archive_path;
};

struct ValidatedTextureSet {
    std::vector<ValidatedTextureVariant> variants;
    std::vector<ValidatedTextureArtifact> artifacts;
    fs::path manifest_path;
    std::string generator_id;
    std::string generator_version;
    std::string manifest_sha256;
    uint64_t original_gpu_bytes = 0;
    uint64_t high_2k_gpu_bytes = 0;
    uint64_t ultra_4k_gpu_bytes = 0;
};

[[nodiscard]] bool safe_archive_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' ||
        path.find('\\') != std::string_view::npos) {
        return false;
    }
    const fs::path candidate(path);
    if (candidate.is_absolute() || candidate.generic_string() != path) return false;
    return std::ranges::all_of(candidate, [](const fs::path& part) {
        return part != "." && part != ".." && !part.empty();
    });
}

[[nodiscard]] bool sha256_token(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(),
        [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

[[nodiscard]] Result<ValidatedTextureSet>
validate_texture_set(const fs::path& manifest_path,
                     const std::set<std::string>& used,
                     const fs::path& source_root,
                     std::string_view data_prefix) {
    std::ifstream input(manifest_path, std::ios::binary);
    if (!input) {
        return std::unexpected(Error{
            "failed to open texture set manifest: " + manifest_path.string()});
    }
    nlohmann::json document;
    try {
        input >> document;
    } catch (const std::exception& ex) {
        return std::unexpected(Error{
            "failed to parse texture set manifest: " + std::string(ex.what())});
    }
    if (document.value("schema_version", 0) != 1 ||
        document.value("format", "") != "heavenbench-texture-set" ||
        !document.value("complete", false) ||
        !document.contains("entries") || !document["entries"].is_array()) {
        return std::unexpected(Error{
            "texture set is incomplete or uses an unsupported schema"});
    }

    try {
    ValidatedTextureSet result;
    result.manifest_path = manifest_path;
    auto manifest_hash = sha256_file(manifest_path);
    if (!manifest_hash) return std::unexpected(manifest_hash.error());
    result.manifest_sha256 = *manifest_hash;
    result.generator_id = document.at("generator").at("id").get<std::string>();
    result.generator_version =
        document.at("generator").at("version").get<std::string>();

    std::set<std::string> required;
    for (const std::string& path : used) {
        if (asset_path_has_suffix(path, ".dds")) required.insert(path);
    }
    required.insert(std::string(data_prefix) + "/sky/moon_disk.png");
    required.insert(std::string(data_prefix) + "/sky/moon_normal.png");
    required.insert(std::string(data_prefix) + "/sky/milkyway.png");

    const fs::path manifest_dir = manifest_path.parent_path().empty()
        ? fs::path(".") : manifest_path.parent_path();
    const fs::path bundled_root = bundled_resource_directory(HEAVEN_BUNDLED_ASSET_DIR);
    std::set<std::string> originals;
    std::set<std::string> archive_paths;
    for (const auto& entry : document["entries"]) {
        ValidatedTextureVariant item;
        item.original_path = entry.at("original_path").get<std::string>();
        if (!safe_archive_path(item.original_path) ||
            !required.contains(item.original_path) ||
            !originals.insert(item.original_path).second) {
            return std::unexpected(Error{
                "texture set has an unsafe, extra, or duplicate original mapping: " +
                item.original_path});
        }
        const std::string sky_prefix = std::string(data_prefix) + "/sky/";
        if (item.original_path.starts_with(sky_prefix)) {
            item.source_path = bundled_root /
                item.original_path.substr(std::string(data_prefix).size() + 1);
        } else {
            item.source_path = source_root / item.original_path;
        }
        item.source_sha256 = entry.at("source_sha256").get<std::string>();
        item.role = entry.at("role").get<std::string>();
        item.wrap = entry.at("wrap").get<std::string>();
        static const std::set<std::string> roles{
            "diffuse", "emissive", "color", "normal", "specular",
            "height", "tessellation", "data"};
        if (!roles.contains(item.role) ||
            (item.wrap != "repeat" && !item.wrap.starts_with("clamp"))) {
            return std::unexpected(Error{
                "texture set semantic metadata is invalid: " +
                item.original_path});
        }
        const auto current_source_hash = sha256_file(item.source_path);
        if (!current_source_hash || !sha256_token(item.source_sha256) ||
            *current_source_hash != item.source_sha256) {
            return std::unexpected(Error{
                "texture set source fingerprint is stale: " + item.original_path});
        }

        const auto& variant = entry.at("variant");
        item.archive_path = variant.at("archive_path").get<std::string>();
        item.output_sha256 = variant.at("sha256").get<std::string>();
        item.format = variant.at("format").get<std::string>();
        item.width = variant.at("width").get<uint32_t>();
        item.height = variant.at("height").get<uint32_t>();
        item.source_gpu_bytes = entry.at("source").at("gpu_bytes").get<uint64_t>();
        item.gpu_bytes_2k = variant.at("gpu_bytes_2k").get<uint64_t>();
        item.gpu_bytes_4k = variant.at("gpu_bytes_4k").get<uint64_t>();
        const std::string status = entry.value("status", "");
        item.alias = status == "alias";
        if (status != "generated" && status != "alias") {
            return std::unexpected(Error{
                "texture set contains a failed or unknown entry: " +
                item.original_path});
        }
        if (!safe_archive_path(item.archive_path) ||
            !archive_paths.insert(item.archive_path).second ||
            item.width == 0 || item.height == 0 ||
            item.width > loadval::kMaxTexDim ||
            item.height > loadval::kMaxTexDim ||
            !sha256_token(item.output_sha256)) {
            return std::unexpected(Error{
                "texture set variant metadata is invalid: " + item.original_path});
        }
        const uint32_t source_width =
            entry.at("source").at("width").get<uint32_t>();
        const uint32_t source_height =
            entry.at("source").at("height").get<uint32_t>();
        const std::string source_format =
            entry.at("source").at("format").get<std::string>();
        if (source_width == 0 || source_height == 0 ||
            source_width > loadval::kMaxTexDim ||
            source_height > loadval::kMaxTexDim) {
            return std::unexpected(Error{
                "texture set source dimensions are invalid: " +
                item.original_path});
        }
        uint32_t actual_source_width = 0;
        uint32_t actual_source_height = 0;
        uint64_t actual_source_gpu_bytes = 0;
        std::string actual_source_format;
        if (asset_path_has_suffix(item.original_path, ".dds")) {
            const DDSTextureCPU source_dds =
                load_dds_cpu(item.source_path.string());
            if (!source_dds) {
                return std::unexpected(Error{
                    "texture set original DDS is invalid: " +
                    item.original_path});
            }
            actual_source_width = source_dds.width;
            actual_source_height = source_dds.height;
            actual_source_format = dds_format_id(source_dds.format);
            for (const DDSMipLevelCPU& mip : source_dds.mip_data) {
                actual_source_gpu_bytes += mip.size();
            }
        } else if (asset_path_has_suffix(item.original_path, ".png")) {
            std::ifstream source_stream(item.source_path, std::ios::binary);
            std::array<std::byte, 32> header{};
            if (!source_stream.read(
                    reinterpret_cast<char*>(header.data()), header.size()) ||
                std::memcmp(header.data(), "\x89PNG\r\n\x1a\n", 8) != 0) {
                return std::unexpected(Error{
                    "texture set original PNG is invalid: " +
                    item.original_path});
            }
            actual_source_width = read_be_u32(header.data() + 16);
            actual_source_height = read_be_u32(header.data() + 20);
            actual_source_format = "PNG_RGBA8";
            for (uint32_t w = actual_source_width,
                          h = actual_source_height;;
                 w = std::max(1u, w / 2u), h = std::max(1u, h / 2u)) {
                actual_source_gpu_bytes += uint64_t(w) * h * 4u;
                if (w == 1u && h == 1u) break;
            }
        }
        if (source_width != actual_source_width ||
            source_height != actual_source_height ||
            source_format != actual_source_format ||
            item.source_gpu_bytes != actual_source_gpu_bytes) {
            return std::unexpected(Error{
                "texture set source metadata is stale: " +
                item.original_path});
        }
        if (item.alias) {
            if (item.archive_path != item.original_path ||
                item.output_sha256 != item.source_sha256 ||
                !item.original_path.starts_with("core/") ||
                source_width != 16u || source_height != 16u ||
                item.width != source_width || item.height != source_height) {
                return std::unexpected(Error{
                    "texture set alias is not an exact 16x16 core default: " +
                    item.original_path});
            }
            item.variant_source_path = item.source_path;
        } else {
            const uint32_t source_edge = std::max(source_width, source_height);
            const uint32_t scale = source_edge <= 1024u ? 4u :
                                   source_edge <= 2048u ? 2u : 1u;
            if (item.width != source_width * scale ||
                item.height != source_height * scale) {
                return std::unexpected(Error{
                    "texture set output dimensions violate the 4K scaling "
                    "policy: " + item.original_path});
            }
            const std::string relative = variant.at("file").get<std::string>();
            if (!safe_archive_path(relative)) {
                return std::unexpected(Error{
                    "texture set variant file path is unsafe: " + relative});
            }
            item.variant_source_path = manifest_dir / relative;
            const auto output_hash = sha256_file(item.variant_source_path);
            if (!output_hash || *output_hash != item.output_sha256) {
                return std::unexpected(Error{
                    "texture set output fingerprint is stale or corrupt: " +
                    item.original_path});
            }
            const DDSTextureCPU output_dds =
                load_dds_cpu(item.variant_source_path.string());
            const DDSFormatCPU expected_format = item.role == "normal"
                ? DDSFormatCPU::BC5
                : (item.role == "height" || item.role == "tessellation")
                    ? DDSFormatCPU::BC4 : DDSFormatCPU::BC7;
            const std::string expected_format_id =
                expected_format == DDSFormatCPU::BC5 ? "BC5" :
                expected_format == DDSFormatCPU::BC4 ? "BC4" :
                (item.role == "diffuse" || item.role == "emissive" ||
                 item.role == "color") ? "BC7_SRGB" : "BC7";
            if (!output_dds || output_dds.format != expected_format ||
                item.format != expected_format_id ||
                output_dds.width != item.width ||
                output_dds.height != item.height ||
                !std::cmp_equal(output_dds.mip_count,
                    std::bit_width(std::max(item.width, item.height)))) {
                return std::unexpected(Error{
                    "texture set output DDS metadata is invalid: " +
                    item.original_path});
            }
            uint64_t actual_4k_bytes = 0;
            uint64_t actual_2k_bytes = 0;
            uint32_t edge = std::max(item.width, item.height);
            for (const DDSMipLevelCPU& mip : output_dds.mip_data) {
                actual_4k_bytes += mip.size();
                if (edge <= 2048u) actual_2k_bytes += mip.size();
                edge = std::max(1u, edge >> 1u);
            }
            if (item.gpu_bytes_2k != actual_2k_bytes ||
                item.gpu_bytes_4k != actual_4k_bytes) {
                return std::unexpected(Error{
                    "texture set mip byte estimates are invalid: " +
                    item.original_path});
            }
        }
        result.original_gpu_bytes += item.source_gpu_bytes;
        result.high_2k_gpu_bytes += item.gpu_bytes_2k;
        result.ultra_4k_gpu_bytes += item.gpu_bytes_4k;
        result.variants.push_back(std::move(item));
    }
    if (originals != required) {
        return std::unexpected(Error{
            "texture set does not map every required packaged texture"});
    }

    if (!document.contains("artifacts") || !document["artifacts"].is_array()) {
        return std::unexpected(Error{
            "texture set is missing reports, licenses, and provenance artifacts"});
    }
    std::set<std::string> required_artifacts{
        std::string(data_prefix) +
            "/texture_sets/metadata/reports/texture-quality-report.json",
        std::string(data_prefix) +
            "/texture_sets/metadata/reports/texture-quality-report.html",
        std::string(data_prefix) +
            "/texture_sets/metadata/reports/contact-sheet-source.png",
        std::string(data_prefix) +
            "/texture_sets/metadata/reports/contact-sheet-2k.png",
        std::string(data_prefix) +
            "/texture_sets/metadata/reports/contact-sheet-4k.png",
        std::string(data_prefix) +
            "/texture_sets/metadata/provenance/PROVENANCE.md",
        std::string(data_prefix) +
            "/texture_sets/metadata/provenance/licenses/HAT-LICENSE.txt",
        std::string(data_prefix) +
            "/texture_sets/metadata/provenance/licenses/BasicSR-LICENSE.txt",
        std::string(data_prefix) +
            "/texture_sets/metadata/provenance/licenses/Compressonator-LICENSE.txt",
    };
    for (const auto& artifact : document["artifacts"]) {
        const std::string file = artifact.at("file").get<std::string>();
        const std::string archive_path =
            artifact.at("archive_path").get<std::string>();
        const std::string expected_hash = artifact.at("sha256").get<std::string>();
        if (!safe_archive_path(file) || !safe_archive_path(archive_path) ||
            !archive_paths.insert(archive_path).second ||
            !sha256_token(expected_hash)) {
            return std::unexpected(Error{"texture set artifact metadata is invalid"});
        }
        const fs::path source = manifest_dir / file;
        const auto actual_hash = sha256_file(source);
        if (!actual_hash || *actual_hash != expected_hash) {
            return std::unexpected(Error{
                "texture set artifact fingerprint is stale or corrupt: " + file});
        }
        required_artifacts.erase(archive_path);
        result.artifacts.push_back({source, archive_path});
    }
    if (!required_artifacts.empty()) {
        return std::unexpected(Error{
            "texture set is missing required reports, contact sheets, licenses, "
            "or provenance"});
    }
    return result;
    } catch (const std::exception& ex) {
        return std::unexpected(Error{
            "texture set manifest structure is invalid: " +
            std::string(ex.what())});
    }
}

[[nodiscard]] Result<ArchivePackOutcome>
write_asset_archive(const std::set<std::string>& used,
                    const std::map<std::string, TextureInventoryHint>& inventory,
                    const PathResolver& paths,
                    const PackOptions& options,
                    const GeneratedAsset& generated,
                    const ValidatedTextureSet* texture_set) {
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::path data_root =
        fs::weakly_canonical(fs::path(paths.data_root()), ec);
    if (ec) data_root = fs::path(paths.data_root()).lexically_normal();
    const fs::path source_root = data_root.parent_path();
    const std::string data_prefix = data_root.filename().generic_string();
    if (data_prefix.empty()) {
        return std::unexpected(
            Error{"Data_Root has no archive prefix"});
    }

    auto writer_result = ZipArchiveWriter::create(
        options.target, options.overwrite);
    if (!writer_result) return std::unexpected(writer_result.error());
    ZipArchiveWriter writer = std::move(*writer_result);

    ArchivePackOutcome outcome;
    std::vector<fs::path> moved_sources;
    moved_sources.reserve(used.size());

    const bool extension_pack = texture_set != nullptr;
    if (!extension_pack) {
        for (const std::string& relative : used) {
            const fs::path source = source_root / fs::path(relative);
            auto added = writer.add_file(
                relative, source, asset_zip_compression(relative));
            if (!added) {
                return std::unexpected(Error{
                    "failed to archive '" + relative + "': " +
                    added.error().message});
            }
            const uintmax_t size = fs::file_size(source, ec);
            if (!ec) outcome.uncompressed_bytes += size;
            ec.clear();
            ++outcome.files;
            if (options.move) moved_sources.push_back(source);
        }
    }

    if (!extension_pack) {
        auto generated_added = writer.add_bytes(
            generated.logical_path,
            std::as_bytes(std::span(*generated.bytes)),
            ZipCompression::Store);
        if (!generated_added) {
            return std::unexpected(Error{
                "failed to archive generated ground height: " +
                generated_added.error().message});
        }
        outcome.uncompressed_bytes += generated.bytes->size();
        ++outcome.files;
    }

    const fs::path bundled_root = bundled_resource_directory(HEAVEN_BUNDLED_ASSET_DIR);
    const auto add_bundled = [&](std::string_view relative) -> Result<void> {
        const std::string entry = data_prefix + "/" + std::string(relative);
        const fs::path source = bundled_root / relative;
        auto added = writer.add_file(
            entry, source, asset_zip_compression(entry));
        if (!added) return std::unexpected(added.error());
        const uintmax_t size = fs::file_size(source, ec);
        if (!ec) outcome.uncompressed_bytes += size;
        ec.clear();
        ++outcome.files;
        return {};
    };
    if (!extension_pack) {
        for (SkyAsset asset : kSkyAssets) {
            const std::string relative =
                "sky/" + std::string(PathResolver::sky_asset_filename(asset));
            auto added = add_bundled(relative);
            if (!added) return std::unexpected(added.error());
        }
        for (std::string_view filename : k_sky_metadata_files) {
            const std::string relative = "sky/" + std::string(filename);
            auto added = add_bundled(relative);
            if (!added) return std::unexpected(added.error());
        }
        auto added = add_bundled(kLoadingScreenRelPath);
        if (!added) return std::unexpected(added.error());
    }
    if (!extension_pack) {
        auto added = add_bundled(k_ground_relief_attribution);
        if (!added) return std::unexpected(added.error());
        added = add_bundled(k_bcdec_attribution);
        if (!added) return std::unexpected(added.error());
    }

    nlohmann::json texture_sets_json;
    if (texture_set) {
        nlohmann::json textures = nlohmann::json::object();
        for (const ValidatedTextureVariant& variant : texture_set->variants) {
            if (!variant.alias) {
                auto added = writer.add_file(
                    variant.archive_path, variant.variant_source_path,
                    ZipCompression::Store);
                if (!added) {
                    return std::unexpected(Error{
                        "failed to archive hi-res texture '" +
                        variant.original_path + "': " + added.error().message});
                }
                const uintmax_t size =
                    fs::file_size(variant.variant_source_path, ec);
                if (!ec) outcome.uncompressed_bytes += size;
                ec.clear();
                ++outcome.files;
            }
            textures[variant.original_path] = {
                {"source_sha256", variant.source_sha256},
                {"role", variant.role},
                {"wrap", variant.wrap},
                {"variant", {
                    {"path", variant.archive_path},
                    {"sha256", variant.output_sha256},
                    {"width", variant.width},
                    {"height", variant.height},
                    {"format", variant.format},
                    {"gpu_bytes_2k", variant.gpu_bytes_2k},
                    {"gpu_bytes_4k", variant.gpu_bytes_4k},
                }},
            };
        }

        // The village-ground height DDS is a deterministic packer-owned
        // derivative rather than an authored source texture. Keep it exact at
        // every tier and index it as an alias so a complete hi-res archive has
        // zero runtime fallback events without asking HAT to alter displacement.
        Sha256 generated_digest;
        generated_digest.update(std::as_bytes(std::span(*generated.bytes)));
        const std::string generated_sha256 = generated_digest.finish();
        const auto& generated_data = *generated.bytes;
        if (generated_data.size() < 128 ||
            std::memcmp(generated_data.data(), "DDS ", 4) != 0) {
            return std::unexpected(Error{
                "generated ground-height DDS is structurally invalid"});
        }
        const auto little_u32 = [&](std::size_t offset) {
            return uint32_t(generated_data[offset]) |
                   (uint32_t(generated_data[offset + 1]) << 8u) |
                   (uint32_t(generated_data[offset + 2]) << 16u) |
                   (uint32_t(generated_data[offset + 3]) << 24u);
        };
        const uint32_t generated_height = little_u32(12);
        const uint32_t generated_width = little_u32(16);
        const uint64_t generated_gpu_bytes = generated_data.size() - 128u;
        textures[generated.logical_path] = {
            {"source_sha256", generated_sha256},
            {"role", "height"},
            {"wrap", "repeat"},
            {"variant", {
                {"path", generated.logical_path},
                {"sha256", generated_sha256},
                {"width", generated_width},
                {"height", generated_height},
                {"format", "BC4"},
                {"gpu_bytes_2k", generated_gpu_bytes},
                {"gpu_bytes_4k", generated_gpu_bytes},
            }},
        };
        for (const ValidatedTextureArtifact& artifact : texture_set->artifacts) {
            auto added = writer.add_file(
                artifact.archive_path, artifact.source_path,
                asset_zip_compression(artifact.archive_path));
            if (!added) return std::unexpected(added.error());
            const uintmax_t size = fs::file_size(artifact.source_path, ec);
            if (!ec) outcome.uncompressed_bytes += size;
            ec.clear();
            ++outcome.files;
        }
        const std::string packed_set_manifest =
            data_prefix + "/texture_sets/texture-set.json";
        auto added = writer.add_file(
            packed_set_manifest, texture_set->manifest_path,
            ZipCompression::Deflate);
        if (!added) return std::unexpected(added.error());
        const uintmax_t manifest_size =
            fs::file_size(texture_set->manifest_path, ec);
        if (!ec) outcome.uncompressed_bytes += manifest_size;
        ec.clear();
        ++outcome.files;

        texture_sets_json = {
            {"generator", {
                {"id", texture_set->generator_id},
                {"version", texture_set->generator_version},
                {"manifest_path", packed_set_manifest},
                {"manifest_sha256", texture_set->manifest_sha256},
            }},
            {"estimated_gpu_bytes", {
                {"original", texture_set->original_gpu_bytes +
                             generated_gpu_bytes},
                {"2k", texture_set->high_2k_gpu_bytes +
                       generated_gpu_bytes},
                {"4k", texture_set->ultra_4k_gpu_bytes +
                       generated_gpu_bytes},
            }},
            {"textures", std::move(textures)},
        };
    }

    auto texture_inventory = build_texture_inventory_json(
        used, inventory, source_root, bundled_root, data_prefix);
    if (!texture_inventory) return std::unexpected(texture_inventory.error());

    nlohmann::json manifest{
        {"schema_version", texture_set ? 2 : 1},
        {"format", "heavenbench-assets"},
        {"data_root_prefix", data_prefix},
        {"entry_count", outcome.files},
        {"compression", {
            {"precompressed_media", "store"},
            {"other_assets", "deflate-6"}
        }},
        {"texture_inventory", std::move(*texture_inventory)},
        {"derived_assets", nlohmann::json::array({{
            {"generator_id", generated.metadata.generator_id},
            {"generator_version", generated.metadata.generator_version},
            {"deepbump_revision", generated.metadata.deepbump_revision},
            {"pocketfft_revision", generated.metadata.pocketfft_revision},
            {"bcdec_revision", generated.metadata.bcdec_revision},
            {"source_path", generated.metadata.source_path},
            {"output_path", generated.metadata.output_path},
            {"source_fingerprint_fnv1a64",
             generated.metadata.source_fingerprint},
            {"output_fingerprint_fnv1a64",
             generated.metadata.output_fingerprint},
            {"attribution", std::string(k_ground_relief_attribution)},
            {"codec_attribution", std::string(k_bcdec_attribution)}
        }})}
    };
    if (texture_set) manifest["texture_sets"] = std::move(texture_sets_json);
    const std::string manifest_text = manifest.dump(2) + '\n';
    auto manifest_added = writer.add_bytes(
        std::string(kAssetArchiveManifest),
        std::as_bytes(std::span(manifest_text)),
        ZipCompression::Deflate);
    if (!manifest_added) {
        return std::unexpected(manifest_added.error());
    }

    auto finished = writer.finish();
    if (!finished) return std::unexpected(finished.error());
    auto validated = validate_zip_archive(options.target, true);
    if (!validated) return std::unexpected(validated.error());

    outcome.archive_bytes = fs::file_size(options.target, ec);
    if (ec) outcome.archive_bytes = 0;

    // Move mode is deliberately a post-commit operation. No source disappears
    // until the complete archive has closed and every entry has passed CRC
    // validation.
    if (options.move) {
        for (const fs::path& source : moved_sources) {
            std::error_code remove_ec;
            fs::remove(source, remove_ec);
            if (remove_ec) {
                return std::unexpected(Error{
                    "archive is valid but failed to remove source '" +
                    source.string() + "': " + remove_ec.message()});
            }
        }
    }
    return outcome;
}

} // namespace

Result<void> validate_texture_set_manifest(
    const std::filesystem::path& manifest_path,
    const std::set<std::string>& used_assets,
    const std::filesystem::path& source_root,
    std::string_view data_prefix) {
    auto validated = validate_texture_set(
        manifest_path, used_assets, source_root, data_prefix);
    if (!validated) return std::unexpected(validated.error());
    return {};
}

// Orchestrate the whole pack: own the ordering, every diagnostic, and the
// process exit code. Lower-level helpers never print or exit — they return
// values/Result/outcome structs — so this is the single place where I/O policy
// lives, mirroring how resolve_config owns the CLI/config diagnostics today.

// Stream discipline: every error diagnostic goes to
// stderr; only the success Pack_Report (counts + bytes) goes to stdout. The
// stages run in the current section-8 order, and no Asset_File is written
// until the Pack_Target has been classified as ready.

//   1. Resolve the Data_Root and probe that the Source_Tree exists on disk; any
//      failure is the fatal  case → EXIT_DATA_ROOT before any write.
//   2. Classify the Pack_Target; any non-ready status is fatal pre-write
//      → EXIT_TARGET.
//   3. Discover the Used_Asset_Set (reusing the renderer's resolution) and scan
//      the Source_Tree for the report denominator. A discovery failure means the
//      source scene could not be loaded — a Source_Tree problem → EXIT_DATA_ROOT.
//   4. Write (copy/move) the used extracted set, then copy renderer-owned
//      runtime assets into the same release Data_Root layout. A fatal failure
//      aborts with the written count reported as zero  → EXIT_WRITE.
//   5. Print the Pack_Report. If discovery recorded unresolved
//      references, list each (path + referrer) and exit EXIT_UNRESOLVED —
//      distinct from EXIT_DATA_ROOT; otherwise exit EXIT_OK.
int run_asset_packer(const AppConfig& config, const PackOptions& opts) {
    namespace fs = std::filesystem;

    // ── 1. Resolve the Data_Root / Source_Tree  ──
    // PathResolver::create rejects an empty or whitespace-only Data_Root; a
    // Data_Root that does not exist on disk is the remaining  case, probed
    // explicitly. Either failure is fatal before any write.
    Result<PathResolver> paths = PathResolver::create(config.data_dir);
    if (!paths) {
        std::fprintf(stderr, "Asset packer: could not resolve Data_Root: %s\n",
                     paths.error().message.c_str());
        return EXIT_DATA_ROOT;
    }

    std::error_code ec;
    if (!fs::is_directory(paths->data_root(), ec) || ec) {
        std::fprintf(stderr,
                     "Asset packer: could not resolve Data_Root: '%s' is not an "
                     "existing directory\n",
                     paths->data_root().c_str());
        return EXIT_DATA_ROOT;
    }

    // ── 2. Classify the ZIP target before any write ──
    const fs::path target_path(opts.target);
    if (target_path.extension() != ".zip") {
        std::fprintf(stderr,
                     "Asset packer: Pack_Target must be a .zip archive: '%s'\n",
                     opts.target.c_str());
        return EXIT_TARGET;
    }
    ec.clear();
    if (fs::exists(target_path, ec) && !ec && !opts.overwrite) {
        std::fprintf(stderr,
                     "Asset packer: target archive already exists; use "
                     "--pack-overwrite: '%s'\n",
                     opts.target.c_str());
        return EXIT_TARGET;
    }

    auto assets = AssetFileSystem::open(config.data_dir);
    if (!assets) {
        std::fprintf(stderr, "Asset packer: %s\n",
                     assets.error().message.c_str());
        return EXIT_DATA_ROOT;
    }
    auto generated = ensure_ground_height(*paths, *assets, true);
    if (!generated || !generated->has_value()) {
        std::fprintf(
            stderr, "Asset packer: failed to generate ground height: %s\n",
            generated ? "generator produced no asset"
                      : generated.error().message.c_str());
        return EXIT_WRITE;
    }

    // ── 3. Discover the Used_Asset_Set + scan the Source_Tree ──
    Result<DiscoveryResult> discovery = discover_used_assets(
        config, *paths, std::cref(*assets));
    if (!discovery) {
        std::fprintf(stderr,
                     "Asset packer: failed to discover used assets: %s\n",
                     discovery.error().message.c_str());
        return EXIT_DATA_ROOT;
    }

    const std::set<std::string> scanned = scan_source_tree(*paths);

    std::optional<ValidatedTextureSet> texture_set;
    if (opts.texture_set_manifest) {
        std::error_code root_ec;
        fs::path data_root = fs::weakly_canonical(paths->data_root(), root_ec);
        if (root_ec) data_root = fs::path(paths->data_root()).lexically_normal();
        auto validated = validate_texture_set(
            *opts.texture_set_manifest, discovery->used,
            data_root.parent_path(), data_root.filename().generic_string());
        if (!validated) {
            std::fprintf(stderr, "Asset packer: %s\n",
                         validated.error().message.c_str());
            return EXIT_WRITE;
        }
        texture_set = std::move(*validated);
    }

    // ── 4. Write one deterministic ZIP archive ──
    auto archived =
        write_asset_archive(discovery->used, discovery->texture_inventory,
                            *paths, opts, **generated,
                            texture_set ? &*texture_set : nullptr);
    if (!archived) {
        std::fprintf(stderr, "Asset packer: %s\n",
                     archived.error().message.c_str());
        std::fputs(render_write_failure(opts.target).c_str(), stderr);
        return EXIT_WRITE;
    }

    // ── 5. Build and print the Pack_Report  ──
    PackReport report = compute_pack_report(
        discovery->used, scanned, archived->uncompressed_bytes);
    // ArchivePackOutcome is the authoritative count for both base packs and
    // hi-res-only extension packs. Keep the report partition identity while
    // treating authored source assets omitted from an extension as intentional.
    report.written = archived->files;
    if (texture_set) {
        report.omitted = static_cast<int>(scanned.size());
    }
    report.scanned = report.written + report.omitted;
    report.archive_bytes = archived->archive_bytes;

    std::fputs(render_pack_report(report).c_str(), stdout);

    // Unresolved references: list each missing target with the Asset_File
    // that referenced it, then exit with a code distinct from EXIT_DATA_ROOT
    // The resolved set was still written above.
    if (!discovery->unresolved.empty()) {
        std::fputs(render_unresolved_list(discovery->unresolved).c_str(), stderr);
        return EXIT_UNRESOLVED;
    }

    return EXIT_OK;
}

} // namespace heaven
