/*
 * HeavenBench — Property test: mi08 static-mesh loader OOB safety
 *
 * Feature: codebase-hardening
 *   •: mi08 loader out-of-bounds safety
 *
 *
 * The mi08 loader (engine/mesh.cpp::load_mi08) parses an untrusted binary asset
 * file: every on-disk count, length, and index is hostile until validated. This
 * property pins the loader's out-of-bounds-safety contract
 * FOR ALL byte sequences supplied as a mi08 file, load_mi08 either
 * returns a valid Mesh or returns an Error — never reading or writing outside an
 * allocated buffer and never terminating the process.
 *
 * The property feeds three families of hostile input, written to a real temp
 * file on disk (load_mi08 takes a path) and passed through the public entry
 * point:
 *
 *   • Random bytes — an arbitrary buffer of arbitrary length. Almost always
 *     rejected (bad magic / short read), exercising the early-out paths.
 *   • Truncated valid — a synthesised, fully-valid mi08 cut off at a random
 *     length. This drives every `read_pod`/`read_array` short-read rejection
 *      at every field boundary: magic, bounding volume,
 *     surface count, name length, name body, vertex/UV/triangle counts and
 *     their payloads.
 *   • Mutated valid — a synthesised valid mi08 with a handful of random byte
 *     overwrites. These hit the count/length range checks (Requirements
 *  an overwritten name length, surface count, or element count is
 *     either rejected with an Error or, if still in range, drives a (bounded)
 *     allocation that the truncation/short-read guards keep safe.
 *
 * The synthesiser `build_valid_mi08` mirrors the on-disk layout load_mi08
 * expects: magic 'mi08', a 10-float header bounding volume, an int32 surface
 * count, then per-surface { int32 name length, name bytes, 40-byte surface
 * bounding volume } headers, then per-surface { int32 num_verts, vertex
 * (3×f32 pos + 3×u16 normal) array, int32 num_tc0, uv (2×f32) array, int32
 * num_tc1 (0), int32 num_tris, triangle (3×u16) index array } data blocks. A
 * freshly-built buffer loads cleanly, so any failure originates purely from the
 * fuzzing mutation/truncation.
 *
 * The assertion is intentionally minimal: the property is that control returns
 * from load_mi08 in a defined state for every input (the no-crash / no-OOB
 * guarantee, enforced when the suite is compiled under AddressSanitizer). On a
 * successful load we additionally assert the structural invariant the loader
 * guarantees — at least one surface (surface count is validated to be ≥ 1).
 * Loaded-index range is 's concern (prop_mi08_indices.cpp), so it is
 * deliberately not asserted here.
 *
 * load_mi08 is GPU-free and host-testable. This file self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point. RapidCheck's default of 100 successful iterations
 * meets the "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
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

#include "engine/mesh.h"

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

// load_mi08 reads with std::fread into native-layout PODs, so we serialise the
// host's byte representation directly (the renderer targets little-endian x86).

void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}

void put_i32(std::vector<std::uint8_t>& b, std::int32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}

void put_f32(std::vector<std::uint8_t>& b, float v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}

void put_u16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}

// The mi08 magic, matching load_mi08's `'m' | ('i' << 8) | ('0' << 16) | ('8' << 24)`.
constexpr std::uint32_t kMi08Magic =
    'm' | ('i' << 8) | ('0' << 16) | ('8' << 24);

// ── Valid-file synthesiser ─────────────────────────────────────

// Build a fully-valid mi08 byte image with `num_surfaces` surfaces, each with
// `verts_per_surf` vertices and `tris_per_surf` triangles. Triangle indices are
// kept strictly inside [0, verts_per_surf) so a freshly-built buffer loads
// without warnings. verts_per_surf is kept < 65536 so the loader takes its
// 16-bit-index branch.
std::vector<std::uint8_t> build_valid_mi08(int num_surfaces,
                                           int verts_per_surf,
                                           int tris_per_surf) {
    std::vector<std::uint8_t> b;

    put_u32(b, kMi08Magic);

    // Header bounding volume: 10 floats (AABB min/max/center + radius).
    for (int i = 0; i < 10; ++i) put_f32(b, static_cast<float>(i) * 0.5f);

    put_i32(b, num_surfaces);

    // Per-surface headers: name length (incl. terminator), name bytes, then a
    // 40-byte surface bounding volume the loader skips with fseek.
    for (int s = 0; s < num_surfaces; ++s) {
        const std::string name = std::format("surf_{}", s);
        const std::int32_t name_len = static_cast<std::int32_t>(name.size()) + 1;
        put_i32(b, name_len);
        b.insert(b.end(), name.begin(), name.end());
        b.push_back('\0');
        for (int i = 0; i < 10; ++i) put_f32(b, 0.0f); // 40-byte surface BV
    }

    // Per-surface data blocks.
    for (int s = 0; s < num_surfaces; ++s) {
        // Vertices: 3×f32 position + 3×u16 compressed normal.
        put_i32(b, verts_per_surf);
        for (int v = 0; v < verts_per_surf; ++v) {
            put_f32(b, static_cast<float>(v));
            put_f32(b, static_cast<float>(v) + 1.0f);
            put_f32(b, static_cast<float>(v) + 2.0f);
            put_u16(b, 32768);
            put_u16(b, 32768);
            put_u16(b, 65535);
        }

        // UV set 0: one (u, v) per vertex.
        put_i32(b, verts_per_surf);
        for (int v = 0; v < verts_per_surf; ++v) {
            put_f32(b, 0.25f);
            put_f32(b, 0.75f);
        }

        // UV set 1: none.
        put_i32(b, 0);

        // Triangle indices: 3×u16 per triangle, every index in range.
        put_i32(b, tris_per_surf);
        for (int t = 0; t < tris_per_surf; ++t) {
            const std::uint16_t base =
                verts_per_surf > 0
                    ? static_cast<std::uint16_t>((t * 3) % verts_per_surf)
                    : 0;
            put_u16(b, base);
            put_u16(b, verts_per_surf > 1
                           ? static_cast<std::uint16_t>((base + 1) % verts_per_surf)
                           : 0);
            put_u16(b, verts_per_surf > 2
                           ? static_cast<std::uint16_t>((base + 2) % verts_per_surf)
                           : 0);
        }
    }

    return b;
}

// ── Temp-file scaffolding ──────────────────────────────────────

// load_mi08 opens a path, so each fuzz input is written to a uniquely-named
// file under the system temp directory and removed once the loader returns.
struct TempFile {
    std::filesystem::path path;
    bool ok = false;

    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               std::format("heaven_mi08_fuzz_{}.bin", id);
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

// Run load_mi08 over a fuzz buffer and assert the OOB-safety contract: control
// returns in a defined Ok-or-Error state (the no-crash / no-OOB guarantee under
// ASan). On success, the loader's structural guarantee (≥ 1 surface) holds.
void check_loads_safely(const std::vector<std::uint8_t>& bytes) {
    TempFile tf(bytes);
    RC_ASSERT(tf.ok); // temp file must be writable, else the test is invalid

    const heaven::Result<heaven::Mesh> res = heaven::load_mi08(tf.path.string());

    // Either outcome is acceptable — the property is that we got here at all,
    // without an out-of-bounds access or process termination. std::expected is
    // always in exactly one of its two states; touch it so the call cannot be
    // optimised away.
    if (res.has_value()) {
        // Surface count is validated to be in [1, kMaxSurfaces], so a loaded
        // mesh always has at least one surface.
        RC_ASSERT(!res->surfaces.empty());
    } else {
        // A rejected file carries a non-empty diagnostic naming the field/path

        RC_ASSERT(!res.error().message.empty());
    }
}

bool run_p1_random_bytes() {
    return rc::check(
        "Feature: codebase-hardening, Property 1: mi08 loader out-of-bounds "
        "safety — arbitrary bytes (Validates: Requirements 1.1, 1.2, 1.3, 1.4, "
        "1.5, 1.9, 1.10, 22.1)",
        [] {
            const auto bytes =
                *rc::gen::container<std::vector<std::uint8_t>>(
                    rc::gen::arbitrary<std::uint8_t>());
            check_loads_safely(bytes);
        });
}

bool run_p1_truncated_valid() {
    return rc::check(
        "Feature: codebase-hardening, Property 1: mi08 loader out-of-bounds "
        "safety — truncated valid file (Validates: Requirements 1.5, 1.9, "
        "1.10, 22.1)",
        [] {
            const int num_surfaces = *rc::gen::inRange(1, 4);
            const int verts = *rc::gen::inRange(0, 32);
            const int tris = *rc::gen::inRange(0, 32);
            const std::vector<std::uint8_t> full =
                build_valid_mi08(num_surfaces, verts, tris);

            // Cut the valid image at a random length: [0, full.size()].
            const std::size_t cut =
                *rc::gen::inRange<std::size_t>(0, full.size() + 1);
            const std::vector<std::uint8_t> truncated(full.begin(),
                full.begin() + static_cast<std::ptrdiff_t>(cut));
            check_loads_safely(truncated);
        });
}

bool run_p1_mutated_valid() {
    return rc::check(
        "Feature: codebase-hardening, Property 1: mi08 loader out-of-bounds "
        "safety — mutated valid file (Validates: Requirements 1.1, 1.2, 1.3, "
        "1.4, 1.9, 1.10, 22.1)",
        [] {
            const int num_surfaces = *rc::gen::inRange(1, 4);
            const int verts = *rc::gen::inRange(1, 32);
            const int tris = *rc::gen::inRange(1, 32);
            std::vector<std::uint8_t> bytes =
                build_valid_mi08(num_surfaces, verts, tris);

            // Overwrite a handful of random byte positions with random values.
            // This perturbs counts, lengths, magic, and payload alike, hitting
            // the range checks  and the short-read guards.
            const int mutations = *rc::gen::inRange(1, 8);
            for (int i = 0; i < mutations && !bytes.empty(); ++i) {
                const std::size_t pos =
                    *rc::gen::inRange<std::size_t>(0, bytes.size());
                bytes[pos] = *rc::gen::arbitrary<std::uint8_t>();
            }
            check_loads_safely(bytes);
        });
}

bool run_mi08_fuzz_properties() {
    bool ok = true;
    ok = run_p1_random_bytes() && ok;
    ok = run_p1_truncated_valid() && ok;
    ok = run_p1_mutated_valid() && ok;
    return ok;
}

const heaven::testing::Register g_register_mi08_fuzz{&run_mi08_fuzz_properties};

} // namespace
