/*
 * HeavenBench used-asset packer, shared by the standalone tool and the
 * renderer's compatible --pack-assets command. Discovery reuses the CPU
 * loaders through NullBackend; neither entry point initializes graphics.
 */
#ifndef HEAVENBENCH_TOOLS_ASSET_PACKER_H_
#define HEAVENBENCH_TOOLS_ASSET_PACKER_H_

#include "engine/types.h"
#include "engine/asset_usage_recorder.h"
#include "engine/path_resolver.h"
#include "engine/asset_file_system.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace heaven {

// ── Abstract graph for the pure traversal core ──────────────────

// AssetId is the Relative_Asset_Path (e.g. "heaven/textures/airship/balloon_d.dds").
// The GraphView hides whether edges come from xml0 files, material chains, or an
// in-memory test fixture.
using AssetId = std::string;

// Abstract out-edge view over the Reference_Graph. The production discovery is a
// GraphView adapter over the recorder-tapped resolution; tests instantiate an
// in-memory GraphView to prove the traversal invariants without any I/O.
struct GraphView {
    // The out-edges of `node` if it exists and is readable; std::nullopt if the
    // node could not be resolved (its referrers record it as unresolved).
    [[nodiscard]] virtual std::optional<std::vector<AssetId>>
    references(const AssetId& node) const = 0;

    virtual ~GraphView() = default;
};

// Result of a Reference_Graph traversal.
struct TraversalResult {
    std::set<AssetId>       reachable;   // every resolvable node reached from roots
    std::vector<Unresolved> unresolved;  // (missing, referrer) pairs, dedup'd
};

// Worklist traversal over the abstract GraphView: visited-set dedup, cycle-safe
// (a node's edges are expanded at most once). Unresolved edges are excluded from
// `reachable` and recorded in `unresolved`.
[[nodiscard]] TraversalResult
traverse_reference_graph(const std::vector<AssetId>& roots, const GraphView& graph);

// ── Discovery ───────────────────────────────────────────────────

// The discovered Used_Asset_Set plus the list of unresolved references.
struct DiscoveryResult {
    std::set<std::string>   used;        // Relative_Asset_Paths
    std::vector<Unresolved> unresolved;
    std::map<std::string, TextureInventoryHint> texture_inventory;
};

// Reuses the renderer's resolution end-to-end via NullBackend + recorder, so the
// Used_Asset_Set is identical to what a render run would load.
[[nodiscard]] Result<DiscoveryResult>
discover_used_assets(const AppConfig& config, const PathResolver& paths,
                     AssetFileSystemRef assets = std::nullopt);

// ── Source_Tree scan ────────────────────────────────────────────

// True when `name` ends in `ext` compared case-insensitively, so that the
// music track's `.oga` extension matches regardless of its on-disk case
// spelling (".oga", ".OGA", ".Oga" all match ".oga"). Pure (no I/O), so the
// classifier is host-testable and shared between the scan and its tests

[[nodiscard]] bool has_extension_ci(std::string_view name, std::string_view ext);

// All Asset_Files under the Source_Tree (Data_Root + sibling core), as
// Relative_Asset_Paths. Asset_File extensions only:
//   .world.node.mesh.smesh.mat.dds.track.oga
// The first seven are matched by exact case-sensitive suffix; `.oga` (the music
// track) is matched case-insensitively via has_extension_ci. This set is the
// denominator for the Pack_Report consistency identity.
[[nodiscard]] std::set<std::string>
scan_source_tree(const PathResolver& paths);

// ── Pack report ─────────────────────────────────────────────────

struct PackReport {
    // run_asset_packer includes renderer-owned bundled files in written/scanned;
    // compute_pack_report itself reports only the extracted Source_Tree sets.
    int        written       = 0;  // files written to Pack_Target
    int        omitted       = 0;  // Source_Tree Asset_Files omitted as unused
    int        scanned       = 0;  // source candidates considered for packaging
    uintmax_t  written_bytes = 0;  // total bytes written
    uintmax_t  archive_bytes = 0;  // final ZIP size on disk
};

// Sets scanned = |scanned|, written = |used ∩ scanned|, omitted = scanned −
// written, so written + omitted == scanned by construction.
[[nodiscard]] PackReport
compute_pack_report(const std::set<std::string>& used,
                    const std::set<std::string>& scanned,
                    uintmax_t written_bytes);

// ── Report / error rendering ────────────────────────────────────

// These pure helpers turn the report/error data into the exact text
// run_asset_packer prints, so the rendered output is host-testable without
// driving the orchestrator's I/O and exit decisions. run_asset_packer prints
// these strings verbatim, preserving its stream discipline (the success report
// to stdout; the unresolved list and write-failure indication to stderr).

// Render the success Pack_Report as the multi-line summary printed to stdout on
// a successful pack: the written/omitted/scanned file counts and a non-negative
// written-bytes line. The result ends in a newline.
[[nodiscard]] std::string
render_pack_report(const PackReport& report);

// Render the unresolved-reference list printed to stderr when discovery recorded
// one or more unresolved references: a count header followed by one line per
// entry stating its missing Relative_Asset_Path and the referring Asset_File
// Returns an empty string when there are no unresolved
// references. A non-empty result ends in a newline.
[[nodiscard]] std::string
render_unresolved_list(const std::vector<Unresolved>& unresolved);

// Render the pre-completion write-failure error indication printed to stderr in
// place of the success report when packing fails before completion: an error
// message naming the Pack_Target and reporting the written count as zero
// The result ends in a newline.
[[nodiscard]] std::string
render_write_failure(const std::string& target);

// ── Layout-preserving path mapping ──────────────────────────────

// pack_target_path is the lexical join `root / rel`, preserving the
// Relative_Asset_Path so the Pack_Target is a drop-in Data_Root (Requirement
// source_relative is its inverse against the Source_Tree root.
[[nodiscard]] std::filesystem::path
pack_target_path(const std::filesystem::path& root, const std::string& rel);

[[nodiscard]] std::string
source_relative(const std::filesystem::path& root, const std::filesystem::path& abs);

// ── Target preparation ──────────────────────────────────────────

enum class TargetStatus {
    ReadyCreated,     // target absent or empty → created/usable as-is
    ReadyOverwrite,   // non-empty dir + overwrite → write over it
    ConflictNonEmpty, // non-empty dir, no overwrite → fatal
    NotADirectory,    // path exists but is not a directory → fatal
    CreateFailed,     // create_directories failed → fatal
};

struct TargetDecision {
    TargetStatus status;
    std::string  message;
};

// Classifies the Pack_Target pre-state per the implementation decision table
// Uses std::error_code overloads — no exceptions cross out.
[[nodiscard]] TargetDecision
prepare_pack_target(const std::filesystem::path& target, bool overwrite);

// ── Writer ──────────────────────────────────────────────────────

enum class TransferMode { Copy, Move };

struct WriteOutcome {
    int                      written = 0;     // files successfully placed
    uintmax_t                bytes   = 0;     // total bytes of written files
    std::vector<std::string> failures;        // per-file diagnostics
    bool                     fatal   = false;  // dir-create failure
};

// Writes (Copy) or relocates (Move) each used Asset_File into the Pack_Target at
// its mirrored Relative_Asset_Path, creating parent directories as needed. A
// per-file content-transfer failure is recorded and skipped; a
// directory-creation failure is the fatal  case.
[[nodiscard]] WriteOutcome
write_used_assets(const std::set<std::string>& used,
                  const PathResolver& paths,
                  const std::filesystem::path& target,
                  TransferMode mode);

// ── Orchestrator options + exit codes ───────────────────────────

struct PackOptions {
    std::string target;             // Pack_Target .zip archive
    bool        move      = false;  // Move_Mode opt-in; default Copy
    bool        overwrite = false;  // write into a non-empty target
    // When present, write a hi-res extension pack only. Authored originals
    // remain in the separately mounted base archive.
    std::optional<std::string> texture_set_manifest;
};

// Validates the fail-closed external texture-set contract used by the packer.
// Exposed so compact GPU-free fixtures can exercise missing, stale, duplicate,
// unsafe, and corrupt manifests without running scene discovery first.
[[nodiscard]] Result<void> validate_texture_set_manifest(
    const std::filesystem::path& manifest_path,
    const std::set<std::string>& used_assets,
    const std::filesystem::path& source_root,
    std::string_view data_prefix);

// Process exit codes owned by run_asset_packer (table).
inline constexpr int EXIT_OK         = 0; // success, no unresolved references
inline constexpr int EXIT_USAGE      = 1; // reserved (--pack-assets defaults to assets/assets.zip)
inline constexpr int EXIT_DATA_ROOT  = 2; // Data_Root unresolved — fatal
inline constexpr int EXIT_TARGET     = 3; // target conflict / not-a-dir / mkdir fail
inline constexpr int EXIT_WRITE      = 4; // fatal write (directory creation) failure
inline constexpr int EXIT_UNRESOLVED = 5; // completed with unresolved references

// Orchestrator: owns ordering, all printing, and the exit-code decisions.
// Returns the process exit code (one of the constants above).
[[nodiscard]] int run_asset_packer(const AppConfig& config, const PackOptions& opts);

} // namespace heaven

#endif // HEAVENBENCH_TOOLS_ASSET_PACKER_H_
