/*
 * HeavenBench — small RAII write façade over pinned libzip
 */
#ifndef HEAVENBENCH_SRC_ENGINE_ZIP_ARCHIVE_H_
#define HEAVENBENCH_SRC_ENGINE_ZIP_ARCHIVE_H_

#include "engine/types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct zip;

namespace heaven {

using zip_t = ::zip;

enum class ZipCompression {
    Store,
    Deflate,
    DeflateFast,
};

// Transactional deterministic ZIP writer backed by libzip. It owns the open
// archive, fixes entry metadata, and publishes only after a successful close.
class ZipArchiveWriter {
public:
    ZipArchiveWriter() = default;
    ZipArchiveWriter(const ZipArchiveWriter&) = delete;
    ZipArchiveWriter& operator=(const ZipArchiveWriter&) = delete;
    ZipArchiveWriter(ZipArchiveWriter&& other) noexcept;
    ZipArchiveWriter& operator=(ZipArchiveWriter&& other) noexcept;
    ~ZipArchiveWriter();

    [[nodiscard]] static Result<ZipArchiveWriter>
    create(std::filesystem::path target, bool overwrite);

    [[nodiscard]] Result<void>
    add_file(const std::string& entry_name,
             const std::filesystem::path& source,
             ZipCompression compression);

    [[nodiscard]] Result<void>
    add_bytes(const std::string& entry_name,
              std::span<const std::byte> bytes,
              ZipCompression compression);

    [[nodiscard]] Result<void> finish();
    [[nodiscard]] const std::filesystem::path& target() const noexcept {
        return target_;
    }

private:
    struct ArchiveDiscard {
        void operator()(zip_t* archive) const noexcept;
    };
    using ArchiveOwner = std::unique_ptr<zip_t, ArchiveDiscard>;

    explicit ZipArchiveWriter(std::filesystem::path target,
                              ArchiveOwner archive)
        : target_(std::move(target)), archive_(std::move(archive)) {}

    [[nodiscard]] Result<void>
    configure_entry(uint64_t index, ZipCompression compression);
    void discard() noexcept;

    std::filesystem::path target_;
    ArchiveOwner archive_;
    bool finished_ = false;
    std::vector<std::shared_ptr<std::vector<std::byte>>> owned_buffers_;
};

[[nodiscard]] Result<void>
validate_zip_archive(const std::filesystem::path& path,
                     bool verify_payload_crc);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_ZIP_ARCHIVE_H_
