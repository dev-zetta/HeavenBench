/*
 * HeavenBench — directory / ZIP-set backed read-only asset file system
 */
#ifndef HEAVENBENCH_SRC_ENGINE_ASSET_FILE_SYSTEM_H_
#define HEAVENBENCH_SRC_ENGINE_ASSET_FILE_SYSTEM_H_

#include "engine/types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace heaven {

struct ZipArchiveHandle;
class AssetFileSystem;

struct TextureVariantInfo {
    std::string original_path;
    std::string variant_path;
    std::string source_sha256;
    std::string output_sha256;
    std::string format;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t gpu_bytes_2k = 0;
    uint64_t gpu_bytes_4k = 0;
};

struct TextureSetSummary {
    bool available = false;
    uint64_t original_gpu_bytes = 0;
    uint64_t high_2k_gpu_bytes = 0;
    uint64_t ultra_4k_gpu_bytes = 0;
    uint64_t texture_count = 0;
};

using AssetBuffer = std::vector<std::uint8_t>;
using SharedAssetBuffer = std::shared_ptr<const AssetBuffer>;
using AssetFileSystemRef =
    std::optional<std::reference_wrapper<const AssetFileSystem>>;

inline constexpr std::string_view kAssetArchiveManifest =
    "heavenbench-assets.json";

enum class AssetSeekOrigin {
    Begin,
    Current,
    End,
};

// A seekable, size-aware byte stream over either a native file or one ZIP
// entry. Implementations read directly into caller-owned buffers.
class AssetStream {
public:
    virtual ~AssetStream() = default;

    [[nodiscard]] virtual uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual uint64_t tell() const noexcept = 0;
    [[nodiscard]] virtual Result<std::size_t>
    read(std::span<std::byte> destination) = 0;
    [[nodiscard]] virtual Result<void>
    seek(int64_t offset, AssetSeekOrigin origin) = 0;
};

// Read-only logical filesystem that mounts either a loose Data_Root directory,
// one manifest-bearing ZIP archive, or a directory of manifest-bearing ZIP
// archives behind identical lookup/search semantics.
class AssetFileSystem {
public:
    enum class Kind {
        Directory,
        Zip,
        ZipDirectory,
        Memory,
    };

    AssetFileSystem() = default;

    // `data_root` is either the existing loose Heaven Data_Root directory, a
    // .zip produced by --pack-assets, or a directory containing *.zip packs.
    [[nodiscard]] static Result<AssetFileSystem>
    open(std::string data_root);

    // A generated-only namespace. Missing files never fall through to disk.
    [[nodiscard]] static Result<AssetFileSystem>
    create_memory(const std::string& data_root);

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_zip() const noexcept {
        return kind_ == Kind::Zip || kind_ == Kind::ZipDirectory;
    }
    [[nodiscard]] const std::string& data_root() const noexcept {
        return data_root_;
    }
    [[nodiscard]] const std::string& data_prefix() const noexcept {
        return data_prefix_;
    }
    [[nodiscard]] int manifest_schema_version() const noexcept {
        return manifest_schema_version_;
    }
    [[nodiscard]] const TextureSetSummary& texture_set_summary() const noexcept {
        return texture_set_summary_;
    }

    // Resolve metadata for a generated full-resolution mip chain. The input is
    // any path spelling accepted by open_read(); schema-v1 archives and loose
    // trees simply return no variant.
    [[nodiscard]] std::optional<TextureVariantInfo>
    texture_variant(std::string_view original_path) const;

    // Paths may be PathResolver results (Data_Root-prefixed) or normalized
    // source-tree-relative archive entry names.
    [[nodiscard]] bool exists(std::string_view path) const;
    [[nodiscard]] Result<std::unique_ptr<AssetStream>>
    open_read(std::string_view path) const;
    [[nodiscard]] Result<AssetBuffer>
    read_all(std::string_view path) const;
    [[nodiscard]] Result<SharedAssetBuffer>
    read_all_shared(std::string_view path) const;

    // Mount one immutable generated asset. The path is normalized into the
    // same source-tree-relative namespace as directory and ZIP entries.
    [[nodiscard]] Result<void>
    mount_memory(std::string_view path,
                 SharedAssetBuffer bytes);

    // Deterministic recursive search. Results use the same path vocabulary as
    // the requested root: native paths for a directory mount and virtual
    // Data_Root-prefixed paths for a ZIP mount.
    [[nodiscard]] std::vector<std::string>
    find(std::string_view root, std::string_view suffix) const;

    // Stable source-tree-relative path used by archive entries, usage
    // recording, and cache identities.
    [[nodiscard]] Result<std::string>
    logical_path(std::string_view path) const;

private:
    Kind kind_ = Kind::Directory;
    std::string data_root_;
    std::filesystem::path directory_source_root_;
    std::string data_prefix_;
    std::vector<std::string> entries_;
    std::unordered_set<std::string> entry_index_;
    std::unordered_map<std::string, uint64_t> entry_sizes_;
    std::unordered_map<std::string, uint64_t> entry_indices_;
    std::unordered_map<std::string, std::shared_ptr<ZipArchiveHandle>>
        entry_archives_;
    std::unordered_map<std::string, SharedAssetBuffer>
        memory_entries_;
    int manifest_schema_version_ = 0;
    TextureSetSummary texture_set_summary_{};
    std::unordered_map<std::string, TextureVariantInfo> texture_variants_;

    [[nodiscard]] Result<std::string>
    zip_entry_path(std::string_view path) const;
    [[nodiscard]] std::string
    virtual_path(std::string_view entry) const;
    [[nodiscard]] Result<std::filesystem::path>
    directory_path(std::string_view path) const;
};

// Open or buffer one asset through an optional mounted filesystem. When no
// mount is supplied, `path` is opened directly as a native file. These helpers
// keep fallback and diagnostic behavior out of individual format loaders.
[[nodiscard]] Result<std::unique_ptr<AssetStream>>
open_asset_stream(std::string_view path,
                  AssetFileSystemRef assets = std::nullopt);
[[nodiscard]] Result<AssetBuffer>
read_asset_file(std::string_view path,
                AssetFileSystemRef assets = std::nullopt);
[[nodiscard]] Result<SharedAssetBuffer>
read_asset_file_shared(std::string_view path,
                       AssetFileSystemRef assets = std::nullopt);
[[nodiscard]] Result<std::string>
read_asset_text(std::string_view path,
                AssetFileSystemRef assets = std::nullopt);
[[nodiscard]] bool
asset_exists(std::string_view path,
             AssetFileSystemRef assets = std::nullopt);

[[nodiscard]] bool asset_path_has_suffix(std::string_view value,
                                         std::string_view suffix) noexcept;

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_ASSET_FILE_SYSTEM_H_
