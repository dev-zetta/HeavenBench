/*
 * HeavenBench — scene-scoped ZIP tessellation cache
 */
#ifndef HEAVENBENCH_SRC_ENGINE_TESSELLATION_CACHE_ARCHIVE_H_
#define HEAVENBENCH_SRC_ENGINE_TESSELLATION_CACHE_ARCHIVE_H_

#include "engine/asset_file_system.h"
#include "engine/types.h"
#include "engine/skinned_mesh.h"

#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include <string>

namespace heaven {

inline constexpr uint32_t kTessellationArchiveVersion = 36;

// TessellationCacheParameters is the stable, implementation-independent
// identity of one topology payload and its displacement/seam inputs.
struct TessellationCacheParameters {
    std::string density_path;
    std::string height_path;
    std::string source_normal_path;
    std::string generator_revision;
    float density_factor = 1.0f;
    float edge_scale = 1.0f;
    int subdivisions = 0;
    std::array<float, 4> base_transform{1.0f, 1.0f, 0.0f, 0.0f};
    float boundary_fade = 0.0f;
    uint32_t surface_policy_version = 0;
};

// Scene-scoped, versioned ZIP cache for validated static and skinned
// tessellation payloads, with staged misses and one atomic publication.
class TessellationCacheArchive {
public:
    struct Impl;

    TessellationCacheArchive();
    ~TessellationCacheArchive();
    TessellationCacheArchive(TessellationCacheArchive&&) noexcept;
    TessellationCacheArchive& operator=(TessellationCacheArchive&&) noexcept;
    TessellationCacheArchive(const TessellationCacheArchive&) = delete;
    TessellationCacheArchive& operator=(const TessellationCacheArchive&) = delete;

    [[nodiscard]] static Result<TessellationCacheArchive> open(
        std::string archive_file, AssetFileSystemRef assets = std::nullopt,
        std::string legacy_cache_dir = {});

    [[nodiscard]] bool load(
        Surface& surface, const std::string& mesh_path,
        const std::string& surface_name,
        const TessellationCacheParameters& parameters);
    void stage(
        const Surface& surface, const std::string& mesh_path,
        const std::string& surface_name,
        const TessellationCacheParameters& parameters,
        uint64_t source_vertex_count = 0,
        uint64_t source_index_count = 0);

    [[nodiscard]] bool load_skinned(
        SkinnedSurface& surface, const std::string& mesh_path,
        const std::string& surface_name,
        const TessellationCacheParameters& parameters);
    void stage_skinned(
        const SkinnedSurface& surface, const std::string& mesh_path,
        const std::string& surface_name,
        const TessellationCacheParameters& parameters,
        uint64_t source_vertex_count = 0,
        uint64_t source_index_count = 0);

    // Publishes one libzip atomic replacement. Failure is returned so callers
    // can diagnose it while continuing with the freshly tessellated scene.
    [[nodiscard]] Result<void> finish();

    [[nodiscard]] const std::string& archive_file() const noexcept;

private:
    explicit TessellationCacheArchive(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string default_tessellation_cache_archive(
    const std::string& data_root, bool data_root_is_zip);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_TESSELLATION_CACHE_ARCHIVE_H_
