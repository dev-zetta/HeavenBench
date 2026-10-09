/*
 * HeavenBench — unit test: DDS uncompressed pixel-format validation
 *
 * Feature: codebase-hardening
 * — Requirements: 4.5
 *
 * Design reference: A4 (dds_loader_cpu.h). The uncompressed (non-FOURCC) path
 * of load_dds_cpu only supports 24-bit BGR and 32-bit BGRA layouts. Any other
 * rgbBitCount must be rejected before allocation, returning a falsy
 * DDSTextureCPU (the loader's existing failure convention — callers test
 * `if (!dds)`).
 *
 * This file pins that contract deterministically:
 *
 *   • An uncompressed DDS whose rgbBitCount is neither 24 nor 32 (8 or 16) is
 *     rejected — load_dds_cpu returns a falsy DDSTextureCPU.
 *
 *   • A valid 24-bit uncompressed DDS and a valid 32-bit uncompressed DDS each
 *     load successfully with the expected format and dimensions — this anchors
 *     the accepted path so the rejection above cannot be satisfied by simply
 *     breaking all uncompressed loads.
 *
 * load_dds_cpu takes a file path, so each synthesised DDS byte image is written
 * to a uniquely-named temp file and removed once the loader returns. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point. This is
 * a deterministic unit test (no random input), so it does not use rc::check.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "dds_loader_cpu.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// ── Little-endian byte emitters ────────────────────────────────

// DDS is a little-endian on-disk format; emit each 32-bit field byte-by-byte so
// the synthesised image is endianness-independent of the host.
void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

// ── Synthesise a single-mip uncompressed DDS byte image ────────

// Builds the 4-byte magic + 124-byte header for an uncompressed (non-FOURCC)
// 2D texture with the given dimensions and rgbBitCount, then appends pixel data
// when `append_pixels` is set. The pixel-data block is sized exactly
// width*height*(rgb_bits/8) so the loader's fits() check passes for valid
// layouts; for unsupported bit counts the loader rejects before any read, so
// callers leave `append_pixels` false.
std::vector<std::uint8_t> build_uncompressed_dds(std::uint32_t width,
                                                 std::uint32_t height,
                                                 std::uint32_t rgb_bits,
                                                 bool append_pixels) {
    constexpr std::uint32_t kDdsMagic = 0x20534444; // "DDS "
    std::vector<std::uint8_t> b;

    put_u32(b, kDdsMagic);

    // Header (124 bytes).
    put_u32(b, 124);     // size
    put_u32(b, 0);       // flags
    put_u32(b, height);  // height
    put_u32(b, width);   // width
    put_u32(b, 0);       // pitchOrLinearSize
    put_u32(b, 0);       // depth
    put_u32(b, 1);       // mipMapCount (single mip)
    for (int i = 0; i < 11; i++) put_u32(b, 0); // reserved1[11]

    // PixelFormat (32 bytes). flags has no DDPF_FOURCC bit, so the loader takes
    // the uncompressed path and inspects rgbBitCount.
    put_u32(b, 32);        // pixelFormat.size
    put_u32(b, rgb_bits == 32 ? 0x41 : 0x40); // RGB, optional alpha
    put_u32(b, 0);         // fourCC
    put_u32(b, rgb_bits);  // rgbBitCount under test
    put_u32(b, 0x00ff0000); // rBitMask
    put_u32(b, 0x0000ff00); // gBitMask
    put_u32(b, 0x000000ff); // bBitMask
    put_u32(b, rgb_bits == 32 ? 0xff000000 : 0); // aBitMask

    put_u32(b, 0);  // caps
    put_u32(b, 0);  // caps2 (no DDSCAPS2_CUBEMAP → single face)
    put_u32(b, 0);  // caps3
    put_u32(b, 0);  // caps4
    put_u32(b, 0);  // reserved2

    if (append_pixels) {
        const std::size_t bpp = rgb_bits / 8;
        const std::size_t pixel_bytes =
            static_cast<std::size_t>(width) * height * bpp;
        b.insert(b.end(), pixel_bytes, std::uint8_t{0});
    }

    return b;
}

// ── Temp-file scaffolding ──────────────────────────────────────

// load_dds_cpu opens a path, so each crafted image is written to a
// uniquely-named file under the system temp directory and removed once the
// loader returns.
struct TempFile {
    std::filesystem::path path;
    bool ok = false;

    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               std::format("heaven_dds_format_{}.dds", id);
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        if (f) {
            if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            ok = true;
        }
    }

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec); // best-effort cleanup
    }
};

bool unit_dds_format() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_dds_format] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    auto load_bytes = [](const std::vector<std::uint8_t>& bytes) {
        TempFile tf(bytes);
        if (!tf.ok) return heaven::DDSTextureCPU{};
        return heaven::load_dds_cpu(tf.path.string());
    };

    // ── 1. Unsupported rgbBitCount is rejected  ──
    // 8-bit and 16-bit uncompressed layouts are neither 24 nor 32 and must be
    // rejected before any pixel-data allocation, yielding a falsy texture.
    {
        auto dds8 = load_bytes(build_uncompressed_dds(4, 4, 8, false));
        if (static_cast<bool>(dds8))
            fail("uncompressed 8-bit DDS must be rejected (falsy result)");

        auto dds16 = load_bytes(build_uncompressed_dds(4, 4, 16, false));
        if (static_cast<bool>(dds16))
            fail("uncompressed 16-bit DDS must be rejected (falsy result)");
    }

    // ── 2. Valid 24-bit uncompressed DDS loads (anchors accepted path) ──
    {
        auto dds24 = load_bytes(build_uncompressed_dds(4, 4, 24, true));
        if (!static_cast<bool>(dds24))
            fail("valid 24-bit uncompressed DDS should load");
        else {
            if (dds24.format != heaven::DDSFormatCPU::RGB8)
                fail("24-bit DDS should decode as RGB8");
            if (dds24.width != 4 || dds24.height != 4)
                fail("24-bit DDS dimensions should be 4x4");
            if (dds24.mip_data.size() != 1)
                fail("24-bit DDS should have exactly one mip level");
        }
    }

    // ── 3. Valid 32-bit uncompressed DDS loads (anchors accepted path) ──
    {
        auto dds32 = load_bytes(build_uncompressed_dds(4, 4, 32, true));
        if (!static_cast<bool>(dds32))
            fail("valid 32-bit uncompressed DDS should load");
        else {
            if (dds32.format != heaven::DDSFormatCPU::RGBA8)
                fail("32-bit DDS should decode as RGBA8");
            if (dds32.width != 4 || dds32.height != 4)
                fail("32-bit DDS dimensions should be 4x4");
            if (dds32.mip_data.size() != 1)
                fail("32-bit DDS should have exactly one mip level");
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_dds_format{&unit_dds_format};

} // namespace
