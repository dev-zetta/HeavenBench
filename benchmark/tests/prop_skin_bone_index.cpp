/*
 * HeavenBench — Property test: skinning never indexes out of range
 *
 * Feature: codebase-hardening
 *   •: Skinning never indexes out of range
 *
 *
 * The CPU linear-blend-skinning code (engine/skinned_mesh.cpp::skin_frame and
 * skin_interpolated) reads a per-vertex bone index for each of the vertex's
 * up to four weight slots and uses it to index the precomputed `skin_mat`
 * array, which is sized to `num_bones`. A corrupt or hostile ms08 file can hold
 * a bone index that is negative or ≥ num_bones while still carrying a positive
 * weight.  added the guard
 *
 *     if (weight <= 0.0f) continue;
 *     int bi = sv.bone_indices[w];
 *     if (!loadval::in_range(bi, 0, num_bones - 1)) continue;
 *
 * so an out-of-range index with positive weight is DROPPED rather than used to
 * index `skin_mat` out of bounds, and the vertex continues to
 * be skinned using only the remaining in-range weights.
 *
 * This property pins both halves of that contract by constructing SkinnedMesh
 * data entirely in memory (no file I/O needed): a small valid bone hierarchy
 * (a -1-rooted parent chain), non-identity per-frame bone transforms so the
 * skinning matrices genuinely move vertices, and a surface whose vertices carry
 * a mix of in-range, out-of-range (negative AND ≥ num_bones), and empty weight
 * slots — all the non-empty slots carrying weight > 0.
 *
 * For each fuzzed vertex we build it twice in lockstep:
 *   • the "with_oob" vertex keeps its out-of-range slots (weight > 0); and
 *   • the "cleaned" reference vertex is byte-identical except every
 *     out-of-range slot has its weight forced to 0, so those slots are skipped
 *     by the `weight <= 0` test alone.
 * Because the in-range slots sit at the same positions with the same bone
 * indices and weights, both meshes accumulate exactly the same terms in the
 * same order through the same `skin_mat`. The property therefore asserts:
 *   1. skin_frame / skin_interpolated return without an out-of-bounds access
 *      (enforced when the suite is built under AddressSanitizer) —;
 *   2. every output component is finite (no index read past skin_mat produced
 *      garbage / NaN); and
 *   3. the with_oob output equals the in-range-only (cleaned) output, i.e. the
 *      out-of-range contributions were dropped —. In particular a
 *      vertex weighted entirely by out-of-range bones skins to the origin,
 *      exactly as its all-weights-dropped reference does.
 *
 * skin_frame / skin_interpolated are GPU-free and host-testable. This file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 */
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
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

// A single weight slot to install into a SkinnedVertex. `oob` marks slots whose
// bone index is deliberately out of range; the cleaned reference zeroes their
// weight so only the in-range slots survive.
struct Slot {
    int   bone_index = 0;
    float weight = 0.0f;
    bool  oob = false;
};

// Generate a small fuzzed value in [-2, 2] for bone/vertex coordinates.
float gen_coord() {
    return static_cast<float>(*rc::gen::inRange(-200, 200)) * 0.01f;
}

// Generate a unit quaternion from fuzzed components (RapidCheck-driven), with a
// safe fallback to identity if the random draw is degenerate.
glm::quat gen_unit_quat() {
    glm::quat q(static_cast<float>(*rc::gen::inRange(-100, 100)) * 0.01f,
                static_cast<float>(*rc::gen::inRange(-100, 100)) * 0.01f,
                static_cast<float>(*rc::gen::inRange(-100, 100)) * 0.01f,
                static_cast<float>(*rc::gen::inRange(-100, 100)) * 0.01f);
    const float len = glm::length(q);
    if (len < 1e-4f) return {1.0f, 0.0f, 0.0f, 0.0f};
    return q / len;
}

// Build a valid bone hierarchy: bone 0 is the root (parent -1); every later
// bone's parent is some EARLIER bone, guaranteeing an acyclic, in-range
// skeleton so the world-transform walk reaches the root cleanly.
std::vector<heaven::Bone> gen_bones(int num_bones) {
    std::vector<heaven::Bone> bones(static_cast<std::size_t>(num_bones));
    bones[0].parent = -1;
    for (int b = 1; b < num_bones; ++b) {
        bones[static_cast<std::size_t>(b)].parent = *rc::gen::inRange(-1, b - 1);
    }
    return bones;
}

// Build `num_frames` worth of per-bone transforms with non-identity
// translation, scale, and rotation so the skinning matrices genuinely move the
// bind pose (otherwise every skin_mat would be the identity and the test would
// be trivially satisfied).
std::vector<heaven::BoneFrame> gen_frames(int num_bones, int num_frames) {
    std::vector<heaven::BoneFrame> frames(
        static_cast<std::size_t>(num_bones) *
        static_cast<std::size_t>(num_frames));
    for (auto& bf : frames) {
        const float scale =
            0.5f + static_cast<float>(*rc::gen::inRange(0, 100)) * 0.01f;
        bf.position = glm::vec4(gen_coord(), gen_coord(), gen_coord(), scale);
        bf.rotation = gen_unit_quat();
    }
    return frames;
}

// Generate the four weight slots for one vertex. Each slot is independently
// chosen to be empty (weight 0), in-range (a real bone with weight > 0), or
// out-of-range (a negative or ≥ num_bones index, still with weight > 0). All
// out-of-range slots must be dropped by the skinning guard.
std::array<Slot, 4> gen_slots(int num_bones) {
    std::array<Slot, 4> slots{};
    for (auto& s : slots) {
        const int kind = *rc::gen::inRange(0, 3); // 0 empty, 1 in-range, 2 oob
        if (kind == 0) {
            s = Slot{0, 0.0f, false};
        } else if (kind == 1) {
            s.bone_index = *rc::gen::inRange(0, num_bones - 1);
            s.weight = static_cast<float>(*rc::gen::inRange(1, 100)) * 0.01f;
            s.oob = false;
        } else {
            // Half the OOB slots use a negative index, half use one ≥ num_bones,
            // covering both directions the in_range(bi, 0, num_bones-1) guard
            // must reject.
            if (*rc::gen::inRange(0, 2) == 0) {
                s.bone_index = *rc::gen::inRange(-100, 0); // negative
            } else {
                s.bone_index = num_bones + *rc::gen::inRange(0, 100); // ≥ num_bones
            }
            s.weight = static_cast<float>(*rc::gen::inRange(1, 100)) * 0.01f;
            s.oob = true;
        }
    }
    return slots;
}

// Construct a vertex with the given position and weight slots. When `cleaned`
// is true, out-of-range slots have their weight forced to 0 so the reference
// vertex carries ONLY the in-range contributions.
heaven::SkinnedVertex make_vertex(float px, float py, float pz,
                                  const std::array<Slot, 4>& slots,
                                  bool cleaned) {
    heaven::SkinnedVertex sv;
    sv.px = px; sv.py = py; sv.pz = pz;
    sv.nx = 0.0f; sv.ny = 1.0f; sv.nz = 0.0f;
    sv.u = 0.25f; sv.v = 0.75f;
    for (int w = 0; w < 4; ++w) {
        sv.bone_indices[w] = slots[static_cast<std::size_t>(w)].bone_index;
        const float weight = slots[static_cast<std::size_t>(w)].weight;
        sv.bone_weights[w] =
            (cleaned && slots[static_cast<std::size_t>(w)].oob) ? 0.0f : weight;
    }
    return sv;
}

bool finite_vertex(const heaven::Vertex& v) {
    return std::isfinite(v.px) && std::isfinite(v.py) && std::isfinite(v.pz) &&
           std::isfinite(v.nx) && std::isfinite(v.ny) && std::isfinite(v.nz);
}

// The in-range and out-of-range skin paths accumulate exactly the same terms in
// the same order through the same skin_mat, so the position/normal output is
// bit-identical. Compare exactly.
bool same_skin(const heaven::Vertex& a, const heaven::Vertex& b) {
    return a.px == b.px && a.py == b.py && a.pz == b.pz &&
           a.nx == b.nx && a.ny == b.ny && a.nz == b.nz;
}

// Build a (with_oob, cleaned) pair of SkinnedMeshes sharing an identical
// skeleton and frame set; they differ only in whether each vertex's
// out-of-range weight slots retain their weight. Returns the two meshes.
struct MeshPair {
    heaven::SkinnedMesh with_oob;
    heaven::SkinnedMesh cleaned;
};

MeshPair gen_mesh_pair(int num_bones, int num_frames, int num_verts) {
    const std::vector<heaven::Bone> bones = gen_bones(num_bones);
    const std::vector<heaven::BoneFrame> frames =
        gen_frames(num_bones, num_frames);

    MeshPair mp;
    for (heaven::SkinnedMesh* m : {&mp.with_oob, &mp.cleaned}) {
        m->name = "fuzz";
        m->path = "memory://fuzz.ms08";
        m->bones = bones;
        m->num_frames = num_frames;
        m->frames = frames;
        m->surfaces.resize(1);
        m->surfaces[0].name = "surf_0";
        m->surfaces[0].vertices.reserve(static_cast<std::size_t>(num_verts));
    }

    // Build a simple triangle-list index buffer over the vertices so
    // compute_tangents (run on the skinned output) has triangles to iterate.
    std::vector<std::uint32_t> indices;
    for (int v = 0; v + 2 < num_verts; ++v) {
        indices.push_back(static_cast<std::uint32_t>(v));
        indices.push_back(static_cast<std::uint32_t>(v + 1));
        indices.push_back(static_cast<std::uint32_t>(v + 2));
    }
    mp.with_oob.surfaces[0].indices = indices;
    mp.cleaned.surfaces[0].indices = indices;

    for (int v = 0; v < num_verts; ++v) {
        const float px = gen_coord();
        const float py = gen_coord();
        const float pz = gen_coord();
        const std::array<Slot, 4> slots = gen_slots(num_bones);
        mp.with_oob.surfaces[0].vertices.push_back(
            make_vertex(px, py, pz, slots, /*cleaned=*/false));
        mp.cleaned.surfaces[0].vertices.push_back(
            make_vertex(px, py, pz, slots, /*cleaned=*/true));
    }

    return mp;
}

// skin_frame: out-of-range bone indices with positive weights are dropped, the
// remaining in-range weights still skin the vertex, no OOB access, output finite.
bool run_p3_skin_frame() {
    return rc::check(
        "Feature: codebase-hardening, Property 3: skinning never indexes out of "
        "range — skin_frame drops out-of-range weighted bones (Validates: "
        "Requirements 2.1, 2.2)",
        [] {
            const int num_bones = *rc::gen::inRange(1, 6);
            const int num_frames = *rc::gen::inRange(1, 4);
            const int num_verts = *rc::gen::inRange(1, 16);
            const int frame = *rc::gen::inRange(0, num_frames);

            const MeshPair mp = gen_mesh_pair(num_bones, num_frames, num_verts);

            const std::vector<heaven::Vertex> got =
                mp.with_oob.skin_frame(0, frame);
            const std::vector<heaven::Vertex> ref =
                mp.cleaned.skin_frame(0, frame);

            RC_ASSERT(got.size() == static_cast<std::size_t>(num_verts));
            RC_ASSERT(got.size() == ref.size());
            for (std::size_t i = 0; i < got.size(); ++i) {
                RC_ASSERT(finite_vertex(got[i]));
                RC_ASSERT(same_skin(got[i], ref[i]));
            }
        });
}

// skin_interpolated: same contract on the interpolation path. num_frames ≥ 2
// and a fractional frame index exercise the lerp/slerp + interp_world walk.
bool run_p3_skin_interpolated() {
    return rc::check(
        "Feature: codebase-hardening, Property 3: skinning never indexes out of "
        "range — skin_interpolated drops out-of-range weighted bones "
        "(Validates: Requirements 2.1, 2.2)",
        [] {
            const int num_bones = *rc::gen::inRange(1, 6);
            const int num_frames = *rc::gen::inRange(2, 5);
            const int num_verts = *rc::gen::inRange(1, 16);
            // A fractional frame index in [0, num_frames-1] to drive the
            // interpolation branch (lerp position/scale, slerp rotation).
            const float frame_f =
                static_cast<float>(*rc::gen::inRange(0, (num_frames - 1) * 100)) *
                0.01f;

            const MeshPair mp = gen_mesh_pair(num_bones, num_frames, num_verts);

            const std::vector<heaven::Vertex> got =
                mp.with_oob.skin_interpolated(0, frame_f);
            const std::vector<heaven::Vertex> ref =
                mp.cleaned.skin_interpolated(0, frame_f);

            RC_ASSERT(got.size() == static_cast<std::size_t>(num_verts));
            RC_ASSERT(got.size() == ref.size());
            for (std::size_t i = 0; i < got.size(); ++i) {
                RC_ASSERT(finite_vertex(got[i]));
                RC_ASSERT(same_skin(got[i], ref[i]));
            }
        });
}

// A focused check that a vertex weighted ENTIRELY by out-of-range bones skins
// to the origin (all contributions dropped → accumulator stays zero), making
// the "out-of-range weights are dropped" guarantee explicit rather than implied
// by the cleaned-reference equality above.
bool run_p3_all_oob_to_origin() {
    return rc::check(
        "Feature: codebase-hardening, Property 3: skinning never indexes out of "
        "range — all-out-of-range vertex skins to the origin (Validates: "
        "Requirements 2.1, 2.2)",
        [] {
            const int num_bones = *rc::gen::inRange(1, 6);
            const int num_frames = *rc::gen::inRange(1, 4);
            const int frame = *rc::gen::inRange(0, num_frames);

            const std::vector<heaven::Bone> bones = gen_bones(num_bones);
            const std::vector<heaven::BoneFrame> frames =
                gen_frames(num_bones, num_frames);

            heaven::SkinnedMesh m;
            m.bones = bones;
            m.num_frames = num_frames;
            m.frames = frames;
            m.surfaces.resize(1);
            m.surfaces[0].name = "surf_0";

            heaven::SkinnedVertex sv;
            sv.px = gen_coord(); sv.py = gen_coord(); sv.pz = gen_coord();
            sv.nx = 0.0f; sv.ny = 1.0f; sv.nz = 0.0f;
            sv.u = 0.0f; sv.v = 0.0f;
            for (int w = 0; w < 4; ++w) {
                // All four slots reference bones outside [0, num_bones) with a
                // positive weight, so every contribution must be dropped.
                sv.bone_indices[w] = num_bones + w; // ≥ num_bones
                sv.bone_weights[w] = 0.25f;
            }
            m.surfaces[0].vertices.push_back(sv);

            const std::vector<heaven::Vertex> got = m.skin_frame(0, frame);
            RC_ASSERT(got.size() == 1);
            RC_ASSERT(finite_vertex(got[0]));
            // No in-range weight contributed → the accumulator stayed at zero.
            RC_ASSERT(got[0].px == 0.0f);
            RC_ASSERT(got[0].py == 0.0f);
            RC_ASSERT(got[0].pz == 0.0f);
        });
}

bool run_skin_bone_index_properties() {
    bool ok = true;
    ok = run_p3_skin_frame() && ok;
    ok = run_p3_skin_interpolated() && ok;
    ok = run_p3_all_oob_to_origin() && ok;
    return ok;
}

const heaven::testing::Register g_register_skin_bone_index{
    &run_skin_bone_index_properties};

} // namespace
