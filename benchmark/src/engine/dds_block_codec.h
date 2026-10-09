/*
 * HeavenBench — shared bounded decoding for DDS block-compressed channels
 */
#ifndef HEAVENBENCH_SRC_ENGINE_DDS_BLOCK_CODEC_H_
#define HEAVENBENCH_SRC_ENGINE_DDS_BLOCK_CODEC_H_

#include "engine/types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace heaven {

enum class DDSFormatCPU;

// DecodedChannel stores one tightly packed 8-bit channel in row-major order.
struct DecodedChannel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

struct DecodedRgbaImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

// Decode exactly one tightly packed mip of unsigned BC1/3/4/5/7 blocks. Edge
// blocks are cropped to the requested dimensions. BC4 and BC5 preserve texture
// sampling defaults: missing color channels are zero and alpha is opaque.
// Color-space conversion belongs to the GPU texture format, not this decoder.
[[nodiscard]] Result<DecodedRgbaImage>
decode_dds_blocks_rgba(std::span<const uint8_t> source,
                       uint32_t width,
                       uint32_t height,
                       DDSFormatCPU format);

// Decode one BC4 channel from standalone BC4 blocks or an interleaved BC5
// channel. `block_stride` and `channel_offset` describe the source layout.
[[nodiscard]] Result<DecodedChannel>
decode_bc4_channel(std::span<const uint8_t> source,
                   uint32_t width,
                   uint32_t height,
                   uint32_t block_stride = 8,
                   uint32_t channel_offset = 0);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_DDS_BLOCK_CODEC_H_
