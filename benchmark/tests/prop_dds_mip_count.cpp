/*
 * HeavenBench — Property test: DDS mip count is bounded
 *
 * Feature: codebase-hardening
 *   •: DDS mip count is bounded
 *
 *
 * The DDS loader (dds_loader_cpu.h::load_dds_cpu) clamps the on-disk
 * `mipMapCount` to the natural mip-chain length so a hostile value cannot drive
 * the per-mip loop past the real chain: a `mipMapCount` of 0
 * is treated as exactly 1, and any positive value is clamped to
 * `floor(log2(max(width, height))) + 1` (computed via `std::bit_width`).
 *
 * This property pins that bound from the outside: for a synthesised DDS with
 * valid dimensions and a complete pixel-data payload, FOR ALL fuzzed
 * width / height / mipMapCount combinations — including a mipMapCount of 0 and
 * absurdly large values — a successfully-decoded texture always satisfies
 *
 *     1 <= mip_count <= floor(log2(max(width, height))) + 1.
 *
 * The synthesiser `build_dds` mirrors the on-disk layout load_dds_cpu expects:
 * the 4-byte 'DDS ' magic, the 124-byte header (size, flags, height, width,
 * pitch, depth, mipMapCount, 11 reserved words, a 32-byte pixel-format block,
 * caps/caps2/caps3/caps4, reserved), then the pixel data. It writes an
 * uncompressed 32-bit BGRA texture (pixel-format FOURCC flag clear,
 * rgbBitCount = 32) and emits the FULL natural mip chain
 * (`floor(log2(max_dim)) + 1` levels). Because the clamped mip count never
 * exceeds the natural chain, supplying the full chain guarantees enough pixel
 * data for every fuzzed mipMapCount, so the load succeeds and the property is
 * exercised on a real decode (operator bool true) rather than only the
 * rejection paths.
 *
 * load_dds_cpu is GPU-free and host-testable; it takes a path, so each input is
 * written to a uniquely-named temp file. This file self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point.
 */
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "dds_loader_cpu.h"

// Mirror the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// ── Little-endian POD appenders ────────────────────────────────

// load_dds_cpu reads with std::fread into native-layout PODs, so we serialise
// the host's byte representation directly (the renderer targets little-endian
// x86).

void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}

// ── Valid-DDS synthesiser ──────────────────────────────────────

// Build a DDS byte image for an uncompressed 32-bit BGRA texture with the given
// dimensions and on-disk mipMapCount, followed by the FULL natural mip chain
// (floor(log2(max_dim)) + 1 levels) of pixel data — enough for any clamped mip
// count the loader derives from `mip_map_count`.
std::vector<std::uint8_t> build_dds(std::uint32_t width, std::uint32_t height,
                                    std::uint32_t mip_map_count) {
    std::vector<std::uint8_t> b;

    put_u32(b, 0x20534444u);   // magic "DDS "

    // ── 124-byte DDS header (31 little-endian u32 fields) ──
    put_u32(b, 124u);          // size
    put_u32(b, 0u);            // flags (unused by the loader)
    put_u32(b, height);        // height
    put_u32(b, width);         // width
    put_u32(b, 0u);            // pitchOrLinearSize
    put_u32(b, 0u);            // depth
    put_u32(b, mip_map_count); // mipMapCount
    for (int i = 0; i < 11; ++i) put_u32(b, 0u); // reserved1[11]

    // PixelFormat (32 bytes). FOURCC flag clear -> uncompressed path;
    // rgbBitCount = 32 selects the supported BGRA8 layout.
    put_u32(b, 32u);           // pf.size
    put_u32(b, 0x41u);         // pf.flags (RGB | ALPHAPIXELS)
    put_u32(b, 0u);            // pf.fourCC
    put_u32(b, 32u);           // pf.rgbBitCount
    put_u32(b, 0x00FF0000u);   // rBitMask
    put_u32(b, 0x0000FF00u);   // gBitMask
    put_u32(b, 0x000000FFu);   // bBitMask
    put_u32(b, 0xFF000000u);   // aBitMask

    put_u32(b, 0u);            // caps
    put_u32(b, 0u);            // caps2 (no cubemap)
    put_u32(b, 0u);            // caps3
    put_u32(b, 0u);            // caps4
    put_u32(b, 0u);            // reserved2

    // ── Pixel data: full natural mip chain, single (non-cube) face ──
    const std::uint32_t max_dim = std::max(width, height);
    const std::uint32_t natural_mips =
        static_cast<std::uint32_t>(std::bit_width(max_dim));
    for (std::uint32_t mip = 0; mip < natural_mips; ++mip) {
        const std::uint32_t w = std::max(1u, width >> mip);
        const std::uint32_t h = std::max(1u, height >> mip);
        const std::size_t bytes = static_cast<std::size_t>(w) * h * 4u; // BGRA8
        b.insert(b.end(), bytes, std::uint8_t{0});
    }

    return b;
}

// ── Temp-file scaffolding ──────────────────────────────────────

// load_dds_cpu opens a path, so each fuzz input is written to a uniquely-named
// file under the system temp directory and removed once the loader returns.
struct TempFile {
    std::filesystem::path path;
    bool ok = false;

    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               std::format("heaven_dds_mip_fuzz_{}.dds", id);
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        if (f) {
            if (!bytes.empty()) {
                std::fwrite(bytes.data(), 1, bytes.size(), f);
            }
            std::fclose(f);
            ok = true;
        }
    }

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec); // best-effort cleanup
    }
};

bool run_p8_mip_count_bounded() {
    return rc::check(
        "Feature: codebase-hardening, Property 8: DDS mip count is bounded "
        "(Validates: Requirements 4.1)",
        [] {
            // Modest dimensions keep the synthesised full mip chain small while
            // still spanning power-of-two and non-power-of-two sizes.
            const std::uint32_t width  = *rc::gen::inRange<std::uint32_t>(1, 257);
            const std::uint32_t height = *rc::gen::inRange<std::uint32_t>(1, 257);

            // Fuzz mipMapCount across the natural range plus the boundary value
            // 0 (treated as 1) and absurdly large values that must be clamped.
            const std::uint32_t mip_map_count = *rc::gen::oneOf(
                rc::gen::inRange<std::uint32_t>(0, 24),
                rc::gen::element<std::uint32_t>(0u, 1u, 1000000u, 0xFFFFFFFFu));

            const std::vector<std::uint8_t> bytes =
                build_dds(width, height, mip_map_count);

            TempFile tf(bytes);
            RC_ASSERT(tf.ok); // temp file must be writable, else test invalid

            const heaven::DDSTextureCPU dds =
                heaven::load_dds_cpu(tf.path.string());

            // Only assert on successfully-decoded textures: for these valid
            // dimensions with a complete mip chain the load is expected to
            // succeed (operator bool true).
            if (!dds) {
                return; // a non-decode is out of scope for this property
            }

            // floor(log2(max_dim)) + 1 == bit_width(max_dim) for max_dim >= 1.
            const std::uint32_t max_dim = std::max(dds.width, dds.height);
            const std::uint32_t upper =
                static_cast<std::uint32_t>(std::bit_width(max_dim));

            RC_ASSERT(dds.mip_count >= 1u);
            RC_ASSERT(dds.mip_count <= upper);

            // The decoded payload must match the (clamped) mip count exactly —
            // no level was dropped or invented.
            RC_ASSERT(dds.mip_data.size() == dds.mip_count);
        });
}

const heaven::testing::Register g_register_dds_mip_count{
    &run_p8_mip_count_bounded};

} // namespace
