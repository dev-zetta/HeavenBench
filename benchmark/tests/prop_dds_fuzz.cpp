/*
 * HeavenBench — Property test: DDS loader OOB safety
 *
 * Feature: codebase-hardening
 *   •: DDS loader out-of-bounds safety
 *
 *
 * The DDS loader (dds_loader_cpu.h::load_dds_cpu) parses an untrusted binary
 * texture file: every on-disk dimension, mip count, pixel-format field, and
 * implied pixel-data size is hostile until validated. This property pins the
 * loader's out-of-bounds-safety contract: FOR ALL byte
 * sequences supplied as a DDS file, load_dds_cpu either returns a valid decoded
 * texture or a falsy DDSTextureCPU — never reading outside the file buffer and
 * never terminating the process.
 *
 * Unlike the mesh loaders, load_dds_cpu does not return heaven::Result; its
 * failure convention is a falsy DDSTextureCPU (operator bool == false), which
 * callers test with `if (!dds)`. The property therefore asserts decoded-or-falsy
 * rather than Ok-or-Error.
 *
 * The property feeds six families of hostile input, written to a real temp file
 * on disk (load_dds_cpu takes a path) and passed through the public entry point:
 *
 *   • Random bytes — an arbitrary buffer. Almost always rejected (bad magic /
 *     short header), exercising the early-out paths.
 *   • Truncated valid — a synthesised, fully-valid DDS cut off at a random
 *     length. Drives the magic / header short-read guards and, crucially, the
 *     per-mip `fits(offset, need, file_size)` bound and per-mip short-read
 *     rejection: a file truncated mid-pixel-data must be
 *     rejected without over-reading the buffer.
 *   • Mutated valid — a synthesised valid DDS with a handful of random byte
 *     overwrites. Perturbs dimensions, mip count, fourCC, masks, and payload,
 *     hitting the dimension / format / size-math gates.
 *   • Zero-dimension — a valid header with width or height forced to 0. Must be
 *     rejected before any allocation.
 *   • Oversized — a valid header with width or height beyond kMaxTexDim. Must be
 *     rejected before any allocation.
 *   • Bad uncompressed format — an uncompressed header whose rgbBitCount is not
 *     24 or 32. Must be rejected.
 *
 * The synthesiser `build_valid_dds` mirrors the on-disk layout load_dds_cpu
 * expects: the 4-byte 'DDS ' magic, a 124-byte Header (size/flags/height/width/
 * pitch/depth/mipMapCount/reserved1[11]/PixelFormat/caps/caps2/caps3/caps4/
 * reserved2), then the full mip chain of zero-filled pixel data sized exactly as
 * the loader computes it (compressed block math for BC1/BC3, w*h*bpp for
 * uncompressed). A freshly-built buffer loads cleanly, so any failure originates
 * purely from the fuzzing mutation/truncation/forcing.
 *
 * The assertion is intentionally minimal: control returns from load_dds_cpu in a
 * defined state for every input (the no-crash / no-OOB guarantee, enforced under
 * AddressSanitizer). On a successful decode we additionally assert the loader's
 * structural guarantees — non-zero dimensions within kMaxTexDim, at least one
 * mip, and a non-empty buffer for every mip level. Mip-count bounds are
 * 's concern (prop_dds_mip_count.cpp) and size arithmetic is
 * 's (prop_dds_size_math.cpp), so neither is the focus here.
 *
 * load_dds_cpu is GPU-free and host-testable. This file self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register pattern,
 * so the file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the
 * entry point. RapidCheck's default of 100 successful iterations meets the
 * "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "dds_loader_cpu.h"
#include "engine/loader_validation.h"

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

// The DDS magic and the FOURCC/flag constants, matching dds_detail.
constexpr std::uint32_t kDdsMagic     = 0x20534444; // "DDS "
constexpr std::uint32_t kDdpfFourcc   = 0x4;
constexpr std::uint32_t kCubemapCaps2 = 0x200;

constexpr std::uint32_t make_fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

// The four texture layouts the synthesiser can emit.
enum class Layout { Bc1, Bc3, Rgb24, Rgba32 };

bool is_compressed(Layout l) { return l == Layout::Bc1 || l == Layout::Bc3; }

std::uint32_t block_bytes(Layout l) { return l == Layout::Bc1 ? 8u : 16u; }

std::uint32_t bytes_per_pixel(Layout l) { return l == Layout::Rgb24 ? 3u : 4u; }

// Size of one face of a single mip level, matching the loader's arithmetic.
std::uint64_t mip_face_size(std::uint32_t w, std::uint32_t h, Layout l) {
    if (is_compressed(l)) {
        std::uint64_t bw = (w + 3) / 4;
        std::uint64_t bh = (h + 3) / 4;
        return bw * bh * block_bytes(l);
    }
    return static_cast<std::uint64_t>(w) * h * bytes_per_pixel(l);
}

// ── Valid-file synthesiser ─────────────────────────────────────

// Build a fully-valid DDS byte image of the given dimensions, mip count, layout,
// and cubemap flag. The pixel data is zero-filled but sized exactly as
// the DDS format specifies it (a full mip chain for each face),
// so a freshly-built buffer decodes cleanly.
std::vector<std::uint8_t> build_valid_dds(std::uint32_t width,
                                          std::uint32_t height,
                                          std::uint32_t mip_count,
                                          Layout layout,
                                          bool cubemap) {
    if (cubemap) height = width;
    std::vector<std::uint8_t> b;

    put_u32(b, kDdsMagic);

    // ── 124-byte Header ──
    put_u32(b, 124);            // size
    put_u32(b, 0);              // flags
    put_u32(b, height);         // height
    put_u32(b, width);          // width
    put_u32(b, 0);              // pitchOrLinearSize
    put_u32(b, 0);              // depth
    put_u32(b, mip_count);      // mipMapCount
    for (int i = 0; i < 11; ++i) put_u32(b, 0); // reserved1[11]

    // PixelFormat (32 bytes).
    put_u32(b, 32);             // size
    if (is_compressed(layout)) {
        put_u32(b, kDdpfFourcc); // flags
        put_u32(b, layout == Layout::Bc1 ? make_fourcc('D', 'X', 'T', '1')
                                         : make_fourcc('D', 'X', 'T', '5')); // fourCC
        put_u32(b, 0);          // rgbBitCount
        put_u32(b, 0);          // rBitMask
        put_u32(b, 0);          // gBitMask
        put_u32(b, 0);          // bBitMask
        put_u32(b, 0);          // aBitMask
    } else {
        put_u32(b, layout == Layout::Rgb24 ? 0x40u : 0x41u); // RGB, optional alpha
        put_u32(b, 0);                                          // fourCC
        put_u32(b, layout == Layout::Rgb24 ? 24u : 32u);        // rgbBitCount
        put_u32(b, 0x00ff0000);                                 // rBitMask
        put_u32(b, 0x0000ff00);                                 // gBitMask
        put_u32(b, 0x000000ff);                                 // bBitMask
        put_u32(b, layout == Layout::Rgba32 ? 0xff000000u : 0); // aBitMask
    }

    put_u32(b, 0x1000);                       // caps (TEXTURE)
    put_u32(b, cubemap ? kCubemapCaps2 | 0xfc00u : 0u); // caps2: all six faces
    put_u32(b, 0);                            // caps3
    put_u32(b, 0);                            // caps4
    put_u32(b, 0);                            // reserved2

    // ── Pixel data: each face's full mip chain ──
    const std::uint32_t faces = cubemap ? 6u : 1u;
    for (std::uint32_t face = 0; face < faces; ++face) {
        for (std::uint32_t mip = 0; mip < mip_count; ++mip) {
            std::uint32_t w = std::max(1u, width >> mip);
            std::uint32_t h = std::max(1u, height >> mip);
            std::uint64_t total = mip_face_size(w, h, layout);
            b.insert(b.end(), static_cast<std::size_t>(total), std::uint8_t{0});
        }
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
               std::format("heaven_dds_fuzz_{}.bin", id);
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

// Run load_dds_cpu over a fuzz buffer and assert the OOB-safety contract:
// control returns in a defined decoded-or-falsy state (the no-crash / no-OOB
// guarantee under ASan). On a successful decode, the loader's structural
// guarantees hold.
void check_loads_safely(const std::vector<std::uint8_t>& bytes) {
    TempFile tf(bytes);
    RC_ASSERT(tf.ok); // temp file must be writable, else the test is invalid

    const heaven::DDSTextureCPU dds = heaven::load_dds_cpu(tf.path.string());

    // Either outcome is acceptable — the property is that we got here at all,
    // without an out-of-bounds access or process termination.
    if (static_cast<bool>(dds)) {
        // A decoded texture has non-zero dimensions within the dimension cap
        // at least one mip, and a non-empty buffer for every
        // mip level (no partially-read / discarded mip survived —).
        RC_ASSERT(dds.width > 0);
        RC_ASSERT(dds.height > 0);
        RC_ASSERT(dds.width <= heaven::loadval::kMaxTexDim);
        RC_ASSERT(dds.height <= heaven::loadval::kMaxTexDim);
        RC_ASSERT(dds.mip_count >= 1);
        RC_ASSERT(!dds.mip_data.empty());
        RC_ASSERT(dds.mip_data.size() == dds.mip_count);
        for (const auto& level : dds.mip_data) {
            RC_ASSERT(!level.empty());
        }
    }
}

// Pick one of the four valid layouts from a RapidCheck-generated selector.
Layout gen_layout() {
    const int which = *rc::gen::inRange(0, 4);
    switch (which) {
        case 0: return Layout::Bc1;
        case 1: return Layout::Bc3;
        case 2: return Layout::Rgb24;
        default: return Layout::Rgba32;
    }
}

bool run_p7_random_bytes() {
    return rc::check(
        "Feature: codebase-hardening, Property 7: DDS loader out-of-bounds "
        "safety — arbitrary bytes (Validates: Requirements 4.2, 4.4, 4.5, 4.6, "
        "4.7, 22.1)",
        [] {
            const auto bytes =
                *rc::gen::container<std::vector<std::uint8_t>>(
                    rc::gen::arbitrary<std::uint8_t>());
            check_loads_safely(bytes);
        });
}

bool run_p7_truncated_valid() {
    return rc::check(
        "Feature: codebase-hardening, Property 7: DDS loader out-of-bounds "
        "safety — truncated valid file (Validates: Requirements 4.4, 4.6, 4.7, "
        "22.1)",
        [] {
            const auto width = *rc::gen::inRange<std::uint32_t>(1, 65);
            const auto height = *rc::gen::inRange<std::uint32_t>(1, 65);
            const auto mips = *rc::gen::inRange<std::uint32_t>(1, 4);
            const Layout layout = gen_layout();
            const bool cubemap = *rc::gen::arbitrary<bool>();
            const std::vector<std::uint8_t> full =
                build_valid_dds(width, height, mips, layout, cubemap);

            // Cut the valid image at a random length: [0, full.size()].
            const std::size_t cut =
                *rc::gen::inRange<std::size_t>(0, full.size() + 1);
            const std::vector<std::uint8_t> truncated(full.begin(),
                                                      full.begin() + static_cast<std::ptrdiff_t>(cut));
            check_loads_safely(truncated);
        });
}

bool run_p7_mutated_valid() {
    return rc::check(
        "Feature: codebase-hardening, Property 7: DDS loader out-of-bounds "
        "safety — mutated valid file (Validates: Requirements 4.2, 4.5, 4.7, "
        "22.1)",
        [] {
            const auto width = *rc::gen::inRange<std::uint32_t>(1, 65);
            const auto height = *rc::gen::inRange<std::uint32_t>(1, 65);
            const auto mips = *rc::gen::inRange<std::uint32_t>(1, 4);
            const Layout layout = gen_layout();
            const bool cubemap = *rc::gen::arbitrary<bool>();
            std::vector<std::uint8_t> bytes =
                build_valid_dds(width, height, mips, layout, cubemap);

            // Overwrite a handful of random byte positions with random values.
            // This perturbs dimensions, mip count, fourCC, masks, and payload
            // alike, hitting the dimension / format / size-math gates.
            const int mutations = *rc::gen::inRange(1, 8);
            for (int i = 0; i < mutations && !bytes.empty(); ++i) {
                const std::size_t pos =
                    *rc::gen::inRange<std::size_t>(0, bytes.size());
                bytes[pos] = *rc::gen::arbitrary<std::uint8_t>();
            }
            check_loads_safely(bytes);
        });
}

bool run_p7_zero_dimension() {
    return rc::check(
        "Feature: codebase-hardening, Property 7: DDS loader out-of-bounds "
        "safety — zero-dimension header (Validates: Requirements 4.2, 4.7)",
        [] {
            // A header with width and/or height forced to 0 must be rejected
            // before any pixel-data allocation. Build a small valid image then
            // overwrite the dimension field(s) in place.
            const Layout layout = gen_layout();
            std::vector<std::uint8_t> bytes =
                build_valid_dds(8, 8, 1, layout, false);

            // Header layout: magic(4) + size(4) + flags(4) + height(4) + width(4).
            constexpr std::size_t kHeightOff = 4 + 4 + 4;       // = 12
            constexpr std::size_t kWidthOff = kHeightOff + 4;   // = 16
            const int mode = *rc::gen::inRange(0, 3);
            if (mode == 0 || mode == 2) {
                for (std::size_t i = 0; i < 4; ++i) bytes[kWidthOff + i] = 0;
            }
            if (mode == 1 || mode == 2) {
                for (std::size_t i = 0; i < 4; ++i) bytes[kHeightOff + i] = 0;
            }
            check_loads_safely(bytes);

            // The loader must treat a zero-dimension header as a hard reject.
            TempFile tf(bytes);
            RC_ASSERT(tf.ok);
            const heaven::DDSTextureCPU dds =
                heaven::load_dds_cpu(tf.path.string());
            RC_ASSERT(!static_cast<bool>(dds));
        });
}

bool run_p7_oversized_dimension() {
    return rc::check(
        "Feature: codebase-hardening, Property 7: DDS loader out-of-bounds "
        "safety — oversized header (Validates: Requirements 4.2, 4.7)",
        [] {
            // A header with a dimension beyond kMaxTexDim must be rejected
            // before allocation. We only overwrite the header field — no
            // gigantic pixel buffer is synthesised.
            const Layout layout = gen_layout();
            std::vector<std::uint8_t> bytes =
                build_valid_dds(8, 8, 1, layout, false);

            const std::uint32_t big =
                *rc::gen::inRange<std::uint32_t>(heaven::loadval::kMaxTexDim + 1,
                                                 heaven::loadval::kMaxTexDim * 4);
            constexpr std::size_t kHeightOff = 12;
            constexpr std::size_t kWidthOff = 16;
            const bool hit_width = *rc::gen::arbitrary<bool>();
            const std::size_t off = hit_width ? kWidthOff : kHeightOff;
            std::memcpy(bytes.data() + off, &big, sizeof(big));

            TempFile tf(bytes);
            RC_ASSERT(tf.ok);
            const heaven::DDSTextureCPU dds =
                heaven::load_dds_cpu(tf.path.string());
            RC_ASSERT(!static_cast<bool>(dds));
        });
}

bool run_p7_bad_uncompressed_format() {
    return rc::check(
        "Feature: codebase-hardening, Property 7: DDS loader out-of-bounds "
        "safety — unsupported uncompressed rgbBitCount (Validates: "
        "Requirements 4.5, 4.7)",
        [] {
            // An uncompressed header whose rgbBitCount is neither 24 nor 32 must
            // be rejected. Build a valid uncompressed image then overwrite the
            // rgbBitCount field with a forbidden value.
            std::vector<std::uint8_t> bytes =
                build_valid_dds(8, 8, 1, Layout::Rgba32, false);

            // rgbBitCount sits inside PixelFormat. Header offset:
            // magic(4) + [size,flags,height,width,pitch,depth,mipMapCount](7*4)
            // + reserved1[11](44) + pixelFormat.size(4) + pixelFormat.flags(4)
            // + pixelFormat.fourCC(4) = 4 + 28 + 44 + 12 = 88.
            constexpr std::size_t kRgbBitCountOff = 4 + 7 * 4 + 44 + 4 + 4 + 4;

            // Any value other than 24 or 32 (8, 16, 48, 64, …) must be rejected.
            std::uint32_t bad =
                *rc::gen::inRange<std::uint32_t>(0, 128);
            if (bad == 24 || bad == 32) bad = 16; // force an unsupported value
            std::memcpy(bytes.data() + kRgbBitCountOff, &bad, sizeof(bad));

            TempFile tf(bytes);
            RC_ASSERT(tf.ok);
            const heaven::DDSTextureCPU dds =
                heaven::load_dds_cpu(tf.path.string());
            RC_ASSERT(!static_cast<bool>(dds));
        });
}

bool run_dds_fuzz_properties() {
    bool ok = true;
    ok = run_p7_random_bytes() && ok;
    ok = run_p7_truncated_valid() && ok;
    ok = run_p7_mutated_valid() && ok;
    ok = run_p7_zero_dimension() && ok;
    ok = run_p7_oversized_dimension() && ok;
    ok = run_p7_bad_uncompressed_format() && ok;
    return ok;
}

const heaven::testing::Register g_register_dds_fuzz{&run_dds_fuzz_properties};

} // namespace
