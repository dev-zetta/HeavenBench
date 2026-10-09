#include "dds_loader_cpu.h"
#include "engine/dds_block_codec.h"
#include "engine/loader_validation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

using Pixel = std::array<std::uint8_t, 4>;
using Block8 = std::array<std::uint8_t, 8>;
using Block16 = std::array<std::uint8_t, 16>;
using heaven::DDSFormatCPU;
using heaven::decode_dds_blocks_rgba;

constexpr Block8 kRed{0x00, 0xf8, 0, 0, 0, 0, 0, 0};
constexpr Block8 kGreen{0xe0, 0x07, 0, 0, 0, 0, 0, 0};
constexpr Block8 kBlue{0x1f, 0x00, 0, 0, 0, 0, 0, 0};
constexpr Block8 kWhite{0xff, 0xff, 0, 0, 0, 0, 0, 0};

// Selectors 0..7 repeated twice exercise both BC3/4 interpolation tables.
constexpr Block8 kSixAlpha{255, 0, 0x88, 0xc6, 0xfa, 0x88, 0xc6, 0xfa};
constexpr Block8 kFourAlpha{0, 255, 0x88, 0xc6, 0xfa, 0x88, 0xc6, 0xfa};
constexpr std::array<std::uint8_t, 8> kSixValues{255, 0, 218, 182, 145, 109, 72, 36};
constexpr std::array<std::uint8_t, 8> kFourValues{0, 255, 51, 102, 153, 204, 0, 255};

bool pixel_matches(const heaven::DecodedRgbaImage& image, std::size_t index,
                   const Pixel& expected) {
    const std::size_t offset = index * expected.size();
    if (offset + expected.size() > image.pixels.size()) return false;
    for (std::size_t channel = 0; channel < expected.size(); ++channel) {
        if (image.pixels[offset + channel] != expected[channel]) {
            std::fprintf(stderr, "DDS decode pixel %zu channel %zu: got %u, expected %u\n",
                         index, channel, static_cast<unsigned>(image.pixels[offset + channel]),
                         static_cast<unsigned>(expected[channel]));
            return false;
        }
    }
    return true;
}

Block16 joined_blocks(const Block8& first, const Block8& second) {
    Block16 result{};
    std::copy(first.begin(), first.end(), result.begin());
    std::copy(second.begin(), second.end(), result.begin() + 8);
    return result;
}

bool unit_dds_block_colors_and_alpha() {
    // RGB565 red/green endpoints, with all four palette selectors in each row.
    constexpr Block8 palette{0x00, 0xf8, 0xe0, 0x07, 0xe4, 0xe4, 0xe4, 0xe4};
    constexpr std::array<Pixel, 4> colors{{
        {255, 0, 0, 255}, {0, 255, 0, 255}, {170, 85, 0, 255}, {85, 170, 0, 255}}};
    const auto bc1 = decode_dds_blocks_rgba(palette, 4, 4, DDSFormatCPU::BC1);
    if (!bc1 || bc1->width != 4 || bc1->height != 4 || bc1->pixels.size() != 64) return false;
    for (std::size_t index = 0; index < 16; ++index) {
        if (!pixel_matches(*bc1, index, colors[index % colors.size()])) return false;
    }

    constexpr Block8 transparent{0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const auto bc1_alpha = decode_dds_blocks_rgba(transparent, 1, 1, DDSFormatCPU::BC1);
    if (!bc1_alpha || bc1_alpha->pixels.size() != 4 ||
        !pixel_matches(*bc1_alpha, 0, {0, 0, 0, 0})) return false;

    // BC3 always uses four-color interpolation, even with c0 <= c1. Selector
    // three must yield opaque gray before the separate alpha channel is applied.
    const Block16 bc3_source = joined_blocks(kFourAlpha, transparent);
    const auto bc3 = decode_dds_blocks_rgba(bc3_source, 4, 4, DDSFormatCPU::BC3);
    if (!bc3) return false;
    for (std::size_t index = 0; index < 16; ++index) {
        if (!pixel_matches(*bc3, index, {170, 170, 170, kFourValues[index % 8]})) return false;
    }
    return true;
}

bool unit_dds_block_single_and_dual_channels() {
    const auto bc4 = decode_dds_blocks_rgba(kSixAlpha, 4, 4, DDSFormatCPU::BC4);
    const Block16 bc5_source = joined_blocks(kSixAlpha, kFourAlpha);
    const auto bc5 = decode_dds_blocks_rgba(bc5_source, 4, 4, DDSFormatCPU::BC5);
    if (!bc4 || !bc5 || bc4->pixels.size() != 64 || bc5->pixels.size() != 64) return false;
    for (std::size_t index = 0; index < 16; ++index) {
        if (!pixel_matches(*bc4, index, {kSixValues[index % 8], 0, 0, 255}) ||
            !pixel_matches(*bc5, index,
                           {kSixValues[index % 8], kFourValues[index % 8], 0, 255})) return false;
    }
    return true;
}

bool unit_dds_block_edges_and_mips() {
    std::vector<std::uint8_t> quadrants;
    for (const Block8& block : {kRed, kGreen, kBlue, kWhite}) {
        quadrants.insert(quadrants.end(), block.begin(), block.end());
    }
    const auto cropped = decode_dds_blocks_rgba(quadrants, 5, 5, DDSFormatCPU::BC1);
    if (!cropped || cropped->width != 5 || cropped->height != 5 ||
        cropped->pixels.size() != 100) return false;
    for (std::size_t y = 0; y < 5; ++y) {
        for (std::size_t x = 0; x < 5; ++x) {
            const Pixel expected = y < 4
                ? (x < 4 ? Pixel{255, 0, 0, 255} : Pixel{0, 255, 0, 255})
                : (x < 4 ? Pixel{0, 0, 255, 255} : Pixel{255, 255, 255, 255});
            if (!pixel_matches(*cropped, y * 5 + x, expected)) return false;
        }
    }

    // A non-square mip chain keeps one complete compressed block even below 4x4.
    constexpr std::array<std::array<std::uint32_t, 2>, 4> mip_sizes{{
        {9, 5}, {4, 2}, {2, 1}, {1, 1}}};
    for (const auto& dimensions : mip_sizes) {
        const auto [width, height] = dimensions;
        const std::size_t block_count =
            static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4);
        std::vector<std::uint8_t> source;
        for (std::size_t block = 0; block < block_count; ++block) {
            source.insert(source.end(), kBlue.begin(), kBlue.end());
        }
        const auto mip = decode_dds_blocks_rgba(source, width, height, DDSFormatCPU::BC1);
        const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
        if (!mip || mip->width != width || mip->height != height ||
            mip->pixels.size() != pixel_count * 4) return false;
        for (std::size_t index = 0; index < pixel_count; ++index) {
            if (!pixel_matches(*mip, index, {0, 0, 255, 255})) return false;
        }
    }
    return true;
}

bool unit_dds_block_unaligned_and_bc7() {
    // BC7 mode 6: equal seven-bit RGBA endpoints (127,32,16,63), zero
    // endpoint p-bits and zero selectors => constant (254,64,32,126).
    constexpr Block16 bc7_source{
        0xc0, 0xff, 0x1f, 0x04, 0x82, 0x40, 0x7e, 0x3f, 0, 0, 0, 0, 0, 0, 0, 0};
    struct Fixture {
        DDSFormatCPU format;
        Block16 source;
        Pixel first_pixel;
    };
    const std::array fixtures{
        Fixture{DDSFormatCPU::BC3, joined_blocks(kSixAlpha, kRed), {255, 0, 0, 255}},
        Fixture{DDSFormatCPU::BC5, joined_blocks(kSixAlpha, kFourAlpha), {255, 0, 0, 255}},
        Fixture{DDSFormatCPU::BC7, bc7_source, {254, 64, 32, 126}},
    };
    for (const Fixture& fixture : fixtures) {
        alignas(std::uint64_t) std::array<std::uint8_t, 17> unaligned{};
        std::copy(fixture.source.begin(), fixture.source.end(), unaligned.begin() + 1);
        const auto image = decode_dds_blocks_rgba(
            std::span<const std::uint8_t>(unaligned).subspan(1), 1, 1, fixture.format);
        if (!image || image->pixels.size() != 4 ||
            !pixel_matches(*image, 0, fixture.first_pixel)) return false;
    }
    const auto bc7 = decode_dds_blocks_rgba(bc7_source, 4, 4, DDSFormatCPU::BC7);
    if (!bc7 || bc7->pixels.size() != 64) return false;
    for (std::size_t index = 0; index < 16; ++index) {
        if (!pixel_matches(*bc7, index, {254, 64, 32, 126})) return false;
    }
    return true;
}

bool unit_dds_block_invalid_layouts() {
    constexpr std::array<std::uint8_t, 17> payload{};
    const std::span<const std::uint8_t> bytes(payload);
    for (const DDSFormatCPU format : {DDSFormatCPU::BC1, DDSFormatCPU::BC3,
            DDSFormatCPU::BC4, DDSFormatCPU::BC5, DDSFormatCPU::BC7}) {
        const std::size_t block_size =
            format == DDSFormatCPU::BC1 || format == DDSFormatCPU::BC4 ? 8 : 16;
        for (std::size_t size = 0; size < block_size; ++size) {
            if (decode_dds_blocks_rgba(bytes.first(size), 1, 1, format)) return false;
        }
        if (decode_dds_blocks_rgba(bytes.first(block_size + 1), 1, 1, format) ||
            decode_dds_blocks_rgba(bytes.first(block_size), 5, 1, format)) return false;
        for (const auto& dimensions : std::array<std::array<std::uint32_t, 2>, 6>{{
                {0, 1}, {1, 0}, {heaven::loadval::kMaxTexDim + 1, 1},
                {1, heaven::loadval::kMaxTexDim + 1},
                {std::numeric_limits<std::uint32_t>::max(), 1},
                {std::numeric_limits<std::uint32_t>::max(),
                 std::numeric_limits<std::uint32_t>::max()}}}) {
            if (decode_dds_blocks_rgba(bytes.first(block_size),
                    dimensions[0], dimensions[1], format)) return false;
        }
        // Valid dimension limits with a tiny payload must fail before allocation.
        if (decode_dds_blocks_rgba(bytes.first(block_size), heaven::loadval::kMaxTexDim,
                                  heaven::loadval::kMaxTexDim, format)) return false;
    }
    return !decode_dds_blocks_rgba(bytes, 1, 1, DDSFormatCPU::Unknown) &&
           !decode_dds_blocks_rgba(bytes, 1, 1, DDSFormatCPU::RGB8) &&
           !decode_dds_blocks_rgba(bytes, 1, 1, DDSFormatCPU::RGBA8);
}

bool unit_bc4_channel_bounds_and_alignment() {
    const Block16 source = joined_blocks(kSixAlpha, kFourAlpha);
    alignas(std::uint64_t) std::array<std::uint8_t, 17> unaligned{};
    std::copy(source.begin(), source.end(), unaligned.begin() + 1);
    const auto bytes = std::span<const std::uint8_t>(unaligned).subspan(1);
    const auto red = heaven::decode_bc4_channel(bytes, 4, 4, 16, 0);
    const auto green = heaven::decode_bc4_channel(bytes, 4, 4, 16, 8);
    if (!red || !green || red->width != 4 || red->height != 4 ||
        red->pixels.size() != 16 || green->pixels.size() != 16) return false;
    for (std::size_t index = 0; index < 16; ++index) {
        if (red->pixels[index] != kSixValues[index % 8] ||
            green->pixels[index] != kFourValues[index % 8]) return false;
    }
    // Channel decoding accepts a containing BC5 payload, unlike exact-sized
    // whole-image decoding. The final unused channel need not be present.
    const auto first_only = heaven::decode_bc4_channel(bytes.first(8), 1, 1, 16, 0);
    if (!first_only || first_only->pixels != std::vector<std::uint8_t>{255}) return false;
    for (std::size_t size = 0; size < 16; ++size) {
        if (heaven::decode_bc4_channel(bytes.first(size), 1, 1, 16, 8)) return false;
    }
    constexpr std::uint32_t largest = std::numeric_limits<std::uint32_t>::max();
    for (const auto& dimensions : std::array<std::array<std::uint32_t, 2>, 6>{{
            {0, 1}, {1, 0}, {largest, 1}, {1, largest}, {largest, largest},
            {heaven::loadval::kMaxTexDim, heaven::loadval::kMaxTexDim}}}) {
        if (heaven::decode_bc4_channel(bytes, dimensions[0], dimensions[1])) return false;
    }
    return !heaven::decode_bc4_channel(bytes, 1, 1, 7, 0) &&
           !heaven::decode_bc4_channel(bytes, 1, 1, 16, 9) &&
           !heaven::decode_bc4_channel(bytes, 5, 1, largest, largest - 8);
}

const heaven::testing::Register g_dds_block_colors{&unit_dds_block_colors_and_alpha};
const heaven::testing::Register g_dds_block_channels{&unit_dds_block_single_and_dual_channels};
const heaven::testing::Register g_dds_block_edges{&unit_dds_block_edges_and_mips};
const heaven::testing::Register g_dds_block_unaligned{&unit_dds_block_unaligned_and_bc7};
const heaven::testing::Register g_dds_block_invalid{&unit_dds_block_invalid_layouts};
const heaven::testing::Register g_bc4_channel_bounds{&unit_bc4_channel_bounds_and_alignment};

} // namespace
