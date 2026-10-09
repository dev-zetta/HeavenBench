#include "dds_loader_cpu.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

using Bytes = std::vector<std::uint8_t>;

void set_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8u * i));
    }
}

Bytes header(std::uint32_t width, std::uint32_t mips, bool cube) {
    Bytes bytes(128);
    set_u32(bytes, 0, 0x20534444);
    set_u32(bytes, 4, 124);
    set_u32(bytes, 12, width);
    set_u32(bytes, 16, width);
    set_u32(bytes, 28, mips);
    set_u32(bytes, 76, 32);
    set_u32(bytes, 112, cube ? 0xfe00u : 0u);
    return bytes;
}

struct Fixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("heaven-dds-layout-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".dds");
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    heaven::DDSTextureCPU load(std::span<const std::uint8_t> bytes,
                               std::uint32_t starting_mip = 0) const {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) return {};
        return heaven::load_dds_cpu(path.string(), std::nullopt, starting_mip);
    }
};

bool expect(bool condition, const char* message) {
    if (!condition) std::printf("[unit_dds_layout] FAILED: %s\n", message);
    return condition;
}

std::uint8_t pattern(std::uint32_t face, std::uint32_t mip, std::size_t byte) {
    return static_cast<std::uint8_t>(face * 31u + mip * 7u + byte);
}

bool compressed_cube_layout() {
    bool ok = true;
    const Fixture fixture;
    for (bool dx10 : {false, true}) {
        Bytes bytes = header(8, 4, true);
        set_u32(bytes, 80, 4);
        set_u32(bytes, 84, dx10 ? 0x30315844u : 0x31545844u); // DX10 / DXT1
        if (dx10) {
            // DX10 identifies the cube independently of the legacy face flags.
            set_u32(bytes, 112, 0);
            bytes.resize(148);
            set_u32(bytes, 128, 98); // BC7_UNORM
            set_u32(bytes, 132, 3); // TEXTURE2D
            set_u32(bytes, 136, 4); // TEXTURECUBE
            set_u32(bytes, 140, 1); // One cube, six faces.
        }
        const std::array<std::size_t, 4> face_sizes = dx10
            ? std::array<std::size_t, 4>{64, 16, 16, 16}
            : std::array<std::size_t, 4>{32, 8, 8, 8};
        for (std::uint32_t face = 0; face < 6; ++face) {
            for (std::uint32_t mip = 0; mip < 4; ++mip) {
                for (std::size_t i = 0; i < face_sizes[mip]; ++i) {
                    bytes.push_back(pattern(face, mip, i));
                }
            }
        }
        for (std::uint32_t first = 0; first < 4; ++first) {
            const auto dds = fixture.load(bytes, first);
            if (!expect(dds && dds.is_cubemap && dds.width == (8u >> first) &&
                        dds.height == dds.width && dds.mip_count == 4u - first &&
                        dds.mip_data.size() == 4u - first,
                        "cube dimensions, face flag and selected mip count")) {
                ok = false;
                continue;
            }
            for (std::uint32_t mip = first; mip < 4; ++mip) {
                const auto& level = dds.mip_data[mip - first];
                if (!expect(level.size() == face_sizes[mip] * 6, "six faces per mip")) {
                    ok = false;
                    continue;
                }
                for (std::uint32_t face = 0; face < 6; ++face) {
                    for (std::size_t i = 0; i < face_sizes[mip]; ++i) {
                        ok &= expect(level.bytes()[face * face_sizes[mip] + i] ==
                                     pattern(face, mip, i), "face/mip bytes preserve DDS order");
                    }
                }
                ok &= expect(level.owner() == dds.mip_data[0].owner(),
                             "selected mip views retain one shared allocation");
            }
        }
        bytes.pop_back();
        ok &= expect(!fixture.load(bytes, 3), "truncated last cube face rejected with mip skipping");
        if (!dx10) {
            bytes.push_back(0);
            set_u32(bytes, 112, 0x600); // Only +X is present.
            ok &= expect(!fixture.load(bytes), "partial legacy cube rejected");
            set_u32(bytes, 112, 0xfe00);
            set_u32(bytes, 12, 4);
            ok &= expect(!fixture.load(bytes), "non-square cube rejected");
        }
    }
    return ok;
}

Bytes rgb_header(std::uint32_t bits, const std::array<std::uint32_t, 4>& masks,
                 bool alpha) {
    Bytes bytes = header(1, 1, false);
    set_u32(bytes, 80, alpha ? 0x41 : 0x40);
    set_u32(bytes, 88, bits);
    for (std::size_t i = 0; i < masks.size(); ++i) set_u32(bytes, 92 + 4 * i, masks[i]);
    return bytes;
}

bool rgb_masks() {
    bool ok = true;
    const Fixture fixture;
    // All channel permutations, including alpha in the first byte, must map
    // to the same RGBA pixel without overwriting a channel before reading it.
    std::array<std::uint32_t, 4> order{0, 1, 2, 3};
    constexpr std::array<std::uint8_t, 4> rgba{19, 83, 149, 211};
    do {
        std::array<std::uint32_t, 4> masks{};
        std::array<std::uint8_t, 4> stored{};
        for (std::size_t channel = 0; channel < 4; ++channel) {
            masks[channel] = 0xffu << (8u * order[channel]);
            stored[order[channel]] = rgba[channel];
        }
        Bytes bytes = rgb_header(32, masks, true);
        bytes.insert(bytes.end(), stored.begin(), stored.end());
        const auto dds = fixture.load(bytes);
        ok &= expect(dds && dds.format == heaven::DDSFormatCPU::RGBA8 &&
                     std::ranges::equal(dds.mip_data[0].bytes(), rgba),
                     "32-bit channel masks and alpha permutation");
    } while (std::next_permutation(order.begin(), order.end()));
    for (bool bgr : {false, true}) {
        Bytes bytes = rgb_header(24, bgr
            ? std::array<std::uint32_t, 4>{0xff0000, 0xff00, 0xff, 0}
            : std::array<std::uint32_t, 4>{0xff, 0xff00, 0xff0000, 0}, false);
        const std::array<std::uint8_t, 3> stored = bgr
            ? std::array<std::uint8_t, 3>{149, 83, 19}
            : std::array<std::uint8_t, 3>{19, 83, 149};
        bytes.insert(bytes.end(), stored.begin(), stored.end());
        const auto dds = fixture.load(bytes);
        ok &= expect(dds && dds.format == heaven::DDSFormatCPU::RGB8 &&
                     std::ranges::equal(dds.mip_data[0].bytes(), std::span(rgba).first(3)),
                     "24-bit RGB/BGR channels");
    }
    Bytes opaque = rgb_header(32, {0xff0000, 0xff00, 0xff, 0xff000000}, false);
    opaque.insert(opaque.end(), {149, 83, 19, 0});
    const auto xrgb = fixture.load(opaque);
    ok &= expect(xrgb && std::ranges::equal(xrgb.mip_data[0].bytes(),
                         std::array<std::uint8_t, 4>{19, 83, 149, 255}),
                 "undeclared alpha byte is opaque");
    for (const std::array<std::uint32_t, 4> invalid : {
             std::array<std::uint32_t, 4>{0, 0, 0, 0},
             std::array<std::uint32_t, 4>{0xff, 0xff, 0xff0000, 0xff000000},
             std::array<std::uint32_t, 4>{0x3ff, 0xffc00, 0x3ff00000, 0xc0000000},
             std::array<std::uint32_t, 4>{0xff, 0xff00, 0xff0000, 0}}) {
        Bytes bytes = rgb_header(32, invalid, true);
        bytes.insert(bytes.end(), 4, 0);
        ok &= expect(!fixture.load(bytes), "invalid or packed channel masks rejected");
    }
    Bytes rows = rgb_header(24, {0xff, 0xff00, 0xff0000, 0}, false);
    set_u32(rows, 8, 8); // DDSD_PITCH
    set_u32(rows, 12, 2); // Two rows, each containing one RGB pixel.
    set_u32(rows, 20, 3);
    rows.insert(rows.end(), {19, 83, 149, 23, 89, 151});
    const auto packed = fixture.load(rows);
    ok &= expect(packed && std::ranges::equal(packed.mip_data[0].bytes(),
                         std::array<std::uint8_t, 6>{19, 83, 149, 23, 89, 151}),
                 "declared tightly packed row pitch");
    set_u32(rows, 20, 4);
    rows.resize(128);
    rows.insert(rows.end(), {19, 83, 149, 0xee, 23, 89, 151, 0xee});
    ok &= expect(!fixture.load(rows), "padded rows rejected instead of treating padding as pixels");
    return ok;
}

bool uncompressed_cube_layout() {
    Bytes bytes = rgb_header(32, {0xff0000, 0xff00, 0xff, 0xff000000}, true);
    set_u32(bytes, 12, 2);
    set_u32(bytes, 16, 2);
    set_u32(bytes, 28, 2);
    set_u32(bytes, 112, 0xfe00);
    for (std::uint8_t face = 0; face < 6; ++face) {
        for (std::uint8_t mip = 0; mip < 2; ++mip) {
            for (unsigned pixel = 0; pixel < (mip == 0 ? 4u : 1u); ++pixel) {
                bytes.insert(bytes.end(), {static_cast<std::uint8_t>(pixel), mip, face, 200});
            }
        }
    }
    const Fixture fixture;
    bool ok = true;
    for (std::uint32_t first : {0u, 1u}) {
        const auto dds = fixture.load(bytes, first);
        if (!expect(dds && dds.is_cubemap && dds.mip_count == 2u - first,
                    "uncompressed cube loads")) return false;
        for (std::uint32_t mip = first; mip < 2; ++mip) {
            Bytes expected;
            for (std::uint8_t face = 0; face < 6; ++face) {
                for (unsigned pixel = 0; pixel < (mip == 0 ? 4u : 1u); ++pixel) {
                    expected.insert(expected.end(), {face, static_cast<std::uint8_t>(mip),
                                                      static_cast<std::uint8_t>(pixel), 200});
                }
            }
            ok &= expect(std::ranges::equal(dds.mip_data[mip - first].bytes(), expected),
                         "uncompressed face/mip order and BGRA conversion");
        }
    }
    return ok;
}

const heaven::testing::Register reg_cube{compressed_cube_layout};
const heaven::testing::Register reg_masks{rgb_masks};
const heaven::testing::Register reg_rgb_cube{uncompressed_cube_layout};

} // namespace
