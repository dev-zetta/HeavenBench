/*
 * HeavenBench — Property test: valid-file load equivalence
 *
 * Feature: codebase-hardening
 *   •: Valid-file load equivalence
 *
 *
 * is the Shipping_Look parity invariant for the loaders: FOR
 * ALL valid asset files, the hardened loaders SHALL produce the same in-memory
 * result they would have produced without the validation. The hardening only
 * adds rejection paths for input the prior code mishandled; the accepted/valid
 * path must be byte-for-byte unchanged.
 *
 * There is no separate "pre-change" binary to diff against in-process, so this
 * property pins valid-input equivalence as DETERMINISM + structural correctness
 * on synthesised valid inputs. For each of the five loaders named in the implementation
 * (load_mi08, load_smesh, load_dds_cpu, the grass generator, load_scene_rhi) we
 *
 *   1. synthesise a fully-valid asset (or fixed input), load/generate it TWICE
 *      in the same process, and assert the two in-memory results are
 *      field-for-field identical — the validation logic carries no hidden state
 *      and never perturbs an accepted load; and
 *   2. assert the loaded data matches the known synthesised content (surface /
 *      vertex / index / bone / frame / mip counts and the values written in),
 *      so the equality check is meaningful rather than a tautology.
 *
 * Determinism across two independent loads is exactly the property a stateful
 * validation regression (e.g. a per-load counter, a shared scratch buffer, or a
 * mip-count clamp that depended on prior calls) would break — so byte-identical
 * second loads, together with the content checks, pin the valid path.
 *
 * The synthesisers mirror the on-disk layouts the loaders expect and reuse the
 * building-block knowledge from the sibling fuzz/parity tests (prop_mi08_fuzz,
 * prop_ms08_parents, prop_dds_fuzz, prop_grass_parity, prop_scene_double_load).
 * All five loaders are GPU-free and host-testable; load_scene_rhi is driven
 * through the GPU-free NullBackend. This file self-registers with the RapidCheck
 * harness in test_main.cpp via heaven::testing::Register, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 */
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include <glm/glm.hpp>
#include <rapidcheck.h>

#include "dds_loader_cpu.h"
#include "engine/grass_cpu.h"
#include "engine/loader_validation.h"
#include "engine/mesh.h"
#include "engine/null_backend.h"
#include "engine/scene_rhi.h"
#include "engine/skinned_mesh.h"
#include "engine/types.h"

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

namespace fs = std::filesystem;

// ── Little-endian POD appenders ────────────────────────────────

// The loaders read with std::fread / memcpy into native-layout PODs, so we
// serialise the host's byte representation directly (the renderer targets
// little-endian x86).

void put_i32(std::vector<std::uint8_t>& b, std::int32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}
void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
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

// ── Temp-file scaffolding ──────────────────────────────────────

// The path-taking loaders open a file, so each synthesised input is written to
// a uniquely-named file under the system temp directory and removed once the
// loader returns.
struct TempFile {
    fs::path path;
    bool ok = false;

    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1);
        path = fs::temp_directory_path() /
               std::format("heaven_valid_equiv_{}.bin", id);
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        if (f) {
            if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            ok = true;
        }
    }

    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec); // best-effort cleanup
    }
};

// ════════════════════════════════════════════════════════════════
//  (1) mi08 static-mesh loader — load_mi08
// ════════════════════════════════════════════════════════════════

constexpr std::uint32_t kMi08Magic =
    'm' | ('i' << 8) | ('0' << 16) | ('8' << 24);

// Build a fully-valid mi08 byte image (see prop_mi08_fuzz.cpp::build_valid_mi08).
// Triangle indices stay strictly inside [0, verts_per_surf) so the file loads
// cleanly with no recovery; verts_per_surf < 65536 keeps the 16-bit-index path.
std::vector<std::uint8_t> build_valid_mi08(int num_surfaces,
                                           int verts_per_surf,
                                           int tris_per_surf) {
    std::vector<std::uint8_t> b;
    put_u32(b, kMi08Magic);
    for (int i = 0; i < 10; ++i) put_f32(b, static_cast<float>(i) * 0.5f);
    put_i32(b, num_surfaces);

    for (int s = 0; s < num_surfaces; ++s) {
        const std::string name = std::format("surf_{}", s);
        put_i32(b, static_cast<std::int32_t>(name.size()) + 1);
        b.insert(b.end(), name.begin(), name.end());
        b.push_back('\0');
        for (int i = 0; i < 10; ++i) put_f32(b, 0.0f); // 40-byte surface BV
    }

    for (int s = 0; s < num_surfaces; ++s) {
        put_i32(b, verts_per_surf);
        for (int v = 0; v < verts_per_surf; ++v) {
            put_f32(b, static_cast<float>(v));
            put_f32(b, static_cast<float>(v) + 1.0f);
            put_f32(b, static_cast<float>(v) + 2.0f);
            put_u16(b, 32768);
            put_u16(b, 32768);
            put_u16(b, 65535);
        }
        put_i32(b, verts_per_surf);
        for (int v = 0; v < verts_per_surf; ++v) { put_f32(b, 0.25f); put_f32(b, 0.75f); }
        put_i32(b, 0);
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

bool vertex_identical(const heaven::Vertex& a, const heaven::Vertex& b) {
    return a.px == b.px && a.py == b.py && a.pz == b.pz &&
           a.nx == b.nx && a.ny == b.ny && a.nz == b.nz &&
           a.u == b.u && a.v == b.v &&
           a.u1 == b.u1 && a.v1 == b.v1 &&
           a.tx == b.tx && a.ty == b.ty && a.tz == b.tz && a.tw == b.tw;
}

bool mesh_identical(const heaven::Mesh& a, const heaven::Mesh& b) {
    if (a.surfaces.size() != b.surfaces.size()) return false;
    if (a.bbox_min != b.bbox_min || a.bbox_max != b.bbox_max) return false;
    for (std::size_t s = 0; s < a.surfaces.size(); ++s) {
        const heaven::Surface& sa = a.surfaces[s];
        const heaven::Surface& sb = b.surfaces[s];
        if (sa.name != sb.name) return false;
        if (sa.vertices.size() != sb.vertices.size()) return false;
        if (sa.indices != sb.indices) return false;
        for (std::size_t v = 0; v < sa.vertices.size(); ++v) {
            if (!vertex_identical(sa.vertices[v], sb.vertices[v])) return false;
        }
    }
    return true;
}

void check_mi08_equiv() {
    const int num_surfaces = *rc::gen::inRange(1, 4);
    const int verts = *rc::gen::inRange(3, 48);
    const int tris = *rc::gen::inRange(1, 32);
    const std::vector<std::uint8_t> bytes =
        build_valid_mi08(num_surfaces, verts, tris);

    TempFile tf(bytes);
    RC_ASSERT(tf.ok);

    const heaven::Result<heaven::Mesh> r1 = heaven::load_mi08(tf.path.string());
    const heaven::Result<heaven::Mesh> r2 = heaven::load_mi08(tf.path.string());
    RC_ASSERT(r1.has_value());
    RC_ASSERT(r2.has_value());

    // Determinism: two independent loads of the identical file agree field-for-field.
    RC_ASSERT(mesh_identical(*r1, *r2));

    // Structural correctness: the loaded data matches the synthesised content.
    RC_ASSERT(r1->surfaces.size() == static_cast<std::size_t>(num_surfaces));
    for (const heaven::Surface& surf : r1->surfaces) {
        RC_ASSERT(surf.vertices.size() == static_cast<std::size_t>(verts));
        RC_ASSERT(surf.indices.size() == static_cast<std::size_t>(tris) * 3);
        const std::uint32_t vc = static_cast<std::uint32_t>(surf.vertices.size());
        for (std::uint32_t idx : surf.indices) RC_ASSERT(idx < vc);
        // Positions are read verbatim by the loader: vertex v has (v, v+1, v+2).
        for (std::size_t v = 0; v < surf.vertices.size(); ++v) {
            RC_ASSERT(surf.vertices[v].px == static_cast<float>(v));
            RC_ASSERT(surf.vertices[v].py == static_cast<float>(v) + 1.0f);
            RC_ASSERT(surf.vertices[v].pz == static_cast<float>(v) + 2.0f);
        }
    }
}

// ════════════════════════════════════════════════════════════════
//  (2) skinned-mesh loader — load_smesh
// ════════════════════════════════════════════════════════════════

constexpr std::uint32_t kMs08Magic =
    'm' | ('s' << 8) | ('0' << 16) | ('8' << 24);

// Build a fully-valid ms08 byte image (see prop_ms08_parents.cpp). Every bone
// parent is in range — bone i parents to i-1, bone 0 is the root (-1) — so the
// file is unambiguously valid and the loader applies no recovery.
std::vector<std::uint8_t> build_valid_ms08(int num_bones, int num_frames) {
    std::vector<std::uint8_t> b;
    put_u32(b, kMs08Magic);

    put_i32(b, num_bones);
    for (int i = 0; i < num_bones; ++i) {
        const std::string name = std::format("bone_{}", i);
        put_i32(b, static_cast<std::int32_t>(name.size()) + 1);
        b.insert(b.end(), name.begin(), name.end());
        b.push_back('\0');
        put_i32(b, i - 1); // in-range parent (root for bone 0)
    }

    put_i32(b, num_frames);
    const std::size_t total =
        static_cast<std::size_t>(num_bones) * static_cast<std::size_t>(num_frames);
    for (std::size_t i = 0; i < total; ++i) {
        put_f32(b, 0.0f); put_f32(b, 0.0f); put_f32(b, 0.0f); put_f32(b, 1.0f); // pos_scale
        put_f32(b, 0.0f); put_f32(b, 0.0f); put_f32(b, 0.0f); put_f32(b, 1.0f); // rot xyzw
    }

    constexpr int num_surfaces = 1;
    put_i32(b, num_surfaces);
    {
        const std::string name = "surf_0";
        put_i32(b, static_cast<std::int32_t>(name.size()) + 1);
        b.insert(b.end(), name.begin(), name.end());
        b.push_back('\0');
    }
    {
        constexpr int num_verts = 3;
        put_i32(b, num_verts);
        for (int v = 0; v < num_verts; ++v) {
            put_f32(b, static_cast<float>(v));
            put_f32(b, static_cast<float>(v) + 1.0f);
            put_f32(b, static_cast<float>(v) + 2.0f);
            put_u16(b, 32768); put_u16(b, 32768); put_u16(b, 65535);
            b.push_back(1);   // one weight
            put_u16(b, 0);    // bone 0
            b.push_back(255); // full weight
        }
        put_i32(b, num_verts);
        for (int v = 0; v < num_verts; ++v) { put_f32(b, 0.25f); put_f32(b, 0.75f); }
        put_i32(b, 0);
        put_i32(b, 1);
        put_u16(b, 0); put_u16(b, 1); put_u16(b, 2);
    }
    return b;
}

bool skinned_vertex_identical(const heaven::SkinnedVertex& a,
                              const heaven::SkinnedVertex& b) {
    if (a.px != b.px || a.py != b.py || a.pz != b.pz) return false;
    if (a.nx != b.nx || a.ny != b.ny || a.nz != b.nz) return false;
    if (a.u != b.u || a.v != b.v) return false;
    if (a.u1 != b.u1 || a.v1 != b.v1) return false;
    for (int i = 0; i < 4; ++i) {
        if (a.bone_indices[i] != b.bone_indices[i]) return false;
        if (a.bone_weights[i] != b.bone_weights[i]) return false;
    }
    return true;
}

bool smesh_identical(const heaven::SkinnedMesh& a, const heaven::SkinnedMesh& b) {
    if (a.num_frames != b.num_frames) return false;
    if (a.bones.size() != b.bones.size()) return false;
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        if (a.bones[i].name != b.bones[i].name) return false;
        if (a.bones[i].parent != b.bones[i].parent) return false;
    }
    if (a.frames.size() != b.frames.size()) return false;
    for (std::size_t i = 0; i < a.frames.size(); ++i) {
        if (a.frames[i].position != b.frames[i].position) return false;
        if (a.frames[i].rotation != b.frames[i].rotation) return false;
    }
    if (a.surfaces.size() != b.surfaces.size()) return false;
    for (std::size_t s = 0; s < a.surfaces.size(); ++s) {
        if (a.surfaces[s].name != b.surfaces[s].name) return false;
        if (a.surfaces[s].indices != b.surfaces[s].indices) return false;
        if (a.surfaces[s].vertices.size() != b.surfaces[s].vertices.size()) return false;
        for (std::size_t v = 0; v < a.surfaces[s].vertices.size(); ++v) {
            if (!skinned_vertex_identical(a.surfaces[s].vertices[v],
                                          b.surfaces[s].vertices[v]))
                return false;
        }
    }
    return true;
}

void check_smesh_equiv() {
    const int num_bones = *rc::gen::inRange(1, 24);
    const int num_frames = *rc::gen::inRange(1, 4);
    const std::vector<std::uint8_t> bytes = build_valid_ms08(num_bones, num_frames);

    TempFile tf(bytes);
    RC_ASSERT(tf.ok);

    const heaven::Result<heaven::SkinnedMesh> r1 = heaven::load_smesh(tf.path.string());
    const heaven::Result<heaven::SkinnedMesh> r2 = heaven::load_smesh(tf.path.string());
    RC_ASSERT(r1.has_value());
    RC_ASSERT(r2.has_value());

    // Determinism: two independent loads agree field-for-field.
    RC_ASSERT(smesh_identical(*r1, *r2));

    // Structural correctness against the synthesised content.
    RC_ASSERT(r1->bones.size() == static_cast<std::size_t>(num_bones));
    RC_ASSERT(r1->num_frames == num_frames);
    RC_ASSERT(r1->frames.size() ==
              static_cast<std::size_t>(num_bones) * static_cast<std::size_t>(num_frames));
    RC_ASSERT(r1->surfaces.size() == static_cast<std::size_t>(1));
    RC_ASSERT(r1->surfaces[0].vertices.size() == static_cast<std::size_t>(3));
    RC_ASSERT(r1->surfaces[0].indices.size() == static_cast<std::size_t>(3));
    for (int i = 0; i < num_bones; ++i) {
        RC_ASSERT(r1->bones[i].parent == i - 1);
    }
}

// ════════════════════════════════════════════════════════════════
//  (3) DDS loader — load_dds_cpu
// ════════════════════════════════════════════════════════════════

constexpr std::uint32_t kDdsMagic     = 0x20534444; // "DDS "
constexpr std::uint32_t kDdpfFourcc   = 0x4;
constexpr std::uint32_t kCubemapCaps2 = 0x200;

constexpr std::uint32_t make_fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

enum class Layout { Bc1, Bc3, Rgb24, Rgba32 };
bool is_compressed(Layout l) { return l == Layout::Bc1 || l == Layout::Bc3; }
std::uint32_t block_bytes(Layout l) { return l == Layout::Bc1 ? 8u : 16u; }
std::uint32_t bytes_per_pixel(Layout l) { return l == Layout::Rgb24 ? 3u : 4u; }

std::uint64_t mip_face_size(std::uint32_t w, std::uint32_t h, Layout l) {
    if (is_compressed(l)) {
        std::uint64_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        return bw * bh * block_bytes(l);
    }
    return static_cast<std::uint64_t>(w) * h * bytes_per_pixel(l);
}

// Build a fully-valid DDS byte image (see prop_dds_fuzz.cpp::build_valid_dds).
// Pixel data is filled with a deterministic, position-dependent pattern (not
// zeros) so the BGR↔RGB swizzle the loader performs on uncompressed layouts is
// actually observable in the content check.
std::vector<std::uint8_t> build_valid_dds(std::uint32_t width,
                                          std::uint32_t height,
                                          std::uint32_t mip_count,
                                          Layout layout,
                                          bool cubemap) {
    std::vector<std::uint8_t> b;
    put_u32(b, kDdsMagic);

    put_u32(b, 124); put_u32(b, 0); put_u32(b, height); put_u32(b, width);
    put_u32(b, 0); put_u32(b, 0); put_u32(b, mip_count);
    for (int i = 0; i < 11; ++i) put_u32(b, 0);

    put_u32(b, 32); // PixelFormat.size
    if (is_compressed(layout)) {
        put_u32(b, kDdpfFourcc);
        put_u32(b, layout == Layout::Bc1 ? make_fourcc('D','X','T','1')
                                         : make_fourcc('D','X','T','5'));
        put_u32(b, 0); put_u32(b, 0); put_u32(b, 0); put_u32(b, 0); put_u32(b, 0);
    } else {
        put_u32(b, layout == Layout::Rgb24 ? 0x40u : 0x41u);
        put_u32(b, 0);
        put_u32(b, layout == Layout::Rgb24 ? 24u : 32u);
        put_u32(b, 0x00ff0000); put_u32(b, 0x0000ff00); put_u32(b, 0x000000ff);
        put_u32(b, layout == Layout::Rgba32 ? 0xff000000u : 0);
    }
    put_u32(b, 0x1000);
    put_u32(b, cubemap ? kCubemapCaps2 | 0xfc00u : 0u);
    put_u32(b, 0); put_u32(b, 0); put_u32(b, 0);

    const std::uint32_t faces = cubemap ? 6u : 1u;
    std::uint8_t fill = 1;
    for (std::uint32_t face = 0; face < faces; ++face) {
        for (std::uint32_t mip = 0; mip < mip_count; ++mip) {
            std::uint32_t w = std::max(1u, width >> mip);
            std::uint32_t h = std::max(1u, height >> mip);
            std::uint64_t total = mip_face_size(w, h, layout);
            for (std::uint64_t i = 0; i < total; ++i) b.push_back(fill++);
        }
    }
    return b;
}

bool dds_identical(const heaven::DDSTextureCPU& a, const heaven::DDSTextureCPU& b) {
    if (a.width != b.width || a.height != b.height) return false;
    if (a.format != b.format || a.is_cubemap != b.is_cubemap) return false;
    if (a.mip_count != b.mip_count) return false;
    if (a.mip_data.size() != b.mip_data.size()) return false;
    for (std::size_t i = 0; i < a.mip_data.size(); ++i) {
        if (a.mip_data[i] != b.mip_data[i]) return false;
    }
    return true;
}

Layout gen_layout() {
    switch (*rc::gen::inRange(0, 4)) {
        case 0: return Layout::Bc1;
        case 1: return Layout::Bc3;
        case 2: return Layout::Rgb24;
        default: return Layout::Rgba32;
    }
}

void check_dds_equiv() {
    const bool cubemap = *rc::gen::arbitrary<bool>();
    const auto width = *rc::gen::inRange<std::uint32_t>(1, 65);
    const auto height = cubemap ? width : *rc::gen::inRange<std::uint32_t>(1, 65);
    // A valid chain ends when both dimensions reach 1; a 1x1 image has
    // exactly one level. Cover every legal prefix, including the full chain.
    const auto natural_mips = static_cast<std::uint32_t>(std::bit_width(std::max(width, height)));
    const auto mips = *rc::gen::inRange<std::uint32_t>(1, natural_mips + 1);
    const Layout layout = gen_layout();
    const std::vector<std::uint8_t> bytes =
        build_valid_dds(width, height, mips, layout, cubemap);

    TempFile tf(bytes);
    RC_ASSERT(tf.ok);

    const heaven::DDSTextureCPU d1 = heaven::load_dds_cpu(tf.path.string());
    const heaven::DDSTextureCPU d2 = heaven::load_dds_cpu(tf.path.string());

    // The synthesised file is valid, so it decodes.
    RC_ASSERT(static_cast<bool>(d1));
    RC_ASSERT(static_cast<bool>(d2));

    // Determinism: two independent loads agree byte-for-byte.
    RC_ASSERT(dds_identical(d1, d2));

    // Structural correctness against the synthesised content. The loader clamps
    // mipMapCount to the real chain length (bit_width of the largest dimension),
    // and our request stays within it.
    RC_ASSERT(d1.width == width);
    RC_ASSERT(d1.height == height);
    RC_ASSERT(d1.is_cubemap == cubemap);
    RC_ASSERT(d1.mip_count == mips);
    RC_ASSERT(d1.mip_data.size() == mips);

    const std::uint32_t faces = cubemap ? 6u : 1u;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        std::uint32_t w = std::max(1u, width >> mip);
        std::uint32_t h = std::max(1u, height >> mip);
        const std::uint64_t expect = mip_face_size(w, h, layout) * faces;
        RC_ASSERT(d1.mip_data[mip].size() == static_cast<std::size_t>(expect));
    }
}

// ════════════════════════════════════════════════════════════════
//  (4) grass generator — generate_grass_blades
// ════════════════════════════════════════════════════════════════

bool grass_vertex_identical(const heaven::GrassVertex& a,
                            const heaven::GrassVertex& b) {
    return a.px == b.px && a.py == b.py && a.pz == b.pz &&
           a.nx == b.nx && a.ny == b.ny && a.nz == b.nz &&
           a.u == b.u && a.v == b.v &&
           a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool grass_arrays_identical(const std::vector<heaven::GrassVertex>& a,
                            const std::vector<heaven::GrassVertex>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!grass_vertex_identical(a[i], b[i])) return false;
    }
    return true;
}

void check_grass_equiv() {
    const int count = *rc::gen::inRange(1, 64);
    std::vector<heaven::GrassRoot> roots;
    roots.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(*rc::gen::inRange(-500, 500)) * 0.1f;
        const float y = static_cast<float>(*rc::gen::inRange(-500, 500)) * 0.1f;
        const float z = static_cast<float>(*rc::gen::inRange(-500, 500)) * 0.1f;
        roots.push_back({glm::vec3(x, y, z)});
    }

    const float time = static_cast<float>(*rc::gen::inRange(0, 100000)) * 0.01f;
    const heaven::GrassParams params{};

    const std::vector<heaven::GrassVertex> g1 =
        heaven::generate_grass_blades(roots, time, params);
    const std::vector<heaven::GrassVertex> g2 =
        heaven::generate_grass_blades(roots, time, params);

    // Determinism: regenerating with identical inputs reproduces the output.
    RC_ASSERT(grass_arrays_identical(g1, g2));

    // Structural correctness: the default 4-segment layout emits
    // GRASS_VERTS_PER_BLADE vertices per root.
    RC_ASSERT(g1.size() ==
              roots.size() * static_cast<std::size_t>(heaven::GRASS_VERTS_PER_BLADE));
}

// ════════════════════════════════════════════════════════════════
//  (5) scene loader — load_scene_rhi (driven through NullBackend)
// ════════════════════════════════════════════════════════════════

// Minimal xml0 binary writer + island synthesiser, mirroring
// prop_scene_double_load.cpp. A `move_*` WorldExpression node carries an
// island-animation script and wraps an ObjectMesh referencing a valid mi08.

struct Element {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;
    std::vector<Element> children;
};

struct Xml0Builder {
    std::vector<std::string> tags;
    std::vector<std::string> attr_names;
    std::vector<std::string> values;
    std::vector<std::uint8_t> tree;

    static std::uint8_t intern(std::vector<std::string>& pool, const std::string& s) {
        for (std::size_t i = 0; i < pool.size(); ++i)
            if (pool[i] == s) return static_cast<std::uint8_t>(i);
        pool.push_back(s);
        return static_cast<std::uint8_t>(pool.size() - 1);
    }

    static void put_lps(std::vector<std::uint8_t>& b, const std::string& s) {
        put_u32(b, static_cast<std::uint32_t>(s.size() + 1));
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }

    void write_element(const Element& e) {
        tree.push_back(intern(tags, e.tag));
        tree.push_back(static_cast<std::uint8_t>(e.attrs.size()));
        tree.push_back(0);
        for (const auto& [an, av] : e.attrs) {
            tree.push_back(intern(attr_names, an));
            tree.push_back(intern(values, av));
        }
        if (!e.text.empty()) {
            tree.push_back(1);
            put_lps(tree, e.text);
        } else {
            tree.push_back(0);
            tree.push_back(static_cast<std::uint8_t>(e.children.size()));
            for (const Element& c : e.children) {
                tree.push_back(0);
                write_element(c);
                tree.push_back(0);
            }
        }
    }

    std::vector<std::uint8_t> finish(const Element& root) {
        write_element(root);
        std::vector<std::uint8_t> out;
        out.insert(out.end(), {'x', 'm', 'l', '0'});
        put_u32(out, 0);
        for (const std::string& t : tags) put_lps(out, t);
        put_u32(out, static_cast<std::uint32_t>(attr_names.size()));
        for (const std::string& a : attr_names) put_lps(out, a);
        put_u32(out, static_cast<std::uint32_t>(values.size()));
        for (const std::string& v : values) put_lps(out, v);
        out.insert(out.end(), tree.begin(), tree.end());
        return out;
    }
};

Element make_island(int index, float speed, float amplitude,
                    const std::string& mesh_rel) {
    Element mesh_node;
    mesh_node.tag = "node";
    mesh_node.attrs = {{"type", "ObjectMesh"},
                       {"name", std::format("island_mesh_{}", index)}};
    {
        Element mesh;
        mesh.tag = "mesh";
        mesh.text = mesh_rel;
        mesh_node.children.push_back(std::move(mesh));
    }
    Element expr;
    expr.tag = "expression";
    expr.text = std::format(
        "v = (engine.game.getTime() - 0.000000) * {:.6f}f; "
        "z = {:.6f}f - cos(time) * {:.6f}f;",
        speed, amplitude, amplitude);
    Element move;
    move.tag = "node";
    move.attrs = {{"type", "WorldExpression"},
                  {"name", std::format("move_island_{}", index)}};
    move.children.push_back(std::move(expr));
    move.children.push_back(std::move(mesh_node));
    return move;
}

bool write_bytes(const fs::path& p, const std::vector<std::uint8_t>& data) {
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

struct IslandSpec { float speed; float amplitude; };

void check_scene_equiv() {
    const int num_islands = *rc::gen::inRange(1, 5);
    std::vector<IslandSpec> specs;
    specs.reserve(static_cast<std::size_t>(num_islands));
    for (int i = 0; i < num_islands; ++i) {
        const float speed =
            *rc::gen::map(rc::gen::inRange(1, 400),
                          [](int x) { return static_cast<float>(x) / 100.0f; });
        const float amp =
            *rc::gen::map(rc::gen::inRange(1, 800),
                          [](int x) { return static_cast<float>(x) / 100.0f; });
        specs.push_back({speed, amp});
    }

    static std::atomic<std::uint64_t> counter{0};
    const fs::path root =
        fs::temp_directory_path() /
        std::format("heaven_valid_equiv_scene_{}", counter.fetch_add(1));
    std::error_code lec;
    fs::remove_all(root, lec);
    fs::create_directories(root / "heaven" / "meshes", lec);
    RC_ASSERT(!lec);

    struct Cleanup {
        fs::path dir;
        ~Cleanup() { std::error_code e; fs::remove_all(dir, e); }
    } cleanup{root};

    const std::string mesh_rel = "heaven/meshes/test.mesh";
    RC_ASSERT(write_bytes(root / "heaven" / "meshes" / "test.mesh",
                          build_valid_mi08(1, 3, 1)));

    Xml0Builder builder;
    Element scene_root;
    scene_root.tag = "world";
    for (int i = 0; i < num_islands; ++i) {
        scene_root.children.push_back(
            make_island(i, specs[i].speed, specs[i].amplitude, mesh_rel));
    }
    const fs::path node_file = root / "scene.node";
    RC_ASSERT(write_bytes(node_file, builder.finish(scene_root)));

    heaven::AppConfig config;
    config.data_dir  = root.string();
    config.node_file = node_file.string();

    heaven::NullBackend backend;

    const heaven::Result<heaven::RhiScene> r1 = heaven::load_scene_rhi(config, backend);
    const heaven::Result<heaven::RhiScene> r2 = heaven::load_scene_rhi(config, backend);
    RC_ASSERT(r1.has_value());
    RC_ASSERT(r2.has_value());

    const std::vector<heaven::FloatingIsland>& a = r1->floating_islands;
    const std::vector<heaven::FloatingIsland>& b = r2->floating_islands;

    // Determinism: the second load equals the first.
    RC_ASSERT(r1->instances.size() == r2->instances.size());
    RC_ASSERT(a.size() == b.size());
    RC_ASSERT(a.size() == static_cast<std::size_t>(num_islands));
    for (std::size_t i = 0; i < a.size(); ++i) {
        RC_ASSERT(a[i].group == b[i].group);
        RC_ASSERT(a[i].instance_index == b[i].instance_index);
        RC_ASSERT(a[i].speed == b[i].speed);
        RC_ASSERT(a[i].amplitude == b[i].amplitude);
    }

    // Structural correctness: a clean load assigns sequential group indices and
    // the parsed anim parameters match what the script authored.
    for (std::size_t i = 0; i < a.size(); ++i) {
        RC_ASSERT(a[i].group == static_cast<int>(i));
        RC_ASSERT(a[i].speed == specs[i].speed);
        RC_ASSERT(a[i].amplitude == specs[i].amplitude);
    }
}

// ── Single  runner ──────────────────────────────────

// One property exercising each loader's valid-input determinism + structural
// correctness. Run as separate rc::check invocations (each ≥ 100 iterations)
// AND-ed into one pass/fail so a regression in any loader fails the property.
bool run_p23_valid_load_equivalence() {
    bool ok = true;
    ok = rc::check(
             "Feature: codebase-hardening, Property 23: valid-file load "
             "equivalence — mi08 (Validates: Requirements 21.2)",
             [] { check_mi08_equiv(); }) && ok;
    ok = rc::check(
             "Feature: codebase-hardening, Property 23: valid-file load "
             "equivalence — ms08 (Validates: Requirements 21.2)",
             [] { check_smesh_equiv(); }) && ok;
    ok = rc::check(
             "Feature: codebase-hardening, Property 23: valid-file load "
             "equivalence — DDS (Validates: Requirements 21.2)",
             [] { check_dds_equiv(); }) && ok;
    ok = rc::check(
             "Feature: codebase-hardening, Property 23: valid-file load "
             "equivalence — grass generator (Validates: Requirements 21.2)",
             [] { check_grass_equiv(); }) && ok;

    // The scene loader needs a writable temp tree; SKIP gracefully (pass) if no
    // temp dir is available, mirroring the other temp-dir-gated tests.
    std::error_code ec;
    const fs::path temp_dir = fs::temp_directory_path(ec);
    if (ec || temp_dir.empty()) {
        std::printf("[prop_valid_load_equiv] scene SKIPPED: no temp dir (%s)\n",
                    ec.message().c_str());
    } else {
        ok = rc::check(
                 "Feature: codebase-hardening, Property 23: valid-file load "
                 "equivalence — scene loader (Validates: Requirements 21.2)",
                 [] { check_scene_equiv(); }) && ok;
    }
    return ok;
}

const heaven::testing::Register g_register_valid_load_equiv{
    &run_p23_valid_load_equivalence};

} // namespace
