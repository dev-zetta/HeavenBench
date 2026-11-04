/*
 * HeavenBench — Shared loader-validation helpers
 *
 * Header-only, GPU-free utilities used by the untrusted binary loaders
 * (mi08 / ms08 / xml0 / DDS / tessellation cache) to treat every on-disk
 * count, length, and offset as hostile. This is the single source of truth
 * for:
 *   - the fewer-than-requested `std::fread` rejection (`read_pod` /
 *     `read_array`),
 *   - the inclusive bounds predicate (`in_range`),
 *   - the buffer-relative block check computed in 64-bit so the sum/product
 *     cannot wrap (`fits`),
 *   - the canonical loader bound constants shared across the loaders.
 *
 * Depends only on the standard library and engine/types.h (for the
 * heaven::Error / heaven::Result aliases). No GPU or Filament dependency, so
 * the helpers are exercised directly on the host with the existing RapidCheck
 * harness.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_LOADER_VALIDATION_H_
#define HEAVENBENCH_SRC_ENGINE_LOADER_VALIDATION_H_

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <limits>
#include <vector>
#include <string>
#include <string_view>
#include <format>
#include <memory>
#include <span>

#include "engine/asset_file_system.h"
#include "engine/types.h"

namespace heaven::loadval {

// Sequential binary reader shared by native and ZIP-backed format loaders.
// Small fields share bounded read-ahead storage; large arrays still read
// directly into typed destination storage. Moving a reader transfers ownership
// of its lazy heap buffer rather than copying it through loader Result values.
class AssetBinaryReader {
public:
    explicit AssetBinaryReader(std::unique_ptr<AssetStream> stream)
        : stream_(std::move(stream)) {}

    [[nodiscard]] static Result<AssetBinaryReader> open(
            std::string_view path, AssetFileSystemRef assets,
            std::string_view asset_kind) {
        auto stream = open_asset_stream(path, assets);
        if (!stream) {
            return std::unexpected(Error(std::format(
                "Cannot open {}: {} ({})", asset_kind, path,
                stream.error().message)));
        }
        return AssetBinaryReader(std::move(*stream));
    }

    [[nodiscard]] bool read_bytes(std::span<std::byte> bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto unread = buffer_size_ - buffer_cursor_;
            if (unread != 0) {
                const auto count = std::min(unread, bytes.size() - offset);
                std::memcpy(bytes.data() + offset, buffer_.get() + buffer_cursor_, count);
                buffer_cursor_ += count;
                offset += count;
                continue;
            }
            buffer_cursor_ = buffer_size_ = 0;
            auto remaining = bytes.subspan(offset);
            if (remaining.size() >= kReadAheadBytes) {
                const auto count = stream_->read(remaining);
                if (!count || *count == 0 || *count > remaining.size()) return false;
                offset += *count;
            } else {
                if (!buffer_) buffer_ = std::make_unique_for_overwrite<std::byte[]>(kReadAheadBytes);
                const auto count = stream_->read({buffer_.get(), kReadAheadBytes});
                // A stream can discover CRC or I/O failure after touching the
                // destination. Publish cached bytes only after a successful
                // read, retaining AssetStream's logical EOF validation.
                if (!count || *count == 0 || *count > kReadAheadBytes) return false;
                buffer_size_ = *count;
            }
        }
        return true;
    }

    template <typename T>
    [[nodiscard]] bool read_object(T& value) {
        return read_bytes(std::as_writable_bytes(std::span(&value, 1)));
    }

    template <typename T, std::size_t Extent>
    [[nodiscard]] bool read_array(std::span<T, Extent> values) {
        return values.empty() ||
               read_bytes(std::as_writable_bytes(values));
    }

    [[nodiscard]] bool seek_relative(int64_t offset) {
        const auto unread = buffer_size_ - buffer_cursor_;
        const auto position = tell();
        uint64_t target = 0;
        if (offset >= 0) {
            const auto distance = static_cast<uint64_t>(offset);
            if (distance <= unread) {
                buffer_cursor_ += static_cast<std::size_t>(distance);
                return true;
            }
            if (distance > std::numeric_limits<uint64_t>::max() - position) return false;
            target = position + distance;
        } else {
            // Negating INT64_MIN or subtracting unread bytes from it would
            // overflow. Convert its magnitude without either signed operation.
            const auto distance = static_cast<uint64_t>(-(offset + 1)) + 1;
            if (distance <= buffer_cursor_) {
                buffer_cursor_ -= static_cast<std::size_t>(distance);
                return true;
            }
            if (distance > position) return false;
            target = position - distance;
        }
        if (target > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
        const auto native_position = stream_->tell();
        const auto result = stream_->seek(static_cast<int64_t>(target), AssetSeekOrigin::Begin);
        // A replay-based failed seek may already have moved its stream. Cached
        // bytes then describe a retired position and must not be reused.
        if (result || stream_->tell() != native_position) buffer_cursor_ = buffer_size_ = 0;
        return result.has_value();
    }

    [[nodiscard]] uint64_t size() const noexcept { return stream_->size(); }
    [[nodiscard]] uint64_t tell() const noexcept {
        return stream_->tell() - (buffer_size_ - buffer_cursor_);
    }

private:
    std::unique_ptr<AssetStream> stream_;
    static constexpr std::size_t kReadAheadBytes = std::size_t{64} * 1024;
    std::unique_ptr<std::byte[]> buffer_;
    std::size_t buffer_cursor_ = 0;
    std::size_t buffer_size_ = 0;
};

// ── Stream readers (FILE*-based: mi08, ms08, dds) ───────────────

// Read exactly one POD value. Returns false if fread returns < 1 element
// Does not close the file — the caller owns the
// RAII guard.
template <typename T>
[[nodiscard]] bool read_pod(std::FILE* f, T& out) {
    return std::fread(&out, sizeof(T), 1, f) == 1;
}

// Read exactly `n` POD elements into a contiguous buffer. Returns false if
// fread returns < n elements. Reading zero elements
// is a no-op that succeeds.
template <typename T>
[[nodiscard]] bool read_array(std::FILE* f, T* dst, std::size_t n) {
    return n == 0 || std::fread(dst, sizeof(T), n, f) == n;
}

template <typename T>
[[nodiscard]] bool read_pod(AssetBinaryReader& reader, T& out) {
    return reader.read_object(out);
}

template <typename T, std::size_t Extent>
[[nodiscard]] bool read_array(
        AssetBinaryReader& reader, std::span<T, Extent> values) {
    return reader.read_array(values);
}

// ── Bounds predicate ────────────────────────────────────────────

// Inclusive range check used for counts/lengths. `lo`/`hi` mirror the
// Ms08_Loader's existing literals (name 1..4096, counts 0..10'000'000,
// surfaces 1..32767, bones 0..4096) so Mi08 adopts identical bounds

template <typename Int>
[[nodiscard]] constexpr bool in_range(Int v, Int lo, Int hi) {
    return v >= lo && v <= hi;
}

// ── Buffer-relative block check (xml0, BC decoders, tess cache) ──

// True iff [offset, offset + need) lies fully inside a buffer of `size`
// bytes, computed in 64-bit so the product/sum cannot wrap (Requirements

[[nodiscard]] constexpr bool fits(std::uint64_t offset, std::uint64_t need,
                                  std::uint64_t size) {
    return offset <= size && need <= size - offset;
}

// ── Canonical loader bounds ─────────────────────────────────────

// Named once so every loader shares them.
inline constexpr std::int32_t  kMaxNameLen   = 4096;       // >=1.. 4096
inline constexpr std::int32_t  kMaxElemCount = 10'000'000; // >=0.. 10M
inline constexpr std::int32_t  kMaxSurfaces  = 32767;      // >=1.. 32767
inline constexpr std::int32_t  kMaxBones     = 4096;       // >=0.. 4096
inline constexpr std::uint32_t kMaxTexDim    = 16384;      // DDS dimension cap
inline constexpr int           kXmlMaxDepth  = 256;        // xml0 recursion cap

} // namespace heaven::loadval

#endif // HEAVENBENCH_SRC_ENGINE_LOADER_VALIDATION_H_
