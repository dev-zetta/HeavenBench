/*
 * HeavenBench — shared bounded decoding for DDS block-compressed channels
 */
#include "engine/dds_block_codec.h"

#include "engine/loader_validation.h"
#include "dds_loader_cpu.h"

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

#include <array>
#include <algorithm>
#include <limits>

namespace heaven {

Result<DecodedRgbaImage>
decode_dds_blocks_rgba(std::span<const uint8_t> source,
                       uint32_t width,
                       uint32_t height,
                       DDSFormatCPU format) {
    const uint32_t block_bytes = dds_detail::block_size(format);
    if (block_bytes == 0 || width == 0 || height == 0 ||
        width > loadval::kMaxTexDim || height > loadval::kMaxTexDim) {
        return std::unexpected(Error{"invalid BC image format or dimensions"});
    }
    const uint64_t blocks_wide = (uint64_t{width} + 3) / 4;
    const uint64_t blocks_high = (uint64_t{height} + 3) / 4;
    const uint64_t required_bytes = blocks_wide * blocks_high * block_bytes;
    const uint64_t decoded_bytes = uint64_t{width} * height * 4;
    // Validate the complete compressed payload before allocating the expanded
    // image. The dimension limit also bounds both products on 64-bit hosts.
    if (required_bytes != source.size() ||
        decoded_bytes > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(Error{"invalid BC image payload size"});
    }

    DecodedRgbaImage result{width, height,
        std::vector<uint8_t>(static_cast<std::size_t>(decoded_bytes))};
    for (uint64_t block_y = 0; block_y < blocks_high; ++block_y) {
        for (uint64_t block_x = 0; block_x < blocks_wide; ++block_x) {
            const auto offset = static_cast<std::size_t>(
                (block_y * blocks_wide + block_x) * block_bytes);
            // bcdec uses integer loads/stores internally. DDS payloads may be
            // unaligned views, so align both sides of that C API boundary.
            alignas(uint64_t) std::array<uint8_t, 16> block{};
            alignas(uint64_t) std::array<uint8_t, 64> decoded{};
            std::ranges::copy(source.subspan(offset, block_bytes), block.begin());
            uint32_t channels = 4;
            switch (format) {
                case DDSFormatCPU::BC1:
                    bcdec_bc1(block.data(), decoded.data(), 16);
                    break;
                case DDSFormatCPU::BC3:
                    bcdec_bc3(block.data(), decoded.data(), 16);
                    break;
                case DDSFormatCPU::BC4:
                    bcdec_bc4(block.data(), decoded.data(), 4);
                    channels = 1;
                    break;
                case DDSFormatCPU::BC5:
                    bcdec_bc5(block.data(), decoded.data(), 8);
                    channels = 2;
                    break;
                case DDSFormatCPU::BC7:
                    bcdec_bc7(block.data(), decoded.data(), 16);
                    break;
                default:
                    return std::unexpected(Error{"unsupported BC image format"});
            }
            for (uint64_t y = 0; y < 4 && block_y * 4 + y < height; ++y) {
                for (uint64_t x = 0; x < 4 && block_x * 4 + x < width; ++x) {
                    const auto destination = static_cast<std::size_t>(
                        ((block_y * 4 + y) * width + block_x * 4 + x) * 4);
                    const auto pixel = static_cast<std::size_t>((y * 4 + x) * channels);
                    result.pixels[destination] = decoded[pixel];
                    result.pixels[destination + 1] = channels >= 2 ? decoded[pixel + 1] : 0;
                    result.pixels[destination + 2] = channels == 4 ? decoded[pixel + 2] : 0;
                    result.pixels[destination + 3] = channels == 4 ? decoded[pixel + 3] : 255;
                }
            }
        }
    }
    return result;
}

Result<DecodedChannel>
decode_bc4_channel(std::span<const uint8_t> source,
                   uint32_t width,
                   uint32_t height,
                   uint32_t block_stride,
                   uint32_t channel_offset) {
    if (width == 0 || height == 0 ||
        width > loadval::kMaxTexDim || height > loadval::kMaxTexDim ||
        block_stride < 8 ||
        channel_offset > block_stride - 8) {
        return std::unexpected(Error{"invalid BC4 channel layout"});
    }
    const uint64_t pixel_count = static_cast<uint64_t>(width) * height;
    const uint64_t blocks_wide = (uint64_t{width} + 3) / 4;
    const uint64_t blocks_high = (uint64_t{height} + 3) / 4;
    const uint64_t required_bytes =
        (blocks_wide * blocks_high - 1) * block_stride + channel_offset + 8;
    if (required_bytes > source.size() ||
        pixel_count > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(Error{"invalid BC4 channel payload size"});
    }

    DecodedChannel result{width, height,
                          std::vector<uint8_t>(static_cast<std::size_t>(pixel_count))};
    for (uint32_t block_y = 0; block_y < blocks_high; ++block_y) {
        for (uint32_t block_x = 0; block_x < blocks_wide; ++block_x) {
            const uint64_t block_index =
                static_cast<uint64_t>(block_y) * blocks_wide + block_x;
            const uint64_t offset =
                block_index * block_stride + channel_offset;
            if (!loadval::fits(offset, 8, source.size())) {
                return std::unexpected(Error{"truncated BC4 channel data"});
            }
            std::array<uint8_t, 16> decoded{};
            const auto block = source.subspan(
                static_cast<std::size_t>(offset), BCDEC_BC4_BLOCK_SIZE);
            // bcdec is a third-party C API and is the only raw-buffer boundary
            // in the decoder.
            alignas(uint64_t) std::array<uint8_t, BCDEC_BC4_BLOCK_SIZE> aligned{};
            std::ranges::copy(block, aligned.begin());
            bcdec_bc4(aligned.data(), decoded.data(), 4);
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t image_x = block_x * 4 + x;
                    const uint32_t image_y = block_y * 4 + y;
                    if (image_x < width && image_y < height) {
                        result.pixels[static_cast<std::size_t>(image_y) * width +
                                      image_x] = decoded[y * 4 + x];
                    }
                }
            }
        }
    }
    return result;
}

} // namespace heaven
