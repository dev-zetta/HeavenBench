/*
 * HeavenBench — directory / ZIP-set backed read-only asset file system
 */
#include "engine/asset_file_system.h"
#include "engine/loader_validation.h"

#include <zip.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <ranges>
#include "json.hpp"
#include <system_error>
#include <utility>

namespace heaven {

struct ZipArchiveClose {
    void operator()(zip_t* archive) const noexcept {
        if (archive) zip_close(archive);
    }
};

struct ZipFileClose {
    void operator()(zip_file_t* file) const noexcept {
        if (file) zip_fclose(file);
    }
};

using ZipArchiveOwner = std::unique_ptr<zip_t, ZipArchiveClose>;
using ZipFileOwner = std::unique_ptr<zip_file_t, ZipFileClose>;

// Shared owner for one read-only libzip mount. Streams retain this handle so
// the central directory remains valid for their entire lifetime. All entry
// operations share the archive mutex: libzip source offsets and its open-file
// list are shared even by streams reading different entries.
struct ZipArchiveHandle {
    explicit ZipArchiveHandle(ZipArchiveOwner archive)
        : archive(std::move(archive)) {}
    ZipArchiveOwner archive;
    std::mutex io_mutex;
};

namespace {

constexpr uint64_t kMaximumAssetBytes =
    std::min<uint64_t>(
        static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()),
        8ull * 1024 * 1024 * 1024);

[[nodiscard]] bool is_sha256_hex(std::string_view value) noexcept {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') ||
                      (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F');
           });
}

[[nodiscard]] std::string zip_error_text(int code) {
    zip_error_t error;
    zip_error_init_with_code(&error, code);
    std::string text = zip_error_strerror(&error);
    zip_error_fini(&error);
    return text;
}

[[nodiscard]] Result<std::string>
normalize_logical(std::string_view input) {
    if (input.empty()) {
        return std::unexpected(Error{"asset path is empty"});
    }
    if (input.front() == '/' || input.front() == '\\') {
        return std::unexpected(Error{"asset path must be relative"});
    }
    if (input.find('\\') != std::string_view::npos) {
        return std::unexpected(Error{"asset path contains a backslash"});
    }

    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (begin <= input.size()) {
        const std::size_t end = input.find('/', begin);
        const std::string_view part =
            input.substr(begin, end == std::string_view::npos
                                    ? input.size() - begin
                                    : end - begin);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (parts.empty()) {
                    return std::unexpected(
                        Error{"asset path escapes the source tree"});
                }
                parts.pop_back();
            } else {
                parts.push_back(part);
            }
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    if (parts.empty()) {
        return std::unexpected(Error{"asset path resolves to the source root"});
    }

    std::string normalized;
    for (std::string_view part : parts) {
        if (!normalized.empty()) normalized.push_back('/');
        normalized.append(part);
    }
    return normalized;
}

// Native-file implementation that reads directly into the destination spans
// supplied by format loaders.
class DirectoryAssetStream final : public AssetStream {
public:
    DirectoryAssetStream(std::FILE* file, uint64_t size)
        : file_(file), size_(size) {}

    ~DirectoryAssetStream() override {
        if (file_) std::fclose(file_);
    }

    uint64_t size() const noexcept override { return size_; }
    uint64_t tell() const noexcept override { return position_; }

    Result<std::size_t> read(std::span<std::byte> destination) override {
        if (destination.empty()) return std::size_t{0};
        const std::size_t count =
            std::fread(destination.data(), 1, destination.size(), file_);
        position_ += count;
        if (count != destination.size() && std::ferror(file_)) {
            return std::unexpected(Error{
                "failed to read asset stream: " +
                std::string(std::strerror(errno))});
        }
        return count;
    }

    Result<void> seek(int64_t offset, AssetSeekOrigin origin) override {
        int whence = SEEK_SET;
        switch (origin) {
            case AssetSeekOrigin::Begin: whence = SEEK_SET; break;
            case AssetSeekOrigin::Current: whence = SEEK_CUR; break;
            case AssetSeekOrigin::End: whence = SEEK_END; break;
        }
#ifdef _WIN32
        if (_fseeki64(file_, offset, whence) != 0) {
            return std::unexpected(Error{"failed to seek asset stream"});
        }
        const auto next = _ftelli64(file_);
#else
        if (fseeko(file_, static_cast<off_t>(offset), whence) != 0) {
            return std::unexpected(Error{"failed to seek asset stream"});
        }
        const auto next = ftello(file_);
#endif
        if (next < 0) {
            return std::unexpected(Error{"failed to tell asset stream"});
        }
        position_ = static_cast<uint64_t>(next);
        return {};
    }

private:
    std::FILE* file_ = nullptr;
    uint64_t size_ = 0;
    uint64_t position_ = 0;
};

// Seekable stream over an immutable generated asset. It retains shared
// ownership of the backing bytes and copies only into caller-provided spans.
class MemoryAssetStream final : public AssetStream {
public:
    explicit MemoryAssetStream(std::shared_ptr<const AssetBuffer> bytes)
        : bytes_(std::move(bytes)) {}

    uint64_t size() const noexcept override { return bytes_->size(); }
    uint64_t tell() const noexcept override { return position_; }

    Result<std::size_t> read(std::span<std::byte> destination) override {
        const std::size_t remaining = bytes_->size() - position_;
        const std::size_t count = std::min(destination.size(), remaining);
        if (count != 0) {
            std::memcpy(destination.data(), bytes_->data() + position_, count);
            position_ += count;
        }
        return count;
    }

    Result<void> seek(int64_t offset, AssetSeekOrigin origin) override {
        const int64_t base = origin == AssetSeekOrigin::Begin
            ? 0
            : origin == AssetSeekOrigin::Current
                ? static_cast<int64_t>(position_)
                : static_cast<int64_t>(bytes_->size());
        if ((offset > 0 && base > std::numeric_limits<int64_t>::max() - offset) ||
            (offset < 0 && base < std::numeric_limits<int64_t>::min() - offset)) {
            return std::unexpected(Error{"memory asset seek overflow"});
        }
        const int64_t target = base + offset;
        if (target < 0 || static_cast<uint64_t>(target) > bytes_->size()) {
            return std::unexpected(Error{"memory asset seek is outside the entry"});
        }
        position_ = static_cast<std::size_t>(target);
        return {};
    }

private:
    std::shared_ptr<const AssetBuffer> bytes_;
    std::size_t position_ = 0;
};

template <typename Buffer>
Result<Buffer> buffer_stream_as(
        Result<std::unique_ptr<AssetStream>> stream_result) {
    if (!stream_result) {
        return std::unexpected(stream_result.error());
    }
    auto stream = std::move(*stream_result);
    if (stream->size() > kMaximumAssetBytes) {
        return std::unexpected(Error{"asset is too large to buffer"});
    }
    Buffer bytes(static_cast<std::size_t>(stream->size()),
                 typename Buffer::value_type{});
    auto destination = std::as_writable_bytes(std::span(bytes));
    std::size_t offset = 0;
    while (offset < destination.size()) {
        auto count = stream->read(destination.subspan(offset));
        if (!count) return std::unexpected(count.error());
        if (*count == 0) {
            return std::unexpected(Error{"unexpected end of asset stream"});
        }
        offset += *count;
    }
    return bytes;
}

Result<AssetBuffer> buffer_stream(
        Result<std::unique_ptr<AssetStream>> stream_result) {
    return buffer_stream_as<AssetBuffer>(std::move(stream_result));
}

// ZIP-entry implementation with direct destination-buffer reads, native seek
// for stored entries, and reopen/replay fallback for compressed entries.
class ZipAssetStream final : public AssetStream {
public:
    static Result<std::unique_ptr<ZipAssetStream>>
    create(std::shared_ptr<ZipArchiveHandle> archive,
           std::string entry,
           uint64_t index,
           uint64_t size) {
        std::scoped_lock lock(archive->io_mutex);
        ZipFileOwner file(zip_fopen_index(
            archive->archive.get(), index, ZIP_FL_UNCHANGED));
        if (!file) {
            std::string detail = zip_strerror(archive->archive.get());
            return std::unexpected(
                Error{"failed to open ZIP entry '" + entry + "': " + detail});
        }
        return std::unique_ptr<ZipAssetStream>(
            new ZipAssetStream(std::move(entry), std::move(archive),
                               std::move(file), index, size));
    }

    uint64_t size() const noexcept override { return size_; }
    uint64_t tell() const noexcept override { return position_; }

    ~ZipAssetStream() override {
        std::scoped_lock lock(archive_->io_mutex);
        file_.reset();
    }

    Result<std::size_t> read(std::span<std::byte> destination) override {
        std::scoped_lock lock(archive_->io_mutex);
        return read_locked(destination);
    }

    Result<void> seek(int64_t offset, AssetSeekOrigin origin) override {
        std::scoped_lock lock(archive_->io_mutex);
        if (!file_) return std::unexpected(Error{"ZIP entry is not open"});
        int64_t target = 0;
        switch (origin) {
            case AssetSeekOrigin::Begin:
                target = offset;
                break;
            case AssetSeekOrigin::Current:
                if (offset > 0 &&
                    std::cmp_greater(position_,
                                     std::numeric_limits<int64_t>::max() - offset)) {
                    return std::unexpected(Error{"ZIP seek overflow"});
                }
                target = static_cast<int64_t>(position_) + offset;
                break;
            case AssetSeekOrigin::End:
                if (std::cmp_greater(size_, std::numeric_limits<int64_t>::max()) ||
                    (offset > 0 && std::cmp_greater(size_,
                        std::numeric_limits<int64_t>::max() - offset))) {
                    return std::unexpected(Error{"ZIP entry is too large to seek"});
                }
                target = static_cast<int64_t>(size_) + offset;
                break;
        }
        if (target < 0 || std::cmp_greater(target, size_)) {
            return std::unexpected(Error{"ZIP seek is outside the entry"});
        }

        if (zip_file_is_seekable(file_.get()) == 1 &&
            zip_fseek(file_.get(), target, SEEK_SET) == 0) {
            position_ = static_cast<uint64_t>(target);
            return {};
        }

        // Deflated entries are not natively seekable. Preserve streaming:
        // forward seeks discard bytes, while backward seeks reopen the entry
        // and replay from the start.
        if (std::cmp_less(target, position_)) {
            file_.reset();
            file_.reset(zip_fopen_index(
                archive_->archive.get(), index_, ZIP_FL_UNCHANGED));
            if (!file_) {
                return std::unexpected(
                    Error{"failed to reopen ZIP entry '" + entry_ + "'"});
            }
            position_ = 0;
        }
        std::array<std::byte, std::size_t{64} * 1024> scratch{};
        while (std::cmp_less(position_, target)) {
            const uint64_t remaining =
                static_cast<uint64_t>(target) - position_;
            const std::size_t amount = static_cast<std::size_t>(
                std::min<uint64_t>(remaining, scratch.size()));
            auto read_result = read_locked(std::span<std::byte>(scratch).first(amount));
            if (!read_result) return std::unexpected(read_result.error());
            if (*read_result == 0) {
                return std::unexpected(Error{"unexpected end of ZIP entry"});
            }
        }
        return {};
    }

private:
    Result<std::size_t> read_locked(std::span<std::byte> destination) {
        if (!file_) return std::unexpected(Error{"ZIP entry is not open"});
        if (destination.empty()) return std::size_t{0};
        const zip_int64_t count =
            zip_fread(file_.get(), destination.data(), destination.size());
        if (count < 0) {
            return std::unexpected(
                Error{"failed to read ZIP entry '" + entry_ + "': " +
                      zip_file_strerror(file_.get())});
        }
        position_ += static_cast<uint64_t>(count);
        if (count > 0 && position_ == size_) {
            // Force libzip through logical EOF now. This is where it verifies
            // the entry CRC; relying on the destructor's zip_fclose would make
            // a fully consumed corrupt stream appear successful to the caller.
            std::byte sentinel{};
            const zip_int64_t tail = zip_fread(file_.get(), &sentinel, 1);
            if (tail < 0) {
                return std::unexpected(
                    Error{"ZIP entry CRC/read validation failed for '" +
                          entry_ + "': " +
                          zip_file_strerror(file_.get())});
            }
            if (tail != 0) {
                return std::unexpected(
                    Error{"ZIP entry exceeds its declared size: " + entry_});
            }
        }
        return static_cast<std::size_t>(count);
    }

    ZipAssetStream(std::string entry,
                   std::shared_ptr<ZipArchiveHandle> archive,
                   ZipFileOwner file,
                   uint64_t index,
                   uint64_t size)
        : entry_(std::move(entry)),
          archive_(std::move(archive)),
          file_(std::move(file)),
          index_(index),
          size_(size) {}

    std::string entry_;
    std::shared_ptr<ZipArchiveHandle> archive_;
    ZipFileOwner file_;
    uint64_t index_ = 0;
    uint64_t size_ = 0;
    uint64_t position_ = 0;
};

[[nodiscard]] Result<std::vector<std::byte>>
read_zip_entry(zip_t& archive, std::string_view name, uint64_t size) {
    if (size > kMaximumAssetBytes) {
        return std::unexpected(Error{"ZIP entry is too large"});
    }
    ZipFileOwner file(
        zip_fopen(&archive, std::string(name).c_str(), ZIP_FL_ENC_UTF_8));
    if (!file) {
        return std::unexpected(
            Error{"failed to open ZIP entry '" + std::string(name) + "'"});
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const zip_int64_t count =
            zip_fread(file.get(), bytes.data() + offset,
                      bytes.size() - offset);
        if (count <= 0) {
            const std::string detail = zip_file_strerror(file.get());
            return std::unexpected(
                Error{"failed to read ZIP entry '" + std::string(name) +
                      "': " + detail});
        }
        offset += static_cast<std::size_t>(count);
    }
    if (zip_fclose(file.release()) != 0) {
        return std::unexpected(
            Error{"CRC validation failed for ZIP entry '" +
                  std::string(name) + "'"});
    }
    return bytes;
}

} // namespace

bool asset_path_has_suffix(std::string_view value,
                           std::string_view suffix) noexcept {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

Result<AssetFileSystem>
AssetFileSystem::open(std::string data_root) {
    namespace fs = std::filesystem;
    if (data_root.empty()) {
        return std::unexpected(Error{"asset Data_Root is empty"});
    }

    AssetFileSystem result;
    result.data_root_ = std::move(data_root);

    std::error_code ec;
    const fs::path candidate(result.data_root_);
    std::vector<fs::path> archive_paths;
    if (fs::is_directory(candidate, ec) && !ec) {
        fs::directory_iterator it(candidate, ec);
        const fs::directory_iterator end;
        while (!ec && it != end) {
            std::error_code type_ec;
            if (it->is_regular_file(type_ec) && !type_ec &&
                it->path().extension() == ".zip") {
                archive_paths.push_back(it->path());
            }
            it.increment(ec);
        }
        if (ec) {
            return std::unexpected(Error{
                "failed to enumerate asset archive directory: " + ec.message()});
        }
        std::sort(archive_paths.begin(), archive_paths.end());
        if (archive_paths.empty()) {
            result.kind_ = Kind::Directory;
            fs::path canonical = fs::weakly_canonical(candidate, ec);
            if (ec) canonical = candidate.lexically_normal();
            result.directory_source_root_ = canonical.parent_path();
            result.data_prefix_ = canonical.filename().generic_string();
            if (result.data_prefix_.empty()) {
                return std::unexpected(
                    Error{"asset Data_Root has no directory name"});
            }
            return result;
        }
        result.kind_ = Kind::ZipDirectory;
    } else {
        ec.clear();
        if (!fs::is_regular_file(candidate, ec) || ec ||
            candidate.extension() != ".zip") {
            return std::unexpected(Error{
                "asset Data_Root is neither a loose directory, a ZIP directory, nor a .zip file: " +
                result.data_root_});
        }
        result.kind_ = Kind::Zip;
        archive_paths.push_back(candidate);
    }

    struct ManifestRecord {
        fs::path archive_path;
        nlohmann::json document;
        int schema_version = 0;
    };
    std::vector<ManifestRecord> manifests;
    for (const fs::path& configured_path : archive_paths) {
        ec.clear();
        fs::path archive_path = fs::weakly_canonical(configured_path, ec);
        if (ec) archive_path = configured_path.lexically_normal();

        int error = 0;
        ZipArchiveOwner archive(
            zip_open(archive_path.string().c_str(), ZIP_RDONLY, &error));
        if (!archive) {
            return std::unexpected(Error{
                "failed to open asset archive '" + archive_path.string() +
                "': " + zip_error_text(error)});
        }

        zip_stat_t manifest_stat{};
        zip_stat_init(&manifest_stat);
        if (zip_stat(archive.get(), std::string(kAssetArchiveManifest).c_str(),
                     ZIP_FL_ENC_UTF_8, &manifest_stat) != 0 ||
            !(manifest_stat.valid & ZIP_STAT_SIZE)) {
            return std::unexpected(Error{
                "asset archive is missing heavenbench-assets.json: " +
                archive_path.string()});
        }
        auto manifest_bytes = read_zip_entry(
            *archive, kAssetArchiveManifest, manifest_stat.size);
        if (!manifest_bytes) return std::unexpected(manifest_bytes.error());

        ManifestRecord manifest_record;
        manifest_record.archive_path = archive_path;
        try {
            const std::string text(
                reinterpret_cast<const char*>(manifest_bytes->data()),
                manifest_bytes->size());
            manifest_record.document = nlohmann::json::parse(text);
            manifest_record.schema_version =
                manifest_record.document.value("schema_version", 0);
            if ((manifest_record.schema_version != 1 &&
                 manifest_record.schema_version != 2) ||
                !manifest_record.document.contains("data_root_prefix") ||
                !manifest_record.document["data_root_prefix"].is_string()) {
                return std::unexpected(Error{
                    "asset archive manifest has an unsupported schema: " +
                    archive_path.string()});
            }
            auto prefix = normalize_logical(
                manifest_record.document["data_root_prefix"].get<std::string>());
            if (!prefix || prefix->find('/') != std::string::npos) {
                return std::unexpected(Error{
                    "asset archive manifest has an invalid Data_Root prefix: " +
                    archive_path.string()});
            }
            if (result.data_prefix_.empty()) {
                result.data_prefix_ = *prefix;
            } else if (result.data_prefix_ != *prefix) {
                return std::unexpected(Error{
                    "asset archives declare different Data_Root prefixes"});
            }
        } catch (const std::exception& ex) {
            return std::unexpected(Error{
                "failed to parse asset archive manifest '" +
                archive_path.string() + "': " + ex.what()});
        }

        const zip_int64_t count =
            zip_get_num_entries(archive.get(), ZIP_FL_UNCHANGED);
        if (count < 0) {
            return std::unexpected(Error{
                "failed to enumerate asset archive '" + archive_path.string() +
                "': " + zip_strerror(archive.get())});
        }
        struct IndexedEntry {
            std::string path;
            uint64_t index = 0;
            uint64_t size = 0;
        };
        std::vector<IndexedEntry> indexed_entries;
        std::unordered_set<std::string> archive_entries;
        indexed_entries.reserve(static_cast<std::size_t>(count));
        archive_entries.reserve(static_cast<std::size_t>(count));
        for (zip_uint64_t i = 0; std::cmp_less(i, count); ++i) {
            zip_stat_t stat{};
            zip_stat_init(&stat);
            if (zip_stat_index(archive.get(), i, ZIP_FL_UNCHANGED, &stat) != 0 ||
                !(stat.valid & ZIP_STAT_NAME) || !stat.name) {
                return std::unexpected(Error{
                    "invalid entry in asset archive '" + archive_path.string() +
                    "': " + zip_strerror(archive.get())});
            }
            const std::string_view raw(stat.name);
            if (raw.empty() || raw.back() == '/') continue;
            auto normalized = normalize_logical(raw);
            if (!normalized || *normalized != raw) {
                return std::unexpected(Error{
                    "asset archive contains an unsafe path: " +
                    std::string(raw)});
            }
            if (!(stat.valid & ZIP_STAT_SIZE) || stat.size > kMaximumAssetBytes) {
                return std::unexpected(Error{
                    "asset archive entry has an invalid size: " + *normalized});
            }
            if (!archive_entries.insert(*normalized).second) {
                return std::unexpected(Error{
                    "asset archive contains a duplicate path: " + *normalized});
            }
            // Every pack owns a manifest. It is metadata for that physical
            // archive, not an overlayable logical asset.
            if (*normalized == kAssetArchiveManifest &&
                result.entry_index_.contains(*normalized)) {
                continue;
            }
            if (!result.entry_index_.insert(*normalized).second) {
                return std::unexpected(Error{
                    "asset archives contain a duplicate path: " + *normalized});
            }
            indexed_entries.push_back(
                IndexedEntry{std::move(*normalized), i, stat.size});
        }

        auto handle =
            std::make_shared<ZipArchiveHandle>(std::move(archive));
        for (auto& entry : indexed_entries) {
            result.entry_sizes_.emplace(entry.path, entry.size);
            result.entry_indices_.emplace(entry.path, entry.index);
            result.entry_archives_.emplace(entry.path, handle);
            result.entries_.push_back(std::move(entry.path));
        }
        result.manifest_schema_version_ = std::max(
            result.manifest_schema_version_, manifest_record.schema_version);
        manifests.push_back(std::move(manifest_record));
    }
    std::sort(result.entries_.begin(), result.entries_.end());

    try {
        for (const ManifestRecord& manifest_record : manifests) {
            if (manifest_record.schema_version != 2) continue;
            const auto& texture_sets =
                manifest_record.document.at("texture_sets");
            const auto& estimates = texture_sets.at("estimated_gpu_bytes");
            const auto& textures = texture_sets.at("textures");
            if (!texture_sets.is_object() || !estimates.is_object() ||
                !textures.is_object()) {
                return std::unexpected(Error{
                    "asset archive manifest has an invalid texture_sets index"});
            }
            result.texture_set_summary_.original_gpu_bytes = std::max(
                result.texture_set_summary_.original_gpu_bytes,
                estimates.at("original").get<uint64_t>());
            result.texture_set_summary_.high_2k_gpu_bytes = std::max(
                result.texture_set_summary_.high_2k_gpu_bytes,
                estimates.at("2k").get<uint64_t>());
            result.texture_set_summary_.ultra_4k_gpu_bytes = std::max(
                result.texture_set_summary_.ultra_4k_gpu_bytes,
                estimates.at("4k").get<uint64_t>());

            for (auto it = textures.begin(); it != textures.end(); ++it) {
                auto original = normalize_logical(it.key());
                if (!original || *original != it.key() ||
                    !result.entry_index_.contains(*original)) {
                    return std::unexpected(Error{
                        "asset archive texture_sets contains an invalid original: " +
                        it.key()});
                }
                const auto& value = it.value();
                const auto& variant = value.at("variant");
                TextureVariantInfo info;
                info.original_path = *original;
                info.variant_path = variant.at("path").get<std::string>();
                auto variant_path = normalize_logical(info.variant_path);
                if (!variant_path || *variant_path != info.variant_path ||
                    !result.entry_index_.contains(*variant_path)) {
                    return std::unexpected(Error{
                        "asset archive texture_sets contains an invalid variant for: " +
                        *original});
                }
                info.source_sha256 = value.at("source_sha256").get<std::string>();
                info.output_sha256 = variant.at("sha256").get<std::string>();
                info.format = variant.at("format").get<std::string>();
                info.width = variant.at("width").get<uint32_t>();
                info.height = variant.at("height").get<uint32_t>();
                info.gpu_bytes_2k = variant.at("gpu_bytes_2k").get<uint64_t>();
                info.gpu_bytes_4k = variant.at("gpu_bytes_4k").get<uint64_t>();
                if (info.width == 0 || info.height == 0 ||
                    info.width > loadval::kMaxTexDim ||
                    info.height > loadval::kMaxTexDim ||
                    !is_sha256_hex(info.source_sha256) ||
                    !is_sha256_hex(info.output_sha256) ||
                    !result.texture_variants_
                         .emplace(*original, std::move(info)).second) {
                    return std::unexpected(Error{
                        "asset archive texture_sets contains invalid metadata for: " +
                        *original});
                }
            }
        }
    } catch (const std::exception& ex) {
        return std::unexpected(Error{
            "failed to validate asset archive manifests: " +
            std::string(ex.what())});
    }
    result.texture_set_summary_.texture_count =
        result.texture_variants_.size();
    result.texture_set_summary_.available =
        !result.texture_variants_.empty();
    if (!result.entry_index_.contains(
            result.data_prefix_ + "/heaven/heaven.world")) {
        return std::unexpected(
            Error{"asset archives do not contain their declared Data_Root"});
    }
    return result;
}

Result<AssetFileSystem>
AssetFileSystem::create_memory(const std::string& data_root) {
    auto normalized = normalize_logical(data_root);
    if (!normalized || normalized->empty()) {
        return std::unexpected(Error{"memory asset Data_Root must be a relative logical path"});
    }
    AssetFileSystem result;
    result.kind_ = Kind::Memory;
    result.data_root_ = *normalized;
    result.data_prefix_ = *normalized;
    return result;
}

std::optional<TextureVariantInfo>
AssetFileSystem::texture_variant(std::string_view original_path) const {
    auto logical = logical_path(original_path);
    if (!logical) return std::nullopt;
    const auto it = texture_variants_.find(*logical);
    if (it == texture_variants_.end()) return std::nullopt;
    return it->second;
}

Result<std::string>
AssetFileSystem::zip_entry_path(std::string_view path) const {
    if (!is_zip() && kind_ != Kind::Memory) {
        return std::unexpected(Error{"asset filesystem is not ZIP-backed"});
    }
    const std::string_view root = data_root_;
    std::string logical;
    if (path == root ||
        (path.starts_with(root) && path.size() > root.size() &&
         path[root.size()] == '/')) {
        std::string_view remainder = path.substr(root.size());
        while (!remainder.empty() && remainder.front() == '/') {
            remainder.remove_prefix(1);
        }
        logical = data_prefix_;
        if (!remainder.empty()) {
            logical.push_back('/');
            logical.append(remainder);
        }
    } else {
        if (path.empty() || path.front() == '/' || path.front() == '\\' ||
            (path.size() >= 2 && path[1] == ':')) {
            return std::unexpected(
                Error{"asset path must be relative to its mounted source tree"});
        }
        logical.assign(path);
        if (!logical.starts_with(data_prefix_ + "/") &&
            logical != data_prefix_) {
            logical = data_prefix_ + "/" + logical;
        }
    }
    auto normalized = normalize_logical(logical);
    if (kind_ == Kind::Memory && normalized &&
        *normalized != data_prefix_ && !normalized->starts_with(data_prefix_ + "/")) {
        return std::unexpected(Error{"generated asset path escapes its namespace"});
    }
    return normalized;
}

Result<std::string>
AssetFileSystem::logical_path(std::string_view path) const {
    namespace fs = std::filesystem;
    if (is_zip() || kind_ == Kind::Memory) return zip_entry_path(path);
    auto candidate = directory_path(path);
    if (!candidate) return std::unexpected(candidate.error());
    fs::path relative =
        candidate->lexically_relative(directory_source_root_);
    if (relative.empty()) {
        return std::unexpected(
            Error{"asset path is outside the source tree: " +
                  std::string(path)});
    }
    return normalize_logical(relative.generic_string());
}

Result<std::filesystem::path>
AssetFileSystem::directory_path(std::string_view path) const {
    namespace fs = std::filesystem;
    fs::path candidate(path);
    if (candidate.is_relative()) {
        const std::string generic = candidate.generic_string();
        const fs::path configured_root(data_root_);
        const std::string configured = configured_root.generic_string();
        if (!(generic == configured ||
              generic.starts_with(configured + "/"))) {
            auto normalized = normalize_logical(generic);
            if (!normalized) return std::unexpected(normalized.error());
            candidate = directory_source_root_ / *normalized;
        }
    }
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (ec) canonical = candidate.lexically_normal();
    const fs::path relative =
        canonical.lexically_relative(directory_source_root_);
    if (relative.empty() || relative.generic_string().starts_with("../")) {
        return std::unexpected(
            Error{"asset path is outside the source tree: " +
                  std::string(path)});
    }
    return canonical;
}

bool AssetFileSystem::exists(std::string_view path) const {
    namespace fs = std::filesystem;
    auto logical = logical_path(path);
    if (logical && memory_entries_.contains(*logical)) return true;
    if (kind_ == Kind::Memory) return false;
    if (!is_zip()) {
        auto native = directory_path(path);
        if (!native) return false;
        std::error_code ec;
        return fs::is_regular_file(*native, ec) && !ec;
    }
    auto entry = zip_entry_path(path);
    return entry && entry_index_.contains(*entry);
}

Result<std::unique_ptr<AssetStream>>
AssetFileSystem::open_read(std::string_view path) const {
    namespace fs = std::filesystem;
    auto logical = logical_path(path);
    if (logical) {
        const auto memory = memory_entries_.find(*logical);
        if (memory != memory_entries_.end()) {
            std::unique_ptr<AssetStream> stream =
                std::make_unique<MemoryAssetStream>(memory->second);
            return stream;
        }
    }
    if (kind_ == Kind::Memory) {
        return std::unexpected(Error{"generated asset is missing: " + std::string(path)});
    }
    if (!is_zip()) {
        auto native_result = directory_path(path);
        if (!native_result) return std::unexpected(native_result.error());
        const fs::path native = std::move(*native_result);
        std::error_code ec;
        const uintmax_t file_size = fs::file_size(native, ec);
        if (ec) {
            return std::unexpected(
                Error{"failed to stat asset '" + native.string() +
                      "': " + ec.message()});
        }
        std::FILE* file = std::fopen(native.string().c_str(), "rb");
        if (!file) {
            return std::unexpected(
                Error{"failed to open asset '" + native.string() + "'"});
        }
        return std::unique_ptr<AssetStream>(
            new DirectoryAssetStream(file, file_size));
    }

    auto entry = zip_entry_path(path);
    if (!entry) return std::unexpected(entry.error());
    if (!entry_index_.contains(*entry)) {
        return std::unexpected(
            Error{"asset archive entry does not exist: " + *entry});
    }

    const auto size_it = entry_sizes_.find(*entry);
    const auto index_it = entry_indices_.find(*entry);
    const auto archive_it = entry_archives_.find(*entry);
    if (size_it == entry_sizes_.end() ||
        index_it == entry_indices_.end() ||
        archive_it == entry_archives_.end() || !archive_it->second) {
        return std::unexpected(
            Error{"failed to stat asset archive entry: " + *entry});
    }
    return ZipAssetStream::create(
        archive_it->second, std::move(*entry),
        index_it->second, size_it->second);
}

Result<AssetBuffer>
AssetFileSystem::read_all(std::string_view path) const {
    return buffer_stream(open_read(path));
}

Result<SharedAssetBuffer>
AssetFileSystem::read_all_shared(std::string_view path) const {
    auto logical = logical_path(path);
    if (logical) {
        const auto memory = memory_entries_.find(*logical);
        if (memory != memory_entries_.end()) return memory->second;
    }
    auto bytes = buffer_stream(open_read(path));
    if (!bytes) return std::unexpected(bytes.error());
    SharedAssetBuffer shared =
        std::make_shared<AssetBuffer>(std::move(*bytes));
    return shared;
}

Result<void>
AssetFileSystem::mount_memory(
        std::string_view path,
        SharedAssetBuffer bytes) {
    if (!bytes) {
        return std::unexpected(Error{"cannot mount a null asset buffer"});
    }
    if (bytes->size() > kMaximumAssetBytes) {
        return std::unexpected(Error{"memory asset is too large"});
    }
    auto logical = logical_path(path);
    if (!logical) return std::unexpected(logical.error());
    if (memory_entries_.contains(*logical)) {
        return std::unexpected(
            Error{"asset path is already mounted: " + *logical});
    }
    memory_entries_.emplace(std::move(*logical), std::move(bytes));
    return {};
}

std::string AssetFileSystem::virtual_path(std::string_view entry) const {
    if (entry == data_prefix_) return data_root_;
    if (entry.starts_with(data_prefix_ + "/")) {
        return data_root_ + "/" +
               std::string(entry.substr(data_prefix_.size() + 1));
    }
    // Sibling source-tree roots (notably core) retain the legacy
    // Data_Root/../<root> spelling consumed by PathResolver/material loading.
    return data_root_ + "/../" + std::string(entry);
}

std::vector<std::string>
AssetFileSystem::find(std::string_view root, std::string_view suffix) const {
    namespace fs = std::filesystem;
    std::vector<std::string> result;
    if (kind_ == Kind::Memory) {
        auto logical = logical_path(root);
        if (!logical) return result;
        const std::string prefix = *logical + "/";
        for (const auto& entry : memory_entries_ | std::views::keys) {
            if (entry.starts_with(prefix) && asset_path_has_suffix(entry, suffix))
                result.push_back(virtual_path(entry));
        }
        std::sort(result.begin(), result.end());
        return result;
    }
    if (!is_zip()) {
        std::error_code ec;
        fs::recursive_directory_iterator it(
            fs::path(root), fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        while (!ec && it != end) {
            if (it->is_regular_file(ec)) {
                const std::string path = it->path().generic_string();
                if (asset_path_has_suffix(path, suffix)) {
                    result.push_back(path);
                }
            }
            it.increment(ec);
        }
        auto root_logical = logical_path(root);
        if (root_logical) {
            std::string prefix = *root_logical;
            if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
            for (const auto& entry : memory_entries_ | std::views::keys) {
                if (entry.starts_with(prefix) &&
                    asset_path_has_suffix(entry, suffix)) {
                    result.push_back(virtual_path(entry));
                }
            }
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    auto prefix_result = zip_entry_path(root);
    if (!prefix_result) return result;
    std::string prefix = std::move(*prefix_result);
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
    auto begin = std::lower_bound(entries_.begin(), entries_.end(), prefix);
    for (auto it = begin; it != entries_.end() && it->starts_with(prefix);
         ++it) {
        if (asset_path_has_suffix(*it, suffix)) {
            result.push_back(virtual_path(*it));
        }
    }
    for (const auto& entry : memory_entries_ | std::views::keys) {
        if (entry.starts_with(prefix) &&
            asset_path_has_suffix(entry, suffix)) {
            result.push_back(virtual_path(entry));
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

Result<std::unique_ptr<AssetStream>>
open_asset_stream(std::string_view path, AssetFileSystemRef assets) {
    if (assets) {
        return assets->get().open_read(path);
    }

    namespace fs = std::filesystem;
    const fs::path native(path);
    std::error_code ec;
    const uintmax_t size = fs::file_size(native, ec);
    if (ec || size > kMaximumAssetBytes) {
        return std::unexpected(
            Error{"failed to stat asset '" + native.string() + "': " +
                  (ec ? ec.message() : "file is too large")});
    }
    std::FILE* file = std::fopen(native.string().c_str(), "rb");
    if (!file) {
        return std::unexpected(
            Error{"failed to open asset '" + native.string() + "'"});
    }
    return std::unique_ptr<AssetStream>(
        new DirectoryAssetStream(file, static_cast<uint64_t>(size)));
}

Result<AssetBuffer>
read_asset_file(std::string_view path, AssetFileSystemRef assets) {
    if (assets) return assets->get().read_all(path);
    return buffer_stream(open_asset_stream(path));
}

Result<SharedAssetBuffer>
read_asset_file_shared(std::string_view path, AssetFileSystemRef assets) {
    if (assets) return assets->get().read_all_shared(path);
    auto bytes = buffer_stream(open_asset_stream(path));
    if (!bytes) return std::unexpected(bytes.error());
    SharedAssetBuffer shared =
        std::make_shared<AssetBuffer>(std::move(*bytes));
    return shared;
}

Result<std::string>
read_asset_text(std::string_view path, AssetFileSystemRef assets) {
    return buffer_stream_as<std::string>(
        open_asset_stream(path, assets));
}

bool asset_exists(std::string_view path, AssetFileSystemRef assets) {
    if (assets) return assets->get().exists(path);
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec) &&
           !ec;
}

} // namespace heaven
