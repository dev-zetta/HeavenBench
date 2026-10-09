/*
 * HeavenBench — Property test: parent-chain walk terminates
 *
 * Feature: codebase-hardening
 *   •: Parent-chain walk terminates
 *
 *
 * The skinned-mesh world-transform walk (engine/skinned_mesh.cpp) traverses a
 * bone's parent chain to compose its world-space transform. A corrupt ms08 file
 * can store a parent array that forms a cycle (a self-parent, or a mutual
 * A→B→A link). The OLD unbounded `while (parent >= 0)` walk would loop forever
 * on such data and hang the renderer.  replaced it with the bounded
 * `walk_to_root` helper that traverses at most `num_bones` parent links before
 * terminating and signalling a cycle, used by both `bone_world_transform` and
 * the `interp_world` lambda in `skin_interpolated`.
 *
 * pin the termination guarantee: for ANY parent array,
 * including one that contains a cycle, the walk SHALL complete in at most
 * `num_bones` parent-link traversal steps and return. This property exercises
 * that contract directly:
 *
 *   • Build a SkinnedMesh with a random number of bones whose `parent` links
 *     are drawn from [-1, num_bones) — which already produces cycles freely —
 *     and additionally inject a guaranteed cycle (a self-parent, or a 2-cycle)
 *     on a subset of runs so the cyclic case is reliably covered.
 *   • Give the mesh at least one frame so the per-bone frame lookup is real
 *     (otherwise `get_frame` short-circuits to identity and the chain is never
 *     walked).
 *   • Call `bone_world_transform(b, 0)` for every bone, and `skin_frame`/
 *     `skin_interpolated` (which walk chains for every bone internally).
 *
 * The mere fact that the test *completes* is the termination proof: were the
 * walk unbounded on a cyclic array, this test would hang and the suite would
 * time out rather than fail. As an additional invariant, with finite,
 * modestly-scaled bone frames the composed world matrix stays finite (the walk
 * composes at most `num_bones + 1` finite matrices), which this property also
 * asserts for `bone_world_transform`.
 *
 * SkinnedMesh and its transform/skinning methods are GPU-free and host-testable.
 * This file self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/CMakeLists.txt
 * picks it up without editing the entry point.
 */
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

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

// True iff every element of the 4×4 matrix is a finite float (no NaN/Inf).
bool mat4_is_finite(const glm::mat4& m) {
    for (int column = 0; column < 4; ++column) {
        const auto& v = m[column];
        if (!std::isfinite(v.x) || !std::isfinite(v.y) ||
            !std::isfinite(v.z) || !std::isfinite(v.w)) return false;
    }
    return true;
}

// Build a SkinnedMesh with `num_bones` bones, `num_frames` frames of modestly
// scaled bone transforms, and the supplied parent array (which may contain
// cycles). A single one-vertex surface bound to bone 0 is added so the
// chain-walking skinning paths can also be exercised.
heaven::SkinnedMesh build_mesh(int num_bones, int num_frames,
                               const std::vector<int>& parents,
                               const std::vector<heaven::BoneFrame>& frames) {
    heaven::SkinnedMesh mesh;
    mesh.name = "cycle_test";
    mesh.path = "<in-memory>";
    mesh.num_frames = num_frames;

    mesh.bones.resize(static_cast<std::size_t>(num_bones));
    for (int b = 0; b < num_bones; ++b) {
        mesh.bones[b].name = "bone";
        mesh.bones[b].parent = parents[static_cast<std::size_t>(b)];
    }

    mesh.frames = frames;

    // One trivial surface: a single triangle referencing vertex 0, fully
    // weighted to bone 0. This drives skin_frame/skin_interpolated, which walk
    // the parent chain for every bone internally.
    heaven::SkinnedSurface surf;
    surf.name = "surf0";
    heaven::SkinnedVertex sv{};
    sv.px = sv.py = sv.pz = 0.0f;
    sv.nx = 0.0f; sv.ny = 1.0f; sv.nz = 0.0f;
    sv.u = sv.v = 0.0f;
    sv.bone_indices[0] = 0;
    sv.bone_weights[0] = 1.0f;
    surf.vertices.push_back(sv);
    surf.indices = {0, 0, 0};
    mesh.surfaces.push_back(std::move(surf));

    return mesh;
}

bool run_p5_parent_chain_terminates() {
    return rc::check(
        "Feature: codebase-hardening, Property 5: parent-chain walk terminates "
        "(Validates: Requirements 2.5, 2.6, 2.7)",
        [] {
            const int num_bones = *rc::gen::inRange(1, 32);

            // Random parent links in [-1, num_bones): this range alone produces
            // cycles freely (e.g. bone 0→1, bone 1→0). Parents are intentionally
            // unconstrained so the walk faces arbitrary topology.
            std::vector<int> parents(static_cast<std::size_t>(num_bones));
            for (int b = 0; b < num_bones; ++b) {
                parents[static_cast<std::size_t>(b)] =
                    *rc::gen::inRange(-1, num_bones - 1);
            }

            // Deterministically guarantee a cycle on a subset of runs so the
            // cyclic case is reliably exercised (not left to chance):
            //   • a self-parent (bone k → k), or
            //   • a 2-cycle (bone j → k, bone k → j).
            const int cycle_mode = *rc::gen::inRange(0, 3); // 0,1,2
            if (cycle_mode == 1) {
                const int k = *rc::gen::inRange(0, num_bones - 1);
                parents[static_cast<std::size_t>(k)] = k; // self-parent
            } else if (cycle_mode == 2 && num_bones >= 2) {
                const int j = *rc::gen::inRange(0, num_bones - 1);
                int k = *rc::gen::inRange(0, num_bones - 1);
                if (k == j) k = (j + 1) % num_bones;
                parents[static_cast<std::size_t>(j)] = k;
                parents[static_cast<std::size_t>(k)] = j; // mutual 2-cycle
            }

            // At least one frame so get_frame returns a real per-bone transform
            // (with zero frames it short-circuits and the chain is never walked).
            const int num_frames = *rc::gen::inRange(1, 4);

            // Modestly-scaled finite bone frames: small translations, near-unit
            // scale, and a normalised rotation. Composing up to num_bones+1 such
            // matrices stays finite, so the finiteness invariant is meaningful.
            const std::size_t total =
                static_cast<std::size_t>(num_bones) *
                static_cast<std::size_t>(num_frames);
            std::vector<heaven::BoneFrame> frames(total);
            for (std::size_t i = 0; i < total; ++i) {
                const float tx = static_cast<float>(*rc::gen::inRange(-20, 20)) / 10.0f;
                const float ty = static_cast<float>(*rc::gen::inRange(-20, 20)) / 10.0f;
                const float tz = static_cast<float>(*rc::gen::inRange(-20, 20)) / 10.0f;
                const float scale = static_cast<float>(*rc::gen::inRange(8, 12)) / 10.0f; // 0.8..1.2
                const float ax = static_cast<float>(*rc::gen::inRange(-10, 10)) / 10.0f;
                const float ay = static_cast<float>(*rc::gen::inRange(-10, 10)) / 10.0f;
                const float az = static_cast<float>(*rc::gen::inRange(-10, 10)) / 10.0f;
                glm::quat q = glm::normalize(
                    glm::quat(1.0f, ax, ay, az)); // never degenerate (w=1)
                frames[i].position = glm::vec4(tx, ty, tz, scale);
                frames[i].rotation = q;
            }

            const heaven::SkinnedMesh mesh =
                build_mesh(num_bones, num_frames, parents, frames);

            // Termination: bone_world_transform must RETURN for every bone, even
            // when the parent array contains a cycle. Reaching the assertion
            // after the call IS the termination proof — an unbounded walk would
            // hang here. The result is also finite for these inputs.
            for (int b = 0; b < num_bones; ++b) {
                const glm::mat4 w = mesh.bone_world_transform(b, 0);
                RC_ASSERT(mat4_is_finite(w));
            }

            // The skinning paths walk the parent chain for every bone internally
            // (skin_frame directly; skin_interpolated via its interp_world
            // lambda). They must likewise complete and return one vertex.
            const std::vector<heaven::Vertex> skinned =
                mesh.skin_frame(0, 0);
            RC_ASSERT(skinned.size() == 1u);

            const std::vector<heaven::Vertex> interp =
                mesh.skin_interpolated(0, 0.5f);
            RC_ASSERT(interp.size() == 1u);
        });
}

const heaven::testing::Register g_register_bone_chain_terminates{
    &run_p5_parent_chain_terminates};

} // namespace
