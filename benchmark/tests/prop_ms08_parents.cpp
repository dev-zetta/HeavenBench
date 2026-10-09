/*
 * HeavenBench — Property test: ms08 loaded bone-parent range
 *
 * Feature: codebase-hardening
 *   •: Loaded bone parents are in range
 *
 *
 * The skinned-mesh loader (engine/skinned_mesh.cpp::load_smesh) parses an
 * untrusted ms08 binary asset whose per-bone parent index may reference a bone
 * that does not exist (or fall below the -1 root sentinel).
 * requires the loader to verify each parent is an integer in [-1, num_bones),
 * and  fixes a skip-and-recover contract: WHEN a bone parent is
 * out of range, THE loader SHALL set that bone's parent to -1 (root), continue
 * loading the remaining bones, and emit an error indication naming the bone.
 *
 * This property pins the post-condition that follows from that contract — FOR
 * every bone of a successfully loaded skinned mesh, the loaded parent index is
 * in the range [-1, num_bones). An out-of-range parent would otherwise index
 * the `bones` vector out of bounds during the parent-chain walk in
 * bone_world_transform.
 *
 * The synthesiser `build_ms08_with_fuzzed_parents` mirrors the on-disk layout
 * load_smesh expects:
 *   magic 'ms08' (uint32)
 *   num_bones (int32)
 *   per bone { name_len (int32, incl. terminator), name bytes, parent (int32) }
 *   num_frames (int32)
 *   frame data [num_bones * num_frames] { pos_scale (4×f32), rot (4×f32) }
 *   num_surfaces (int32)
 *   per surface { name_len (int32), name bytes }                 (all names)
 *   per surface {                                                 (all data)
 *     num_verts (int32),
 *     vertex { pos (3×f32), normal (3×u16), num_weights (u8),
 *              weights[num_weights] { bone (u16), weight (u8) } },
 *     num_tc0 (int32), uv (2×f32) per tc0,
 *     num_tc1 (int32) = 0,
 *     num_tris (int32), triangle (3×u16) index array
 *   }
 * The only fuzzed field is each bone's parent, drawn from a range that
 * deliberately straddles the valid window so a substantial fraction of parents
 * are out of range and must be recovered to -1 by the loader. Every other field
 * is fully valid, so load_smesh succeeds; the property asserts both that it
 * succeeds and that every loaded bone parent satisfies
 * `parent >= -1 && parent < num_bones`.
 *
 * load_smesh is GPU-free and host-testable. This file self-registers with the
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

#include "engine/skinned_mesh.h"

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

// load_smesh reads with std::fread into native-layout PODs, so we serialise the
// host's byte representation directly (the renderer targets little-endian x86).

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

// The ms08 magic, matching load_smesh's `'m' | ('s' << 8) | ('0' << 16) | ('8' << 24)`.
constexpr std::uint32_t kMs08Magic =
    'm' | ('s' << 8) | ('0' << 16) | ('8' << 24);

// ── Synthesiser with fuzzed bone parents ───────────────────────

// Build a structurally-valid ms08 byte image whose bone parent fields are taken
// verbatim from `parents` (one int32 per bone). Callers supply parents that may
// be out of [-1, num_bones) so the loader's clamp-to-root recovery path
// is exercised. Geometry is intentionally minimal — a
// single surface with a few in-range-skinned vertices and one triangle — since
// the bone parents are the field under test.
std::vector<std::uint8_t> build_ms08_with_fuzzed_parents(
    const std::vector<std::int32_t>& parents,
    int num_frames) {
    std::vector<std::uint8_t> b;
    const int num_bones = static_cast<int>(parents.size());

    put_u32(b, kMs08Magic);

    // ── Bones ───────────────────────────────────────────────────
    put_i32(b, num_bones);
    for (int i = 0; i < num_bones; ++i) {
        const std::string name = std::format("bone_{}", i);
        const std::int32_t name_len = static_cast<std::int32_t>(name.size()) + 1;
        put_i32(b, name_len);
        b.insert(b.end(), name.begin(), name.end());
        b.push_back('\0');
        put_i32(b, parents[i]); // fuzzed parent (possibly out of range)
    }

    // ── Animation frames ────────────────────────────────────────
    put_i32(b, num_frames);
    const std::size_t total_entries =
        static_cast<std::size_t>(num_bones) * static_cast<std::size_t>(num_frames);
    for (std::size_t i = 0; i < total_entries; ++i) {
        // pos_scale: identity translation, unit scale.
        put_f32(b, 0.0f);
        put_f32(b, 0.0f);
        put_f32(b, 0.0f);
        put_f32(b, 1.0f);
        // rot: identity quaternion stored xyzw (loader maps to glm w,x,y,z).
        put_f32(b, 0.0f);
        put_f32(b, 0.0f);
        put_f32(b, 0.0f);
        put_f32(b, 1.0f);
    }

    // ── Surfaces ────────────────────────────────────────────────
    constexpr int num_surfaces = 1;
    put_i32(b, num_surfaces);

    // Surface names (all names precede all data blocks).
    {
        const std::string name = "surf_0";
        const std::int32_t name_len = static_cast<std::int32_t>(name.size()) + 1;
        put_i32(b, name_len);
        b.insert(b.end(), name.begin(), name.end());
        b.push_back('\0');
    }

    // Surface data block.
    {
        constexpr int num_verts = 3;
        put_i32(b, num_verts);
        for (int v = 0; v < num_verts; ++v) {
            // Position.
            put_f32(b, static_cast<float>(v));
            put_f32(b, static_cast<float>(v) + 1.0f);
            put_f32(b, static_cast<float>(v) + 2.0f);
            // Compressed normal (u16 ×3).
            put_u16(b, 32768);
            put_u16(b, 32768);
            put_u16(b, 65535);
            // One weight binding to bone 0 (always in range when num_bones ≥ 1;
            // when num_bones == 0 the loader's fallback handles it).
            const std::uint8_t num_weights = 1;
            b.push_back(num_weights);
            put_u16(b, 0);   // bone index 0
            b.push_back(255); // full weight
        }

        // UV set 0: one (u, v) per vertex.
        put_i32(b, num_verts);
        for (int v = 0; v < num_verts; ++v) {
            put_f32(b, 0.25f);
            put_f32(b, 0.75f);
        }

        // UV set 1: none.
        put_i32(b, 0);

        // Triangles: a single in-range triangle (num_verts < 65536 → u16 path).
        constexpr int num_tris = 1;
        put_i32(b, num_tris);
        put_u16(b, 0);
        put_u16(b, 1);
        put_u16(b, 2);
    }

    return b;
}

// ── Temp-file scaffolding ──────────────────────────────────────

// load_smesh opens a path, so each synthesised input is written to a
// uniquely-named file under the system temp directory and removed once the
// loader returns.
struct TempFile {
    std::filesystem::path path;
    bool ok = false;

    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               std::format("heaven_ms08_parents_{}.bin", id);
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

bool run_p4_loaded_parents_in_range() {
    return rc::check(
        "Feature: codebase-hardening, Property 4: loaded bone parents are in "
        "range (Validates: Requirements 2.3, 2.4)",
        [] {
            // At least one bone so there is a parent field to fuzz; cap the
            // count well inside the loader's kMaxBones (4096) bound.
            const int num_bones = *rc::gen::inRange(1, 32);
            const int num_frames = *rc::gen::inRange(1, 4);

            // Draw each parent from a window that straddles the valid range
            // [-1, num_bones): the low end reaches below the -1 root sentinel
            // and the high end past the last bone, so a large fraction of
            // parents are out of range and must be recovered to -1 by the
            // loader.
            std::vector<std::int32_t> parents(num_bones);
            for (int i = 0; i < num_bones; ++i) {
                parents[i] = *rc::gen::inRange(-4, num_bones + 4);
            }

            const std::vector<std::uint8_t> bytes =
                build_ms08_with_fuzzed_parents(parents, num_frames);

            TempFile tf(bytes);
            RC_ASSERT(tf.ok); // temp file must be writable, else test is invalid

            const heaven::Result<heaven::SkinnedMesh> res =
                heaven::load_smesh(tf.path.string());

            // The file is structurally valid, so the loader succeeds; it only
            // recovers out-of-range parents to -1 rather than rejecting it.
            RC_ASSERT(res.has_value());

            // Post-condition: every loaded bone parent references a real bone
            // or the -1 root sentinel. Out-of-range parents were set to -1.
            const int loaded_bones = static_cast<int>(res->bones.size());
            for (const heaven::Bone& bone : res->bones) {
                RC_ASSERT(bone.parent >= -1);
                RC_ASSERT(bone.parent < loaded_bones);
            }
        });
}

const heaven::testing::Register g_register_ms08_parents{
    &run_p4_loaded_parents_in_range};

} // namespace
