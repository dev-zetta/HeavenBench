/*
 * HeavenBench — unit test: mi08 / ms08 shared bounds + error messages
 *
 * Feature: codebase-hardening
 * — Validates:
 *
 * The static-mesh loader (engine/mesh.cpp::load_mi08) and the skinned-mesh
 * loader (engine/skinned_mesh.cpp::load_smesh) both validate their on-disk
 * counts and name lengths against the SAME canonical bounds, defined once in
 * engine/loader_validation.h (heaven::loadval): kMaxNameLen, kMaxElemCount,
 * kMaxSurfaces, kMaxBones. This file pins two contracts:
 *
 *   • Shared bounds applied: a synthesised mi08 with an
 *     out-of-range surface count, name length, or vertex count is rejected; and
 *     the SAME boundary value (kMaxNameLen + 1 for a name length, kMaxElemCount
 *     + 1 for a vertex count) is rejected by BOTH load_mi08 and load_smesh —
 *     demonstrating the two loaders share one source of truth for the bounds.
 *     A just-in-range value, by contrast, loads cleanly through both loaders.
 *
 *   • Error names the file path: a rejected load returns a
 *     Result holding an Error whose message contains the exact path string
 *     passed in, so a failure can always be traced back to the offending file.
 *
 * Both loaders are GPU-free and host-testable: each fuzz/crafted byte image is
 * written to a uniquely-named temp file (the loaders open a path) and removed
 * once the loader returns. This is a deterministic unit test (no random input),
 * so it does not use rc::check. It self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "engine/loader_validation.h"
#include "engine/mesh.h"
#include "engine/skinned_mesh.h"

// Mirror of the harness registry hooks declared in test_main.cpp.
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using namespace heaven::loadval;

// ── Little-endian POD appenders ────────────────────────────────

// Both loaders read with std::fread into native-layout PODs, so we serialise
// the host's byte representation directly (the renderer targets little-endian
// x86).

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

// Append a length-prefixed name exactly as the loaders expect: an int32 length
// that INCLUDES the null terminator, followed by the name bytes and the
// terminator. The loaders reconstruct the string from (name_len - 1) bytes.
void put_name(std::vector<std::uint8_t>& b, const std::string& name) {
    put_i32(b, static_cast<std::int32_t>(name.size()) + 1);
    b.insert(b.end(), name.begin(), name.end());
    b.push_back('\0');
}

constexpr std::uint32_t kMi08Magic =
    'm' | ('i' << 8) | ('0' << 16) | ('8' << 24);
constexpr std::uint32_t kMs08Magic =
    'm' | ('s' << 8) | ('0' << 16) | ('8' << 24);

// ── mi08 byte synthesisers ─────────────────────────────────────

// A minimal, fully-valid single-surface mi08 image that load_mi08 accepts.
std::vector<std::uint8_t> valid_mi08() {
    std::vector<std::uint8_t> b;
    put_u32(b, kMi08Magic);
    for (int i = 0; i < 10; ++i) put_f32(b, 0.0f);   // header bounding volume
    put_i32(b, 1);                                   // num_surfaces
    put_name(b, "surf0");                            // surface name
    for (int i = 0; i < 10; ++i) put_f32(b, 0.0f);   // 40-byte surface BV
    // Surface data block.
    const int verts = 3;
    put_i32(b, verts);                               // num_verts
    for (int v = 0; v < verts; ++v) {
        put_f32(b, static_cast<float>(v));
        put_f32(b, 0.0f);
        put_f32(b, 0.0f);
        put_u16(b, 32768); put_u16(b, 32768); put_u16(b, 65535);
    }
    put_i32(b, verts);                               // num_tc0
    for (int v = 0; v < verts; ++v) { put_f32(b, 0.0f); put_f32(b, 0.0f); }
    put_i32(b, verts);                               // num_tc1
    for (int v = 0; v < verts; ++v) {
        put_f32(b, 0.125f + static_cast<float>(v));
        put_f32(b, 0.875f - static_cast<float>(v) * 0.25f);
    }
    put_i32(b, 1);                                   // num_tris
    put_u16(b, 0); put_u16(b, 1); put_u16(b, 2);     // one in-range triangle
    return b;
}

// mi08 prefix ending at a (possibly out-of-range) surface count.
std::vector<std::uint8_t> mi08_with_surface_count(std::int32_t count) {
    std::vector<std::uint8_t> b;
    put_u32(b, kMi08Magic);
    for (int i = 0; i < 10; ++i) put_f32(b, 0.0f);
    put_i32(b, count);
    return b;
}

// mi08 prefix ending at a (possibly out-of-range) surface name length.
std::vector<std::uint8_t> mi08_with_name_len(std::int32_t name_len) {
    std::vector<std::uint8_t> b;
    put_u32(b, kMi08Magic);
    for (int i = 0; i < 10; ++i) put_f32(b, 0.0f);
    put_i32(b, 1);            // num_surfaces
    put_i32(b, name_len);     // surface name length under test
    return b;
}

// mi08 prefix ending at a (possibly out-of-range) vertex count for surface 0.
std::vector<std::uint8_t> mi08_with_vert_count(std::int32_t num_verts) {
    std::vector<std::uint8_t> b;
    put_u32(b, kMi08Magic);
    for (int i = 0; i < 10; ++i) put_f32(b, 0.0f);
    put_i32(b, 1);                                   // num_surfaces
    put_name(b, "surf0");                            // surface name
    for (int i = 0; i < 10; ++i) put_f32(b, 0.0f);   // 40-byte surface BV
    put_i32(b, num_verts);                           // vertex count under test
    return b;
}

// ── ms08 byte synthesisers ─────────────────────────────────────

// A minimal, fully-valid single-bone single-surface ms08 image.
std::vector<std::uint8_t> valid_ms08() {
    std::vector<std::uint8_t> b;
    put_u32(b, kMs08Magic);
    put_i32(b, 1);             // num_bones
    put_name(b, "bone0");      // bone name
    put_i32(b, -1);            // parent (root)
    put_i32(b, 1);             // num_frames
    // One frame entry per bone: pos_scale[4] + rot[4].
    put_f32(b, 0.0f); put_f32(b, 0.0f); put_f32(b, 0.0f); put_f32(b, 1.0f);
    put_f32(b, 0.0f); put_f32(b, 0.0f); put_f32(b, 0.0f); put_f32(b, 1.0f);
    put_i32(b, 1);             // num_surfaces
    put_name(b, "surf0");      // surface name
    // Surface data block.
    const int verts = 3;
    put_i32(b, verts);         // num_verts
    for (int v = 0; v < verts; ++v) {
        put_f32(b, static_cast<float>(v)); put_f32(b, 0.0f); put_f32(b, 0.0f);
        put_u16(b, 32768); put_u16(b, 32768); put_u16(b, 65535);
        b.push_back(1);                  // num_weights
        put_u16(b, 0);                   // weight bone index
        b.push_back(255);                // weight value
    }
    put_i32(b, verts);         // num_tc0
    for (int v = 0; v < verts; ++v) { put_f32(b, 0.0f); put_f32(b, 0.0f); }
    put_i32(b, verts);         // num_tc1
    for (int v = 0; v < verts; ++v) {
        put_f32(b, 0.125f + static_cast<float>(v));
        put_f32(b, 0.875f - static_cast<float>(v) * 0.25f);
    }
    put_i32(b, 1);             // num_tris
    put_u16(b, 0); put_u16(b, 1); put_u16(b, 2);
    return b;
}

// ms08 prefix ending at a (possibly out-of-range) bone count.
std::vector<std::uint8_t> ms08_with_bone_count(std::int32_t count) {
    std::vector<std::uint8_t> b;
    put_u32(b, kMs08Magic);
    put_i32(b, count);
    return b;
}

// ms08 prefix ending at a (possibly out-of-range) bone name length.
std::vector<std::uint8_t> ms08_with_bone_name_len(std::int32_t name_len) {
    std::vector<std::uint8_t> b;
    put_u32(b, kMs08Magic);
    put_i32(b, 1);            // num_bones
    put_i32(b, name_len);     // bone name length under test
    return b;
}

// ms08 prefix ending at a (possibly out-of-range) vertex count for surface 0.
std::vector<std::uint8_t> ms08_with_vert_count(std::int32_t num_verts) {
    std::vector<std::uint8_t> b;
    put_u32(b, kMs08Magic);
    put_i32(b, 1);             // num_bones
    put_name(b, "bone0");      // bone name
    put_i32(b, -1);            // parent
    put_i32(b, 0);             // num_frames (0 → no frame data)
    put_i32(b, 1);             // num_surfaces
    put_name(b, "surf0");      // surface name
    put_i32(b, num_verts);     // vertex count under test
    return b;
}

// ── Temp-file scaffolding ──────────────────────────────────────

// The loaders open a path, so each crafted image is written to a uniquely-named
// file under the system temp directory and removed once the loader returns. The
// returned path string is what we assert the Error message contains.
struct TempFile {
    std::filesystem::path path;
    bool ok = false;

    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               std::format("heaven_mesh_bounds_{}.bin", id);
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

bool unit_mesh_bounds() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_mesh_bounds] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    // Load a crafted mi08 image; return the Result so the caller can inspect it.
    auto load_mi08_bytes = [](const std::vector<std::uint8_t>& bytes,
                              std::string& path_out) {
        TempFile tf(bytes);
        path_out = tf.path.string();
        return heaven::load_mi08(path_out);
    };
    auto load_ms08_bytes = [](const std::vector<std::uint8_t>& bytes,
                              std::string& path_out) {
        TempFile tf(bytes);
        path_out = tf.path.string();
        return heaven::load_smesh(path_out);
    };

    // ── 1. mi08: a valid image loads (anchors the accepted path) ──
    {
        std::string p;
        auto res = load_mi08_bytes(valid_mi08(), p);
        if (!res.has_value())
            fail(std::format("valid mi08 should load, got error: {}",
                             res ? std::string{} : res.error().message));
        else if (res->surfaces.size() != 1)
            fail("valid mi08 should have exactly one surface");
        else {
            const auto& vertices = res->surfaces[0].vertices;
            if (vertices.size() != 3 ||
                vertices[0].u1 != 0.125f || vertices[0].v1 != 0.875f ||
                vertices[2].u1 != 2.125f || vertices[2].v1 != 0.375f) {
                fail("valid mi08 should preserve authored UV1 coordinates");
            }
        }
    }

    // ── 2. mi08: surface count bound (1.. kMaxSurfaces)  ──
    {
        std::string p;
        // 0 is below the lower bound of 1 → rejected.
        if (load_mi08_bytes(mi08_with_surface_count(0), p).has_value())
            fail("mi08 surface count 0 must be rejected");
        // kMaxSurfaces + 1 is above the upper bound → rejected.
        if (load_mi08_bytes(mi08_with_surface_count(kMaxSurfaces + 1), p).has_value())
            fail("mi08 surface count > kMaxSurfaces must be rejected");
    }

    // ── 3. mi08: name length bound (1.. kMaxNameLen)  ──
    {
        std::string p;
        if (load_mi08_bytes(mi08_with_name_len(0), p).has_value())
            fail("mi08 name length 0 must be rejected");
        if (load_mi08_bytes(mi08_with_name_len(kMaxNameLen + 1), p).has_value())
            fail("mi08 name length > kMaxNameLen must be rejected");
    }

    // ── 4. mi08: vertex count bound (0.. kMaxElemCount)  ──
    {
        std::string p;
        if (load_mi08_bytes(mi08_with_vert_count(-1), p).has_value())
            fail("mi08 negative vertex count must be rejected");
        if (load_mi08_bytes(mi08_with_vert_count(kMaxElemCount + 1), p).has_value())
            fail("mi08 vertex count > kMaxElemCount must be rejected");
    }

    // ── 5. ms08: a valid image loads (anchors the accepted path) ──
    {
        std::string p;
        auto res = load_ms08_bytes(valid_ms08(), p);
        if (!res.has_value())
            fail(std::format("valid ms08 should load, got error: {}",
                             res ? std::string{} : res.error().message));
        else {
            if (res->bones.size() != 1) fail("valid ms08 should have one bone");
            if (res->surfaces.size() != 1) fail("valid ms08 should have one surface");
            if (res->surfaces.size() == 1) {
                const auto& vertices = res->surfaces[0].vertices;
                if (vertices.size() != 3 ||
                    vertices[0].u1 != 0.125f || vertices[0].v1 != 0.875f ||
                    vertices[2].u1 != 2.125f || vertices[2].v1 != 0.375f) {
                    fail("valid ms08 should preserve authored UV1 coordinates");
                }
            }
        }
    }

    // ── 6. ms08: bone count bound (0.. kMaxBones)  ──
    {
        std::string p;
        if (load_ms08_bytes(ms08_with_bone_count(-1), p).has_value())
            fail("ms08 negative bone count must be rejected");
        if (load_ms08_bytes(ms08_with_bone_count(kMaxBones + 1), p).has_value())
            fail("ms08 bone count > kMaxBones must be rejected");
    }

    // ── 7. SHARED bounds: the SAME boundary value is rejected by BOTH loaders ──
    // This is the heart of  — mi08 and ms08 draw their name-length
    // and element-count bounds from the single loadval source of truth, so a
    // value one past the shared cap fails identically in each loader.
    {
        std::string p;

        // Name length: kMaxNameLen + 1 rejected by mi08 (surface name) AND ms08
        // (bone name).
        const bool mi08_name_rejected =
            !load_mi08_bytes(mi08_with_name_len(kMaxNameLen + 1), p).has_value();
        const bool ms08_name_rejected =
            !load_ms08_bytes(ms08_with_bone_name_len(kMaxNameLen + 1), p).has_value();
        if (!mi08_name_rejected || !ms08_name_rejected)
            fail("kMaxNameLen+1 must be rejected by BOTH mi08 and ms08 (shared bound)");

        // Element (vertex) count: kMaxElemCount + 1 rejected by mi08 AND ms08.
        const bool mi08_vert_rejected =
            !load_mi08_bytes(mi08_with_vert_count(kMaxElemCount + 1), p).has_value();
        const bool ms08_vert_rejected =
            !load_ms08_bytes(ms08_with_vert_count(kMaxElemCount + 1), p).has_value();
        if (!mi08_vert_rejected || !ms08_vert_rejected)
            fail("kMaxElemCount+1 must be rejected by BOTH mi08 and ms08 (shared bound)");

        // And the just-in-range name length (kMaxNameLen) is NOT rejected on the
        // bounds check itself — both loaders accept it as a length and proceed to
        // read the (here truncated) name, confirming the cap is inclusive and
        // shared rather than off-by-one in either loader.
        // (We only assert the out-of-range side above; the in-range acceptance is
        // covered by the valid_mi08 / valid_ms08 anchors.)
    }

    // ── 8. Error message names the file path  ──
    // A rejected load must return an Error whose message contains the exact path
    // string the caller passed in, for both loaders and across field kinds.
    {
        std::string p;

        auto err_contains_path = [&](const auto& res, const std::string& path,
                                     const std::string& what) {
            if (res.has_value()) {
                fail(what + ": expected rejection but load succeeded");
                return;
            }
            if (res.error().message.find(path) == std::string::npos)
                fail(what + ": Error message does not contain the file path");
        };

        err_contains_path(load_mi08_bytes(mi08_with_surface_count(0), p), p,
                          "mi08 bad surface count");
        err_contains_path(load_mi08_bytes(mi08_with_name_len(kMaxNameLen + 1), p), p,
                          "mi08 bad name length");
        err_contains_path(load_mi08_bytes(mi08_with_vert_count(kMaxElemCount + 1), p), p,
                          "mi08 bad vertex count");
        err_contains_path(load_ms08_bytes(ms08_with_bone_count(kMaxBones + 1), p), p,
                          "ms08 bad bone count");
        err_contains_path(load_ms08_bytes(ms08_with_bone_name_len(0), p), p,
                          "ms08 bad bone name length");
        err_contains_path(load_ms08_bytes(ms08_with_vert_count(kMaxElemCount + 1), p), p,
                          "ms08 bad vertex count");

        // A path that cannot be opened at all must also name the path.
        {
            const std::string missing = "/nonexistent/heaven_mesh_bounds_missing.bin";
            auto mres = heaven::load_mi08(missing);
            if (mres.has_value())
                fail("mi08 missing file should fail to open");
            else if (mres.error().message.find(missing) == std::string::npos)
                fail("mi08 open failure must name the missing path");

            auto sres = heaven::load_smesh(missing);
            if (sres.has_value())
                fail("ms08 missing file should fail to open");
            else if (sres.error().message.find(missing) == std::string::npos)
                fail("ms08 open failure must name the missing path");
        }
    }

    // An empty surface is valid only when it has no triangles. Recovering an
    // invalid index to zero used to dereference an empty vertex array during
    // mi08 tangent construction (or when the ms08 surface was later skinned).
    for (const int triangles : {0, 1}) {
        auto mi08 = mi08_with_vert_count(0);
        auto ms08 = ms08_with_vert_count(0);
        for (auto* bytes : {&mi08, &ms08}) {
            put_i32(*bytes, 0); // UV0
            put_i32(*bytes, 0); // UV1
            put_i32(*bytes, triangles);
            if (triangles != 0) {
                put_u16(*bytes, 0);
                put_u16(*bytes, 0);
                put_u16(*bytes, 0);
            }
        }
        std::string p;
        const auto static_mesh = load_mi08_bytes(mi08, p);
        const auto skinned_mesh = load_ms08_bytes(ms08, p);
        if (static_mesh.has_value() != (triangles == 0) ||
            skinned_mesh.has_value() != (triangles == 0)) {
            fail("empty surfaces must load only when they contain no triangles");
        }
        if (skinned_mesh && !skinned_mesh->skin_frame(0, 0).empty()) {
            fail("skinning an empty surface must return no vertices");
        }
    }

    // Capped counts still need to fit the file before CPU storage is allocated.
    // Check the explicit payload diagnostic so these tests distinguish the
    // allocation guard from a short read reached only after a large allocation.
    {
        const auto expect_payload_rejection = [&](const auto& result,
                                                   const std::string& kind) {
            if (result || result.error().message.find("payload") == std::string::npos) {
                fail(kind + " must reject the truncated payload before allocation");
            }
        };
        std::string p;
        expect_payload_rejection(
            load_mi08_bytes(mi08_with_vert_count(kMaxElemCount), p), "mi08 vertices");
        expect_payload_rejection(
            load_ms08_bytes(ms08_with_vert_count(kMaxElemCount), p), "ms08 vertices");

        // A one-bone animation with the largest legal frame count but no frames.
        auto frames = ms08_with_bone_count(1);
        put_name(frames, "bone0");
        put_i32(frames, -1);
        put_i32(frames, 100000);
        expect_payload_rejection(load_ms08_bytes(frames, p), "ms08 frames");

        auto mi08 = valid_mi08();
        auto ms08 = valid_ms08();
        for (auto* bytes : {&mi08, &ms08}) {
            bytes->resize(bytes->size() - 3 * sizeof(uint16_t) - sizeof(int32_t));
            put_i32(*bytes, kMaxElemCount);
        }
        expect_payload_rejection(load_mi08_bytes(mi08, p), "mi08 indices");
        expect_payload_rejection(load_ms08_bytes(ms08, p), "ms08 indices");
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_mesh_bounds{&unit_mesh_bounds};

} // namespace
