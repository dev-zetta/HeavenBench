/*
 * HeavenBench — deterministic DeepBump-inspired ground-height generation
 */
#ifndef HEAVENBENCH_SRC_ENGINE_GROUND_RELIEF_GENERATOR_H_
#define HEAVENBENCH_SRC_ENGINE_GROUND_RELIEF_GENERATOR_H_

#include "engine/asset_file_system.h"
#include "engine/path_resolver.h"

#include <memory>
#include <string>
#include <string_view>

namespace heaven {

inline constexpr std::string_view kGroundNormalRelativePath =
    "heaven/textures/ruins/ruins_ground_n.dds";
inline constexpr std::string_view kGroundHeightRelativePath =
    "heaven/generated/ground/ruins_ground_h.dds";
inline constexpr std::string_view kGroundHeightGeneratorId =
    "deepbump-frankot-chellappa";
inline constexpr uint32_t kGroundHeightGeneratorVersion = 2;
inline constexpr std::string_view kDeepBumpRevision =
    "fad19ba87daed12b1d0410a57e74f3d79e82f78d";
inline constexpr std::string_view kPocketFftRevision =
    "c90e55b3d529f8efa40ed01a20de22405f45fc65";
inline constexpr std::string_view kBcdecRevision =
    "93628fe5627102fe5187b7eeb99122dec6612c36";

// GeneratedAssetMetadata records reproducibility and provenance fields written
// into an asset archive manifest.
struct GeneratedAssetMetadata {
    std::string generator_id;
    uint32_t generator_version = 0;
    std::string deepbump_revision;
    std::string pocketfft_revision;
    std::string bcdec_revision;
    std::string source_path;
    std::string output_path;
    std::string source_fingerprint;
    std::string output_fingerprint;
};

// GeneratedAsset is an immutable logical asset plus the metadata needed to
// reproduce and validate it.
struct GeneratedAsset {
    std::string logical_path;
    SharedAssetBuffer bytes;
    GeneratedAssetMetadata metadata;
};

[[nodiscard]] Result<GeneratedAsset>
generate_ground_height(std::string_view normal_path,
                       std::string_view output_path,
                       AssetFileSystemRef assets);

// Ensure the derived village-ground height is available through the supplied
// filesystem. Existing packed entries are retained; missing entries are
// generated once and mounted as immutable memory.
[[nodiscard]] Result<std::optional<GeneratedAsset>>
ensure_ground_height(const PathResolver& paths, AssetFileSystem& assets,
                     bool regenerate = false);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_GROUND_RELIEF_GENERATOR_H_
