/*
 * HeavenBench — small RAII write façade over pinned libzip
 */
#include "engine/zip_archive.h"

#include <zip.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <system_error>
#include <utility>

namespace heaven {
namespace {

[[nodiscard]] Result<std::string>
validate_entry_name(std::string_view name) {
    if (name.empty() || name.front() == '/' ||
        name.find('\\') != std::string_view::npos) {
        return std::unexpected(Error{"invalid ZIP entry path"});
    }
    std::filesystem::path path(name);
    std::string normalized = path.lexically_normal().generic_string();
    if (normalized != name || normalized.starts_with("../") ||
        normalized == "..") {
        return std::unexpected(Error{"unsafe ZIP entry path: " +
                                     std::string(name)});
    }
    return normalized;
}

[[nodiscard]] std::string open_error(int code) {
    zip_error_t error;
    zip_error_init_with_code(&error, code);
    std::string text = zip_error_strerror(&error);
    zip_error_fini(&error);
    return text;
}

} // namespace

ZipArchiveWriter::ZipArchiveWriter(ZipArchiveWriter&& other) noexcept
    : target_(std::move(other.target_)),
      archive_(std::move(other.archive_)),
      finished_(other.finished_),
      owned_buffers_(std::move(other.owned_buffers_)) {
    other.finished_ = true;
}

ZipArchiveWriter&
ZipArchiveWriter::operator=(ZipArchiveWriter&& other) noexcept {
    if (this == &other) return *this;
    discard();
    target_ = std::move(other.target_);
    archive_ = std::move(other.archive_);
    finished_ = other.finished_;
    owned_buffers_ = std::move(other.owned_buffers_);
    other.finished_ = true;
    return *this;
}

ZipArchiveWriter::~ZipArchiveWriter() {
    discard();
}

Result<ZipArchiveWriter>
ZipArchiveWriter::create(std::filesystem::path target, bool overwrite) {
    namespace fs = std::filesystem;
    if (target.extension() != ".zip") {
        return std::unexpected(
            Error{"ZIP archive target must end in .zip"});
    }
    std::error_code ec;
    if (fs::exists(target, ec) && !overwrite) {
        return std::unexpected(
            Error{"ZIP archive already exists: " + target.string()});
    }
    if (!target.parent_path().empty()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            return std::unexpected(
                Error{"failed to create ZIP output directory: " +
                      ec.message()});
        }
    }
    int error = 0;
    zip_t* archive = zip_open(
        target.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!archive) {
        return std::unexpected(
            Error{"failed to create ZIP archive: " + open_error(error)});
    }
    return ZipArchiveWriter(
        std::move(target), ArchiveOwner(archive));
}

void ZipArchiveWriter::ArchiveDiscard::operator()(
        zip_t* archive) const noexcept {
    if (archive) zip_discard(archive);
}

Result<void>
ZipArchiveWriter::configure_entry(uint64_t index,
                                  ZipCompression compression) {
    zip_t* archive = archive_.get();
    zip_int32_t method = ZIP_CM_STORE;
    zip_uint32_t flags = 0;
    if (compression == ZipCompression::Deflate) {
        method = ZIP_CM_DEFLATE;
        flags = 6;
    } else if (compression == ZipCompression::DeflateFast) {
        method = ZIP_CM_DEFLATE;
        flags = 1;
    }
    if (zip_set_file_compression(archive, index, method, flags) != 0 ||
        zip_file_set_mtime(archive, index, 0, 0) != 0 ||
        zip_file_set_external_attributes(
            archive, index, 0, ZIP_OPSYS_UNIX,
            static_cast<zip_uint32_t>(0100644u << 16u)) != 0) {
        return std::unexpected(
            Error{"failed to configure ZIP entry: " +
                  std::string(zip_strerror(archive))});
    }
    return {};
}

Result<void>
ZipArchiveWriter::add_file(const std::string& entry_name,
                           const std::filesystem::path& source,
                           ZipCompression compression) {
    auto validated = validate_entry_name(entry_name);
    if (!validated) return std::unexpected(validated.error());
    zip_t* archive = archive_.get();
    zip_source_t* zip_source = zip_source_file(
        archive, source.string().c_str(), 0, ZIP_LENGTH_TO_END);
    if (!zip_source) {
        return std::unexpected(
            Error{"failed to create ZIP source for '" + source.string() +
                  "': " + zip_strerror(archive)});
    }
    const zip_int64_t index = zip_file_add(
        archive, validated->c_str(), zip_source,
        ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
    if (index < 0) {
        zip_source_free(zip_source);
        return std::unexpected(
            Error{"failed to add ZIP entry '" + *validated +
                  "': " + zip_strerror(archive)});
    }
    return configure_entry(static_cast<uint64_t>(index), compression);
}

Result<void>
ZipArchiveWriter::add_bytes(const std::string& entry_name,
                            std::span<const std::byte> bytes,
                            ZipCompression compression) {
    auto validated = validate_entry_name(entry_name);
    if (!validated) return std::unexpected(validated.error());
    auto owned = std::make_shared<std::vector<std::byte>>(
        bytes.begin(), bytes.end());
    zip_t* archive = archive_.get();
    zip_source_t* zip_source = zip_source_buffer(
        archive, owned->data(), owned->size(), 0);
    if (!zip_source) {
        return std::unexpected(
            Error{"failed to create in-memory ZIP source: " +
                  std::string(zip_strerror(archive))});
    }
    const zip_int64_t index = zip_file_add(
        archive, validated->c_str(), zip_source,
        ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
    if (index < 0) {
        zip_source_free(zip_source);
        return std::unexpected(
            Error{"failed to add ZIP entry '" + *validated +
                  "': " + zip_strerror(archive)});
    }
    owned_buffers_.push_back(std::move(owned));
    return configure_entry(static_cast<uint64_t>(index), compression);
}

Result<void> ZipArchiveWriter::finish() {
    if (finished_) return {};
    zip_t* archive = archive_.release();
    if (zip_close(archive) != 0) {
        const std::string detail = zip_strerror(archive);
        zip_discard(archive);
        finished_ = true;
        return std::unexpected(
            Error{"failed to finalize ZIP archive: " + detail});
    }
    finished_ = true;
    owned_buffers_.clear();
    return validate_zip_archive(target_, false);
}

void ZipArchiveWriter::discard() noexcept {
    archive_.reset();
}

Result<void>
validate_zip_archive(const std::filesystem::path& path,
                     bool verify_payload_crc) {
    int error = 0;
    zip_t* archive =
        zip_open(path.string().c_str(), ZIP_RDONLY, &error);
    if (!archive) {
        return std::unexpected(
            Error{"failed to validate ZIP archive: " + open_error(error)});
    }
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    if (count < 0) {
        const std::string detail = zip_strerror(archive);
        zip_close(archive);
        return std::unexpected(
            Error{"failed to enumerate ZIP archive: " + detail});
    }
    if (verify_payload_crc) {
        std::array<std::byte, std::size_t{128} * 1024> buffer{};
        for (zip_uint64_t index = 0;
             std::cmp_less(index, count); ++index) {
            zip_file_t* file = zip_fopen_index(archive, index, 0);
            if (!file) {
                const std::string detail = zip_strerror(archive);
                zip_close(archive);
                return std::unexpected(
                    Error{"failed to validate ZIP entry: " + detail});
            }
            while (true) {
                const zip_int64_t read =
                    zip_fread(file, buffer.data(), buffer.size());
                if (read < 0) {
                    const std::string detail = zip_file_strerror(file);
                    zip_fclose(file);
                    zip_close(archive);
                    return std::unexpected(
                        Error{"ZIP entry CRC/read validation failed: " +
                              detail});
                }
                if (read == 0) break;
            }
            if (zip_fclose(file) != 0) {
                zip_close(archive);
                return std::unexpected(
                    Error{"ZIP entry CRC validation failed"});
            }
        }
    }
    if (zip_close(archive) != 0) {
        return std::unexpected(Error{"failed to close validated ZIP archive"});
    }
    return {};
}

} // namespace heaven
