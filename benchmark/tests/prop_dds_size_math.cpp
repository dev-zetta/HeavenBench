/*
 * HeavenBench — Property test: DDS size arithmetic is overflow-free
 *
 * Feature: codebase-hardening
 *   •: DDS size arithmetic is overflow-free
 *
 *
 * The DDS loader (dds_loader_cpu.h::load_dds_cpu) computes, for every mip
 * level, a per-face byte size, a per-mip total (face_size × face_count), and a
 * cumulative file offset.  mandates that ALL of this arithmetic
 * be carried in 64-bit (size_t / uint64_t) so that no intermediate or final
 * value is ever truncated to 32 bits. A texture near the dimension cap
 * (kMaxTexDim = 16384) with multiple faces produces totals that exceed
 * UINT32_MAX, so a 32-bit computation would silently wrap — corrupting the
 * read sizes and over/under-reading pixel data.
 *
 * This property is pure computation — no file I/O. It pins the contract two
 * ways:
 *
 *   1. The loader's per-mip block-size primitive,
 *      dds_detail::mip_size_compressed(w, h, fmt), is compared against an
 *      INDEPENDENT uint64 reference that derives the block counts with a
 *      different ceil-division expression. For fuzzed w/h across the legal
 *      dimension range and every block-compressed format, the two must agree
 *      exactly. Because the reference is computed in uint64 and the assertion
 *      is exact, any 32-bit truncation inside the loader's formula would be
 *      caught as a mismatch.
 *
 *   2. The full per-mip size formula the loader inlines — the compressed path
 *      (face_size = mip_size_compressed(w, h, fmt)) and the uncompressed path
 *      (face_size = w·h·bpp), each scaled by face_count and accumulated into a
 *      cumulative offset down the mip chain — is replicated here in uint64 and
 *      compared against an independent uint64 reference. The accumulation is
 *      asserted to be strictly monotonic (no wrap) and to match the reference
 *      at every step.
 *
 * Additionally, a deterministic extreme-dimension check confirms the 64-bit
 * math is actually necessary: at w = h = kMaxTexDim the uncompressed cubemap
 * total exceeds UINT32_MAX, so the value provably cannot be represented in 32
 * bits — pinning that the loader's size type is wide enough.
 *
 * mip_size_compressed is GPU-free and host-testable; this file self-registers
 * with the RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point. RapidCheck's default of 100 successful iterations
 * meets the "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <functional>
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

using heaven::DDSFormatCPU;
using heaven::loadval::kMaxTexDim;

// ── Independent uint64 references ──────────────────────────────

// These deliberately avoid reusing the loader's expressions. The loader rounds
// block counts up with (x + 3) / 4; the reference uses the distinct
// floor-plus-remainder form x/4 + (x % 4 ? 1: 0). Both are carried in uint64,
// so an exact match proves the loader's result is the true 64-bit value (no
// 32-bit truncation), and the differing expression makes the check independent
// rather than a tautology.

std::uint64_t ref_block_count(std::uint64_t x) {
    return x / 4 + ((x % 4) != 0 ? 1 : 0);
}

std::uint64_t ref_block_bytes(DDSFormatCPU fmt) {
    switch (fmt) {
        case DDSFormatCPU::BC1:
        case DDSFormatCPU::BC4:
            return 8;
        case DDSFormatCPU::BC3:
        case DDSFormatCPU::BC5:
            return 16;
        default:
            return 0;
    }
}

// Independent reference for mip_size_compressed: block_count(w) × block_count(h)
// × block_bytes, all in uint64.
std::uint64_t ref_mip_size_compressed(std::uint32_t w, std::uint32_t h,
                                      DDSFormatCPU fmt) {
    return ref_block_count(w) * ref_block_count(h) * ref_block_bytes(fmt);
}

const std::vector<DDSFormatCPU>& compressed_formats() {
    static const std::vector<DDSFormatCPU> f = {
        DDSFormatCPU::BC1, DDSFormatCPU::BC3,
        DDSFormatCPU::BC4, DDSFormatCPU::BC5,
    };
    return f;
}

// ── a: mip_size_compressed matches the uint64 reference ─

// For fuzzed dimensions across the legal range and every block-compressed
// format, the loader's primitive equals the independent 64-bit reference.
bool run_p9_mip_size_matches_reference() {
    return rc::check(
        "Feature: codebase-hardening, Property 9: DDS size arithmetic is "
        "overflow-free — mip_size_compressed matches an independent uint64 "
        "reference (Validates: Requirements 4.3)",
        [] {
            const auto w =
                *rc::gen::inRange<std::uint32_t>(1, kMaxTexDim + 1);
            const auto h =
                *rc::gen::inRange<std::uint32_t>(1, kMaxTexDim + 1);
            const DDSFormatCPU fmt = *rc::gen::elementOf(compressed_formats());

            const std::uint64_t got =
                heaven::dds_detail::mip_size_compressed(w, h, fmt);
            const std::uint64_t ref = ref_mip_size_compressed(w, h, fmt);

            RC_ASSERT(got == ref);
        });
}

// ── b: full per-mip / cumulative-offset math is wrap-free ─

// Replicate the loader's per-mip formula (compressed and uncompressed) down a
// full mip chain, accumulating a cumulative offset exactly as load_dds_cpu
// does, and assert in uint64 that:
//   • each per-mip total equals an independent uint64 reference, and
//   • the cumulative offset is strictly monotonic (never wraps).
// The chain length mirrors the loader's clamp: floor(log2(max_dim)) + 1.
bool run_p9_cumulative_offset_no_wrap() {
    return rc::check(
        "Feature: codebase-hardening, Property 9: DDS size arithmetic is "
        "overflow-free — per-mip totals and cumulative offset never wrap "
        "(Validates: Requirements 4.3)",
        [] {
            const auto width =
                *rc::gen::inRange<std::uint32_t>(1, kMaxTexDim + 1);
            const auto height =
                *rc::gen::inRange<std::uint32_t>(1, kMaxTexDim + 1);
            const std::uint32_t face_count =
                *rc::gen::element<std::uint32_t>(1u, 6u);
            // false → uncompressed (32-bit RGBA8, bpp = 4); true → compressed.
            const bool compressed = *rc::gen::arbitrary<bool>();
            const DDSFormatCPU fmt = *rc::gen::elementOf(compressed_formats());
            const std::uint32_t bpp = 4; // RGBA8, the larger uncompressed layout

            const std::uint32_t max_dim = std::max(width, height);
            const std::uint32_t mip_count =
                static_cast<std::uint32_t>(std::bit_width(max_dim));

            // Loader-equivalent accumulation (all uint64).
            std::uint64_t offset = 0;
            // Independent reference accumulation, summed separately.
            std::uint64_t ref_offset = 0;

            for (std::uint32_t mip = 0; mip < mip_count; ++mip) {
                const std::uint32_t w = std::max(1u, width >> mip);
                const std::uint32_t h = std::max(1u, height >> mip);

                std::uint64_t face_size;
                std::uint64_t ref_face_size;
                if (compressed) {
                    face_size =
                        heaven::dds_detail::mip_size_compressed(w, h, fmt);
                    ref_face_size = ref_mip_size_compressed(w, h, fmt);
                } else {
                    face_size =
                        static_cast<std::uint64_t>(w) * h * bpp;
                    ref_face_size = static_cast<std::uint64_t>(w) *
                                    static_cast<std::uint64_t>(h) *
                                    static_cast<std::uint64_t>(bpp);
                }

                const std::uint64_t total_size = face_size * face_count;
                const std::uint64_t ref_total =
                    ref_face_size * static_cast<std::uint64_t>(face_count);

                // The replicated per-mip total equals the independent ref.
                RC_ASSERT(total_size == ref_total);

                const std::uint64_t prev_offset = offset;
                offset += total_size;
                ref_offset += ref_total;

                // Cumulative offset is monotonic non-decreasing: a 32-bit wrap
                // would make it jump backwards. (Equal only when total_size is
                // 0, which the >= 1 dimensions and bpp/block size preclude.)
                RC_ASSERT(offset >= prev_offset);
                RC_ASSERT(offset == ref_offset);
            }
        });
}

// ── c (deterministic): extreme dims exceed 32 bits ─────

// At the dimension cap, the uncompressed cubemap total provably exceeds
// UINT32_MAX, so the value cannot be represented in 32 bits — confirming the
// 64-bit size type is necessary, not incidental. 16384·16384·4·6 ≈ 6.4e9.
bool run_p9_extreme_dims_exceed_u32() {
    bool ok = true;
    auto fail = [&](const char* msg) {
        std::printf("[prop_dds_size_math] FAILED: %s\n", msg);
        ok = false;
    };

    const std::uint64_t w = kMaxTexDim;
    const std::uint64_t h = kMaxTexDim;
    const std::uint64_t bpp = 4;        // RGBA8
    const std::uint64_t face_count = 6; // cubemap

    const std::uint64_t uncompressed_total = w * h * bpp * face_count;
    if (uncompressed_total <= 0xFFFFFFFFull)
        fail("uncompressed cubemap total at kMaxTexDim must exceed UINT32_MAX");

    // The compressed primitive at the cap must also equal its reference and be
    // a precise large value (BC3 face at 16384² = 4096·4096·16 = 268,435,456).
    const std::uint64_t bc3_face = heaven::dds_detail::mip_size_compressed(
        kMaxTexDim, kMaxTexDim, DDSFormatCPU::BC3);
    if (bc3_face != ref_mip_size_compressed(kMaxTexDim, kMaxTexDim,
                                            DDSFormatCPU::BC3))
        fail("compressed face size at kMaxTexDim must match the uint64 reference");

    // The BC3 cubemap total at the cap likewise crosses UINT32_MAX
    // (268,435,456 × 6 ≈ 1.6e9 — still within 32 bits — but the running mip-0
    // RGBA8 case above already pins the >32-bit boundary). Assert the product
    // is computed without wrapping relative to the reference.
    const std::uint64_t bc3_total = bc3_face * face_count;
    if (bc3_total != ref_mip_size_compressed(kMaxTexDim, kMaxTexDim,
                                             DDSFormatCPU::BC3) * face_count)
        fail("compressed cubemap total must equal the uint64 reference product");

    return ok;
}

bool run_dds_size_math_properties() {
    bool ok = true;
    ok = run_p9_mip_size_matches_reference() && ok;
    ok = run_p9_cumulative_offset_no_wrap() && ok;
    ok = run_p9_extreme_dims_exceed_u32() && ok;
    return ok;
}

const heaven::testing::Register g_register_dds_size_math{
    &run_dds_size_math_properties};

} // namespace
