/*
 * HeavenBench — Used-asset packer: resolution tap implementation
 *
 * See asset_usage_recorder.h for the role of this accumulator. All filesystem
 * calls use the std::error_code overloads so no exception ever crosses out of
 * the recorder.
 */
#include "engine/asset_usage_recorder.h"

#include <filesystem>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace heaven {

AssetUsageRecorder::AssetUsageRecorder(std::string source_root)
    : source_root_(std::move(source_root)) {}

std::string AssetUsageRecorder::relative_key(
        const std::string& on_disk_path) const {
    // Normalize the path so that distinct spellings of the same file (e.g.
    // "<data>/../core/x" vs a directly-rooted "core/x") collapse to one
    // canonical form before being made relative to the Source_Tree root.
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(fs::path(on_disk_path), ec);
    if (ec) {
        // weakly_canonical does not require the file to exist, so a failure here
        // is unusual; fall back to a lexically-normalized path so a usable
        // Relative_Asset_Path is still produced.
        canonical = fs::path(on_disk_path).lexically_normal();
    }

    const fs::path rel = canonical.lexically_relative(fs::path(source_root_));

    // Use generic (forward-slash) form for a stable, platform-independent
    // Relative_Asset_Path. An empty result means the path could not be made
    // relative to the root; store the canonical form so nothing is silently
    // dropped.
    return rel.empty() ? canonical.generic_string() : rel.generic_string();
}

void AssetUsageRecorder::record_used(const std::string& on_disk_path) {
    used_.insert(relative_key(on_disk_path));
}

void AssetUsageRecorder::record_texture(const std::string& on_disk_path,
                                        std::string_view role,
                                        std::string_view wrap) {
    const std::string key = relative_key(on_disk_path);
    used_.insert(key);
    const std::string effective_role = role.empty() ? "data" : std::string(role);
    const std::string effective_wrap = wrap.empty() ? "repeat" : std::string(wrap);
    auto [it, inserted] = texture_inventory_.try_emplace(
        key, TextureInventoryHint{effective_role, effective_wrap});
    if (inserted) return;
    if (it->second.role != effective_role) {
        const bool both_color =
            (it->second.role == "diffuse" || it->second.role == "emissive" ||
             it->second.role == "color") &&
            (effective_role == "diffuse" || effective_role == "emissive" ||
             effective_role == "color");
        it->second.role = both_color ? "color" : "data";
    }
    if (it->second.wrap != effective_wrap) {
        const bool either_repeats = it->second.wrap == "repeat" ||
                                    effective_wrap == "repeat";
        it->second.wrap = either_repeats ? "repeat" : "clamp";
    }
}

void AssetUsageRecorder::record_unresolved(const std::string& missing_rel,
                                           const std::string& referring_rel) {
    // Dedup on the combined (missing, referrer) key so the same broken edge is
    // listed once while preserving first-seen order.
    const std::string dedup_key = missing_rel + '\x1f' + referring_rel;
    if (!unresolved_seen_.insert(dedup_key).second) return;

    unresolved_.push_back(Unresolved{missing_rel, referring_rel});
}

} // namespace heaven
