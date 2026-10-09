/*
 * HeavenBench — Used-asset packer: resolution tap
 *
 * AssetUsageRecorder is the GPU-free accumulator that the packer threads into
 * the renderer's existing resolution entry points as an optional, default-null
 * pointer. When the pointer is null (every normal render run) behavior is
 * byte-for-byte unchanged; when the packer supplies a recorder, each file the
 * resolution code successfully opens is reported here.
 *
 * Every recorded on-disk path is normalized (weakly_canonical) and made
 * relative to the canonical Source_Tree root, so that "<data>/../core/x" and
 * "<data>/heaven/x" both yield stable Relative_Asset_Paths ("core/x",
 * "heaven/x"). The used set is a std::set keyed by Relative_Asset_Path: it gives
 * both the at-most-once guarantee  and an enumeration order
 * independent of how the graph was traversed.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_ASSET_USAGE_RECORDER_H_
#define HEAVENBENCH_SRC_ENGINE_ASSET_USAGE_RECORDER_H_

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace heaven {

// A reference whose target could not be located or read, paired with the
// referring Asset_File for the Pack_Report.
struct Unresolved {
    std::string missing_rel_path;    // Relative_Asset_Path of the missing target
    std::string referring_rel_path;  // the Asset_File that referenced it
};

struct TextureInventoryHint {
    // Semantic role comes from the effective inherited material slot. Address
    // mode is the authored value, with Unigine's repeat default made explicit.
    std::string role;
    std::string wrap;
};

class AssetUsageRecorder {
public:
    // Construct from the canonical Source_Tree root (the parent of Data_Root).
    explicit AssetUsageRecorder(std::string source_root);

    // Record a successfully resolved on-disk file as used. The path is
    // normalized (weakly_canonical) and made relative to source_root_ so
    // "<data>/../core/x" and "<data>/heaven/x" both yield stable
    // Relative_Asset_Paths. Idempotent: a path already present is not added
    // twice.
    void record_used(const std::string& on_disk_path);

    // Record a texture through the same canonical path mapping while retaining
    // the effective inherited slot semantic and authored address mode. If one
    // file is bound incompatibly, aggregation degrades it to non-generative
    // data; any repeat binding wins over clamp to protect tiled edges.
    void record_texture(const std::string& on_disk_path,
                        std::string_view role,
                        std::string_view wrap);

    // Record a reference whose target could not be located or read, with the
    // referring file for the report. Deduplicated by
    // (missing, referrer) while preserving insertion order.
    void record_unresolved(const std::string& missing_rel,
                           const std::string& referring_rel);

    [[nodiscard]] const std::set<std::string>& used() const { return used_; }
    [[nodiscard]] const std::vector<Unresolved>& unresolved() const { return unresolved_; }
    [[nodiscard]] const std::map<std::string, TextureInventoryHint>&
    texture_inventory() const { return texture_inventory_; }

private:
    [[nodiscard]] std::string relative_key(
        const std::string& on_disk_path) const;
    std::string           source_root_;     // canonical Source_Tree root
    std::set<std::string> used_;             // dedup + deterministic order
    std::map<std::string, TextureInventoryHint> texture_inventory_;
    std::set<std::string> unresolved_seen_;  // dedup key for the unresolved list
    std::vector<Unresolved> unresolved_;     // insertion order
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_ASSET_USAGE_RECORDER_H_
