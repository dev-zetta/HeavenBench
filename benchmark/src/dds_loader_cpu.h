/*
 * DDS Texture Loader — CPU-side (no OpenGL dependency)
 *
 * Loads DDS files into CPU memory buffers suitable for uploading to any
 * graphics API (Filament, OpenGL, Vulkan, etc.).
 */
#ifndef HEAVENBENCH_SRC_DDS_LOADER_CPU_H_
#define HEAVENBENCH_SRC_DDS_LOADER_CPU_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/asset_file_system.h"
#include "engine/loader_validation.h"

namespace heaven {

// ── DDS format enum (API-agnostic) ──────────────────────────────

enum class DDSFormatCPU {
    Unknown,
    BC1,    // DXT1
    BC3,    // DXT5
    BC4,    // ATI1 / RGTC1
    BC5,    // ATI2 / RGTC2
    BC7,    // DXGI_FORMAT_BC7_UNORM(_SRGB), DX10 header
    RGBA8,  // Uncompressed
    RGB8,   // Uncompressed (no alpha)
};

// ── CPU-side DDS texture data ───────────────────────────────────

// DDSMipLevelCPU is an immutable view into shared DDS storage. Copies retain
// the backing allocation, allowing asynchronous GPU uploads without copying
// compressed mip payloads into a second buffer.
class DDSMipLevelCPU {
public:
    DDSMipLevelCPU() = default;
    DDSMipLevelCPU(SharedAssetBuffer storage, std::size_t offset,
                   std::size_t size)
        : storage_(std::move(storage)), offset_(offset), size_(size) {}

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        if (!storage_ || offset_ > storage_->size() ||
            size_ > storage_->size() - offset_) {
            return {};
        }
        return std::span<const std::uint8_t>(*storage_).subspan(offset_, size_);
    }
    [[nodiscard]] const std::uint8_t* data() const noexcept {
        return bytes().data();
    }
    [[nodiscard]] std::size_t size() const noexcept { return bytes().size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] const SharedAssetBuffer& owner() const noexcept {
        return storage_;
    }
    [[nodiscard]] operator std::span<const std::uint8_t>() const noexcept {
        return bytes();
    }

    friend bool operator==(const DDSMipLevelCPU& lhs,
                           const DDSMipLevelCPU& rhs) noexcept {
        const auto left = lhs.bytes();
        const auto right = rhs.bytes();
        return left.size() == right.size() &&
               std::equal(left.begin(), left.end(), right.begin());
    }

private:
    SharedAssetBuffer storage_;
    std::size_t offset_ = 0;
    std::size_t size_ = 0;
};

struct DDSTextureCPU {
    uint32_t    width       = 0;
    uint32_t    height      = 0;
    DDSFormatCPU format     = DDSFormatCPU::Unknown;
    bool        is_cubemap  = false;
    uint32_t    mip_count   = 0;

    // Per-mip-level data (for cubemaps, all 6 faces are concatenated per level)
    std::vector<DDSMipLevelCPU> mip_data;

    explicit operator bool() const { return width > 0 && height > 0 && !mip_data.empty(); }
};

// ── DDS file constants ──────────────────────────────────────────

namespace dds_detail {

constexpr uint32_t DDS_MAGIC = 0x20534444;  // "DDS "
constexpr uint32_t DDSD_PITCH = 0x8;
constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
constexpr uint32_t DDPF_FOURCC = 0x4;
constexpr uint32_t DDPF_RGB = 0x40;
constexpr uint32_t DDSCAPS2_CUBEMAP = 0x200;
constexpr uint32_t DDSCAPS2_CUBEMAP_ALLFACES = 0xfc00;
constexpr uint32_t DDS_RESOURCE_MISC_TEXTURECUBE = 0x4;

#pragma pack(push, 1)
struct PixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask, gBitMask, bBitMask, aBitMask;
};

struct Header {
    uint32_t    size;
    uint32_t    flags;
    uint32_t    height;
    uint32_t    width;
    uint32_t    pitchOrLinearSize;
    uint32_t    depth;
    uint32_t    mipMapCount;
    uint32_t    reserved1[11];
    PixelFormat pixelFormat;
    uint32_t    caps;
    uint32_t    caps2;
    uint32_t    caps3;
    uint32_t    caps4;
    uint32_t    reserved2;
};

struct HeaderDX10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
#pragma pack(pop)

[[nodiscard]] constexpr std::uint32_t
make_fourcc(char a, char b, char c, char d) noexcept {
    return static_cast<std::uint32_t>(a) |
           (static_cast<std::uint32_t>(b) << 8u) |
           (static_cast<std::uint32_t>(c) << 16u) |
           (static_cast<std::uint32_t>(d) << 24u);
}

inline uint32_t block_size(DDSFormatCPU fmt) {
    switch (fmt) {
        case DDSFormatCPU::BC1:
        case DDSFormatCPU::BC4:
            return 8;
        case DDSFormatCPU::BC3:
        case DDSFormatCPU::BC5:
        case DDSFormatCPU::BC7:
            return 16;
        default:
            return 0;
    }
}

inline uint64_t mip_size_compressed(uint32_t w, uint32_t h, DDSFormatCPU fmt) {
    uint64_t bw = (w + 3) / 4;
    uint64_t bh = (h + 3) / 4;
    return bw * bh * block_size(fmt);
}

} // namespace dds_detail

// ── Load function ───────────────────────────────────────────────

inline DDSTextureCPU load_dds_cpu(
        const std::string& path,
        AssetFileSystemRef assets = std::nullopt,
        uint32_t starting_mip = 0) {
    using heaven::loadval::fits;
    using heaven::loadval::kMaxTexDim;
    using namespace dds_detail;

    DDSTextureCPU result;
    auto stream_result = open_asset_stream(path, assets);
    if (!stream_result) return result;
    std::unique_ptr<AssetStream> stream = std::move(*stream_result);
    const uint64_t file_size = stream->size();

    const auto read_exact = [&](void* destination, std::size_t count) -> bool {
        auto out = std::span<std::byte>(static_cast<std::byte*>(destination), count);
        std::size_t offset = 0;
        while (offset < count) {
            auto read = stream->read(out.subspan(offset));
            if (!read || *read == 0) return false;
            offset += *read;
        }
        return true;
    };

    constexpr uint64_t legacy_data_start =
        sizeof(uint32_t) + sizeof(Header);
    if (!fits(0, legacy_data_start, file_size)) return result;

    uint32_t magic = 0;
    Header hdr{};
    if (!read_exact(&magic, sizeof(magic)) || magic != DDS_MAGIC ||
        !read_exact(&hdr, sizeof(hdr))) {
        return result;
    }
    if (hdr.size != sizeof(Header) || hdr.pixelFormat.size != sizeof(PixelFormat) ||
        hdr.width == 0 || hdr.height == 0 ||
        hdr.width > kMaxTexDim || hdr.height > kMaxTexDim) {
        return result;
    }

    const uint32_t source_width = hdr.width;
    const uint32_t source_height = hdr.height;
    const uint32_t natural_mips =
        static_cast<uint32_t>(std::bit_width(std::max(source_width, source_height)));
    const uint32_t source_mips = hdr.mipMapCount == 0
        ? 1u : std::min(hdr.mipMapCount, natural_mips);
    if (starting_mip >= source_mips) return result;

    result.is_cubemap = (hdr.caps2 & DDSCAPS2_CUBEMAP) != 0;
    const bool compressed = (hdr.pixelFormat.flags & DDPF_FOURCC) != 0;
    bool has_dx10_header = false;
    // Indices into a stored pixel; index 4 denotes an absent alpha channel.
    std::array<uint32_t, 4> channel_bytes{0, 1, 2, 4};
    uint64_t data_start = legacy_data_start;

    if (compressed) {
        const uint32_t fourcc = hdr.pixelFormat.fourCC;
        if (fourcc == make_fourcc('D','X','T','1')) {
            result.format = DDSFormatCPU::BC1;
        } else if (fourcc == make_fourcc('D','X','T','5')) {
            result.format = DDSFormatCPU::BC3;
        } else if (fourcc == make_fourcc('A','T','I','1') ||
                   fourcc == make_fourcc('B','C','4','U')) {
            result.format = DDSFormatCPU::BC4;
        } else if (fourcc == make_fourcc('A','T','I','2') ||
                   fourcc == make_fourcc('B','C','5','U')) {
            result.format = DDSFormatCPU::BC5;
        } else if (fourcc == make_fourcc('D','X','1','0')) {
            HeaderDX10 dx10{};
            if (!fits(data_start, sizeof(dx10), file_size) ||
                !read_exact(&dx10, sizeof(dx10)) || dx10.arraySize != 1 ||
                dx10.resourceDimension != 3u) {
                return DDSTextureCPU{};
            }
            data_start += sizeof(dx10);
            has_dx10_header = true;
            result.is_cubemap = (dx10.miscFlag & DDS_RESOURCE_MISC_TEXTURECUBE) != 0;
            // DXGI_FORMAT_BC7_UNORM / DXGI_FORMAT_BC7_UNORM_SRGB.
            if (dx10.dxgiFormat != 98u && dx10.dxgiFormat != 99u) {
                return DDSTextureCPU{};
            }
            result.format = DDSFormatCPU::BC7;
        } else {
            return DDSTextureCPU{};
        }
    } else {
        if ((hdr.pixelFormat.flags & DDPF_RGB) == 0 ||
            (hdr.pixelFormat.rgbBitCount != 24 &&
             hdr.pixelFormat.rgbBitCount != 32)) {
            return DDSTextureCPU{};
        }
        result.format = hdr.pixelFormat.rgbBitCount == 32
            ? DDSFormatCPU::RGBA8 : DDSFormatCPU::RGB8;
        // Only tightly packed RGB rows are supported. An explicit padded
        // pitch must not be mistaken for pixel bytes or shift later mips.
        if ((hdr.flags & DDSD_PITCH) != 0 &&
            hdr.pitchOrLinearSize != source_width * (hdr.pixelFormat.rgbBitCount / 8u)) {
            return DDSTextureCPU{};
        }
        const std::array masks{hdr.pixelFormat.rBitMask, hdr.pixelFormat.gBitMask,
                               hdr.pixelFormat.bBitMask, hdr.pixelFormat.aBitMask};
        const uint32_t channels = (hdr.pixelFormat.flags & DDPF_ALPHAPIXELS) ? 4u : 3u;
        uint32_t used_masks = 0;
        for (uint32_t channel = 0; channel < channels; ++channel) {
            bool found = false;
            for (uint32_t byte = 0; byte < hdr.pixelFormat.rgbBitCount / 8u; ++byte) {
                if (masks[channel] == (0xffu << (byte * 8u))) {
                    channel_bytes[channel] = byte;
                    found = true;
                    break;
                }
            }
            // Packed/overlapping/absent masks cannot be represented by RGB8/RGBA8.
            if (!found || (used_masks & masks[channel]) != 0) return DDSTextureCPU{};
            used_masks |= masks[channel];
        }
    }

    if (result.is_cubemap &&
        (source_width != source_height ||
         (!has_dx10_header && (hdr.caps2 & DDSCAPS2_CUBEMAP_ALLFACES) !=
                                  DDSCAPS2_CUBEMAP_ALLFACES))) {
        return DDSTextureCPU{};
    }
    const uint32_t face_count = result.is_cubemap ? 6u : 1u;
    const uint32_t bytes_per_pixel = hdr.pixelFormat.rgbBitCount / 8u;
    const auto mip_face_size = [&](uint32_t mip) -> uint64_t {
        const uint32_t w = std::max(1u, source_width >> mip);
        const uint32_t h = std::max(1u, source_height >> mip);
        if (compressed) {
            return mip_size_compressed(w, h, result.format);
        }
        return static_cast<uint64_t>(w) * h * bytes_per_pixel;
    };

    // DDS stores each face's complete mip chain before the next face. The
    // upload contract instead groups all six faces of a mip into one view.
    // Dimensions and mip count are bounded above, so these 64-bit sums/products
    // cannot overflow. Validate every face before allocating selected storage.
    uint64_t face_chain_size = 0;
    uint64_t skipped_face_bytes = 0;
    for (uint32_t mip = 0; mip < source_mips; ++mip) {
        const uint64_t required_bytes = mip_face_size(mip);
        face_chain_size += required_bytes;
        if (mip < starting_mip) skipped_face_bytes += required_bytes;
    }
    if (!fits(data_start, face_chain_size * face_count, file_size)) {
        return DDSTextureCPU{};
    }
    const uint64_t selected_size = (face_chain_size - skipped_face_bytes) * face_count;
    if (selected_size > static_cast<uint64_t>(SIZE_MAX)) return DDSTextureCPU{};

    auto selected = std::make_shared<AssetBuffer>(
        static_cast<std::size_t>(selected_size));
    for (uint32_t face = 0; face < face_count; ++face) {
        const uint64_t source_offset = data_start + face * face_chain_size + skipped_face_bytes;
        if (!stream->seek(static_cast<int64_t>(source_offset), AssetSeekOrigin::Begin)) {
            return DDSTextureCPU{};
        }
        if (!result.is_cubemap) {
            // Keep the common 2D path as one contiguous selected-mip read.
            if (!read_exact(selected->data(), selected->size())) return DDSTextureCPU{};
            break;
        }
        std::size_t output_offset = 0;
        for (uint32_t mip = starting_mip; mip < source_mips; ++mip) {
            const auto face_bytes = static_cast<std::size_t>(mip_face_size(mip));
            if (!read_exact(selected->data() + output_offset + face * face_bytes, face_bytes)) {
                return DDSTextureCPU{};
            }
            output_offset += face_bytes * face_count;
        }
    }
    if (!compressed) {
        for (std::size_t i = 0; i < selected->size(); i += bytes_per_pixel) {
            const std::array rgb{(*selected)[i + channel_bytes[0]],
                                 (*selected)[i + channel_bytes[1]],
                                 (*selected)[i + channel_bytes[2]]};
            const uint8_t alpha = channel_bytes[3] == 4 ? uint8_t{255}
                : (*selected)[i + channel_bytes[3]];
            std::copy(rgb.begin(), rgb.end(), selected->begin() + static_cast<std::ptrdiff_t>(i));
            if (bytes_per_pixel == 4) (*selected)[i + 3] = alpha;
        }
    }

    result.width = std::max(1u, source_width >> starting_mip);
    result.height = std::max(1u, source_height >> starting_mip);
    result.mip_count = source_mips - starting_mip;
    std::size_t selected_offset = 0;
    for (uint32_t mip = starting_mip; mip < source_mips; ++mip) {
        const std::size_t size = static_cast<std::size_t>(mip_face_size(mip) * face_count);
        result.mip_data.emplace_back(selected, selected_offset, size);
        selected_offset += size;
    }
    return result;
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_DDS_LOADER_CPU_H_
