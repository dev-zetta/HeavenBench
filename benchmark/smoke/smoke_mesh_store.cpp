/*
 * HeavenBench — mesh-store vertex re-upload count-validation smoke case
 *                    (renderer-linked, GATED)
 *
 * MeshStore::upload
 * records each surface's vertex count, and MeshStore::update_vertices (the
 * CPU-skinning re-upload path) rejects a re-upload whose incoming count does NOT
 * match that recorded count — returning an Error and touching no buffer — so a
 * mismatched push cannot copy the wrong byte span into the interleaved vertex
 * buffer and corrupt it. The valid CPU-skinning path always preserves the
 * bind-mesh vertex count, so a matching re-upload proceeds exactly as before
 *
 *
 * WHY THE SMOKE HARNESS (not heaven_benchmark_tests): the VertexBuffer /
 * IndexBuffer this exercises are real Filament resources built against a live
 * engine. The headless RapidCheck target links no Filament, so it cannot upload
 * a real buffer. This target links the full renderer, so it can — matching the
 * gated-smoke seam strategy ("MeshStore::update_vertices rejects a
 * mismatched count against a real uploaded buffer").
 *
 * HERMETIC ENGINE: the case stands up its OWN no-op (NOOP) Filament engine +
 * MeshStore (NOOP needs no GPU/display), uploads a small real mesh, and asserts
 * against that real uploaded buffer. It does not depend on (or perturb) the
 * shared backend, and SKIPs (exit 0 via the harness) if NOOP can't be created.
 * Mirrors smoke_glass_refraction.cpp for these conventions.
 *
 * The (orig, new) accept-iff-equal decision over the full integer space is owned
 * by the host test (prop_mesh_store_count). This case pins the live
 * behaviour: an Ok on a matching count, an Error on a too-small and a too-large
 * count, and a benign no-op on an unknown handle. It also rejects changes to
 * bind-mesh UV0 or displacement-normal magnitude, then accepts a valid
 * deformation after each rejection.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 */

#include "smoke_cases.h"

#include "rhi/mesh_store.h"
#include "rhi/rhi.h"
#include "engine/types.h"

#include <filament/Engine.h>

#include <cstdio>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_mesh_store]";

// A minimal valid single-surface mesh: three vertices forming one triangle.
// upload() needs non-empty vertices + indices to create the VB/IB and record
// the vertex count.
Mesh make_triangle_mesh() {
    Mesh mesh;
    mesh.name = "smoke_tri";
    mesh.path = "smoke_tri.mesh";

    Surface surf;
    surf.name = "tri";
    surf.vertices = {
        // px py pz   nx ny nz   u v   tangent(xyzw)
        {0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f,  1, 0, 0, 1},
        {1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f,  1, 0, 0, 1},
        {0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f,  1, 0, 0, 1},
    };
    surf.indices = {0, 1, 2};
    mesh.surfaces.push_back(std::move(surf));
    return mesh;
}

} // namespace

CaseResult run_mesh_store_case(const CaseContext& /*ctx*/) {
    filament::Engine* engine =
        filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) {
        std::printf("%s SKIP: could not create a NOOP Filament engine\n", kTag);
        return CaseResult::Skip;
    }

    bool ok   = true;
    bool skip = false;
    const auto resource_counts = [&] {
        return std::tuple{engine->getVertexBufferCount(), engine->getIndexBufferCount(),
                          engine->getBufferObjectCount()};
    };
    const auto baseline = resource_counts();

    {
        MeshStore store(*engine);

        const Mesh mesh = make_triangle_mesh();
        const GpuMesh gm = store.upload(mesh);

        if (gm.surfaces.empty()) {
            std::printf("%s SKIP: upload produced no surfaces on NOOP — cannot "
                        "assert update_vertices against a real buffer\n", kTag);
            skip = true;
        } else {
            const uint64_t handle = gm.surfaces[0].handle;
            const uint32_t orig_count =
                static_cast<uint32_t>(mesh.surfaces[0].vertices.size());
            const std::vector<Vertex>& verts = mesh.surfaces[0].vertices;

            // ──: matching count ⇒ Ok (valid CPU-skinning re-upload) ──
            if (auto r = store.update_vertices(handle, verts.data(), orig_count); !r) {
                std::printf("%s FAIL: matching-count re-upload (%u) was rejected "
                            "(%s) — the valid skinning path must re-upload "
                            "(R10.3)\n", kTag, orig_count, r.error().message.c_str());
                ok = false;
            }

            // ──: too-small count ⇒ Error, no buffer write ──
            if (auto r = store.update_vertices(handle, verts.data(), orig_count - 1); r) {
                std::printf("%s FAIL: a too-small count (%u vs %u) was accepted "
                            "— a mismatch must be rejected with an Error before "
                            "it corrupts the buffer (R10.1/R10.2)\n", kTag,
                            orig_count - 1, orig_count);
                ok = false;
            }

            // ──: too-large count ⇒ Error, no buffer write ──
            if (auto r = store.update_vertices(handle, verts.data(), orig_count + 5); r) {
                std::printf("%s FAIL: a too-large count (%u vs %u) was accepted "
                            "— a mismatch must be rejected with an Error "
                            "(R10.1/R10.2)\n", kTag, orig_count + 5, orig_count);
                ok = false;
            }

            // A matching count does not authorize changes to the bind-mesh
            // attributes that determine the immutable displacement footprint.
            // Exercise both rejected attributes, with a valid deformation after
            // each failure so a rejection cannot poison subsequent updates.
            for (const bool change_magnitude : {false, true}) {
                auto changed = verts;
                if (change_magnitude) {
                    changed[0].displacement_normal_scale += 0.25f;
                } else {
                    changed[0].u += 0.25f;
                }
                const char* attribute = change_magnitude
                    ? "displacement-normal magnitude" : "UV0";
                const auto rejected = store.update_vertices(
                    handle, changed.data(), orig_count);
                if (rejected || rejected.error().message.find(
                        "a full mesh upload is required") == std::string::npos) {
                    std::printf("%s FAIL: changing %s at the same vertex count "
                                "did not return the required full-upload error\n",
                                kTag, attribute);
                    ok = false;
                }

                auto deformed = verts;
                for (auto& vertex : deformed) vertex.pz += 0.125f;
                if (const auto accepted = store.update_vertices(
                        handle, deformed.data(), orig_count); !accepted) {
                    std::printf("%s FAIL: valid deformation after rejecting %s "
                                "failed (%s)\n", kTag, attribute,
                                accepted.error().message.c_str());
                    ok = false;
                }
            }

            // ── Unknown handle ⇒ benign no-op (Ok) ──────────────────
            // find-miss path: no surface to corrupt, so it returns Ok without
            // touching anything.
            if (auto r = store.update_vertices(handle + 9999, verts.data(), orig_count); !r) {
                std::printf("%s FAIL: an unknown handle returned an Error "
                            "(%s) — it should be a benign no-op\n", kTag,
                            r.error().message.c_str());
                ok = false;
            }

            // Replacements must retire actual Filament owners, not just hide
            // their handle records. Keep a neighboring mesh alive throughout
            // to catch an accidental whole-store reset during replacement.
            const auto neighbor = store.upload(mesh);
            const auto neighbor_handle = neighbor.surfaces.front().handle;
            auto current = gm;
            const auto two_meshes = resource_counts();
            for (int iteration = 0; iteration < 96; ++iteration) {
                auto replacement = store.upload(mesh);
                const auto retired_handle = current.surfaces.front().handle;
                store.release(current);
                store.release(current);
                const auto retired = store.find(retired_handle);
                const auto retained = store.find(neighbor_handle);
                const auto live_handle = replacement.surfaces.front().handle;
                const auto live = store.find(live_handle);
                if (retired.vb || retired.ib || !retained.vb || !retained.ib ||
                        !live.vb || !live.ib || resource_counts() != two_meshes ||
                        !store.update_vertices(live_handle, verts.data(), orig_count)) {
                    std::printf("%s FAIL: replacement %d retained retired owners or "
                                "invalidated a live/neighbor mesh\n", kTag, iteration);
                    ok = false;
                    store.release(replacement);
                    break;
                }
                current = std::move(replacement);
            }
            store.release(current);
            store.release(neighbor);
            GpuMesh unknown;
            unknown.surfaces.push_back({handle + 999999, 3, "unknown"});
            store.release(unknown);
            if (resource_counts() != baseline) {
                std::printf("%s FAIL: released static meshes did not return "
                            "Filament resource counts to baseline\n", kTag);
                ok = false;
            }

            // GPU-skinned owners carry extra buffer objects and metadata. They
            // must obey the same retirement contract, including erasing the
            // skinning marker without disturbing a separate static owner.
            SkinnedMesh skin;
            skin.name = "smoke_skin_release";
            skin.path = "smoke_skin_release.smesh";
            skin.bones.push_back({"root", -1});
            SkinnedSurface skin_surface;
            skin_surface.name = "tri";
            skin_surface.indices = mesh.surfaces.front().indices;
            for (const auto& vertex : verts) {
                SkinnedVertex skinned{vertex.px, vertex.py, vertex.pz,
                    vertex.nx, vertex.ny, vertex.nz, vertex.u, vertex.v,
                    vertex.u1, vertex.v1};
                skinned.bone_weights[0] = 1.0f;
                skin_surface.vertices.push_back(skinned);
            }
            skin.surfaces.push_back(std::move(skin_surface));
            const auto static_neighbor = store.upload(mesh);
            const auto one_mesh = resource_counts();
            for (int iteration = 0; iteration < 32; ++iteration) {
                const auto skinned = store.upload_skinned(skin);
                const auto skinned_handle = skinned.surfaces.front().handle;
                const auto live = store.skinned_info(skinned_handle);
                store.release(skinned);
                const auto retired = store.find(skinned_handle);
                const auto marker = store.skinned_info(skinned_handle);
                const auto retained = store.find(static_neighbor.surfaces.front().handle);
                if (!live.skinned || live.bone_count != 1 || marker.skinned ||
                        marker.bone_count || retired.vb || retired.ib ||
                        !retained.vb || !retained.ib || resource_counts() != one_mesh) {
                    std::printf("%s FAIL: skinned release %d retained owners/metadata "
                                "or invalidated the static neighbor\n", kTag, iteration);
                    ok = false;
                    break;
                }
            }
            store.release(static_neighbor);
            if (resource_counts() != baseline) {
                std::printf("%s FAIL: skinned releases did not return Filament "
                            "resource counts to baseline\n", kTag);
                ok = false;
            }
        }
        // MeshStore destructor destroys the VB/IB here (before the engine).
    }

    filament::Engine::destroy(&engine);

    if (skip) return CaseResult::Skip;
    if (!ok)  return CaseResult::Fail;

    std::printf("%s update_vertices honored the recorded count against a real "
                "uploaded buffer: matching count ⇒ Ok (R10.3); too-small and "
                "too-large counts ⇒ Error with no buffer write (R10.1/R10.2); "
                "changed UV0/normal magnitude ⇒ full-upload Error, subsequent "
                "valid deformation ⇒ Ok; unknown handle ⇒ benign no-op; "
                "96 replacements and 32 skinned retirements preserve neighbors "
                "and return real Filament owner counts to baseline\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
