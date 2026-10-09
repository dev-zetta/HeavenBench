/*
 * HeavenBench — Property test: mi08 loaded-index range
 *
 * Feature: codebase-hardening
 *   •: mi08 loaded indices are in range
 *
 *
 * The mi08 loader (engine/mesh.cpp::load_mi08) parses an untrusted binary asset
 * file whose per-surface triangle indices may reference vertices that do not
 * exist.  fixes a skip-and-recover contract: WHEN a triangle
 * index value is greater than or equal to that surface's vertex count, THE
 * loader SHALL replace the offending index with 0 and continue loading. This
 * property pins the post-condition that follows from that contract — FOR every
 * surface of a successfully loaded mesh, every loaded index is strictly less
 * than that surface's vertex count.
 *
 * The synthesiser `build_mi08_with_oob_indices` mirrors the on-disk layout
 * load_mi08 expects (see prop_mi08_fuzz.cpp's build_valid_mi08): magic 'mi08',
 * a 10-float header bounding volume, an int32 surface count, then per-surface
 * { int32 name length, name bytes, 40-byte surface bounding volume } headers,
 * then per-surface { int32 num_verts, vertex (3×f32 pos + 3×u16 normal) array,
 * int32 num_tc0, uv (2×f32) array, int32 num_tc1 (0), int32 num_tris, triangle
 * (3×u16) index array } data blocks. The only difference from the fully-valid
 * synthesiser is the triangle indices: each index is drawn from a range that
 * deliberately reaches past num_verts, so a substantial fraction of indices are
 * out of range and must be recovered to 0 by the loader. num_verts is kept
 * < 65536 so the loader takes its 16-bit-index branch, which is where the
 * out-of-range values flow through.
 *
 * Because the file is otherwise structurally valid, load_mi08 succeeds; the
 * property asserts both that it succeeds and that every loaded index across
 * every surface satisfies `index < surface.vertices.size()`.
 *
 * load_mi08 is GPU-free and host-testable. This file self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
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

void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}

// The mi08 magic, matching load_mi08's `'m' | ('i' << 8) | ('0' << 16) | ('8' << 24)`.
constexpr std::uint32_t kMi08Magic =
    'm' | ('i' << 8) | ('0' << 16) | ('8' << 24);

// ── Synthesiser with out-of-range triangle indices ────────────

// Build a structurally-valid mi08 byte image whose triangle indices are taken
// verbatim from `tri_indices[s]` (one flat array of 3×num_tris u16 values per
// surface). Callers supply indices that deliberately exceed num_verts so the
// loader's skip-and-recover path  is exercised.
std::vector<std::uint8_t> build_mi08_with_oob_indices(
    int verts_per_surf,
    const std::vector<std::vector<std::uint16_t>>& tri_indices) {
    std::vector<std::uint8_t> b;
    const int num_surfaces = static_cast<int>(tri_indices.size());

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

        // Triangle indices: 3×u16 per triangle, taken verbatim from the caller.
        const std::vector<std::uint16_t>& idx = tri_indices[s];
        const std::int32_t num_tris = static_cast<std::int32_t>(idx.size() / 3);
        put_i32(b, num_tris);
        for (std::uint16_t i : idx) put_u16(b, i);
    }

    return b;
}

// ── Temp-file scaffolding ──────────────────────────────────────

// load_mi08 opens a path, so each synthesised input is written to a
// uniquely-named file under the system temp directory and removed once the
// loader returns.
struct TempFile {
    std::filesystem::path path;
    bool ok = false;

    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               std::format("heaven_mi08_idx_{}.bin", id);
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

bool run_p2_loaded_indices_in_range() {
    return rc::check(
        "Feature: codebase-hardening, Property 2: mi08 loaded indices are in "
        "range (Validates: Requirements 1.6)",
        [] {
            const int num_surfaces = *rc::gen::inRange(1, 4);
            // Keep num_verts ≥ 1 (so index 0 is always valid) and < 65536 so
            // the loader takes its 16-bit-index branch.
            const int verts = *rc::gen::inRange(1, 64);

            // Build the per-surface index arrays. Each index is drawn from
            // [0, verts + 256): values ≥ verts are out of range and must be
            // recovered to 0 by the loader. The upper bound
            // comfortably exceeds verts so a large fraction of indices are OOB.
            std::vector<std::vector<std::uint16_t>> tri_indices(num_surfaces);
            const std::uint16_t hi =
                static_cast<std::uint16_t>(verts + 256);
            for (int s = 0; s < num_surfaces; ++s) {
                const int tris = *rc::gen::inRange(1, 32);
                tri_indices[s].reserve(static_cast<std::size_t>(tris) * 3);
                for (int i = 0; i < tris * 3; ++i) {
                    tri_indices[s].push_back(
                        *rc::gen::inRange<std::uint16_t>(0, hi));
                }
            }

            const std::vector<std::uint8_t> bytes =
                build_mi08_with_oob_indices(verts, tri_indices);

            TempFile tf(bytes);
            RC_ASSERT(tf.ok); // temp file must be writable, else test is invalid

            const heaven::Result<heaven::Mesh> res =
                heaven::load_mi08(tf.path.string());

            // The file is structurally valid, so the loader succeeds; it only
            // recovers the out-of-range indices rather than rejecting the file.
            RC_ASSERT(res.has_value());

            // Post-condition: every loaded index references a real vertex of
            // its surface. Out-of-range indices were replaced with 0.
            for (const heaven::Surface& surf : res->surfaces) {
                const std::uint32_t vertex_count =
                    static_cast<std::uint32_t>(surf.vertices.size());
                for (std::uint32_t idx : surf.indices) {
                    RC_ASSERT(idx < vertex_count);
                }
            }
        });
}

const heaven::testing::Register g_register_mi08_indices{
    &run_p2_loaded_indices_in_range};

} // namespace
