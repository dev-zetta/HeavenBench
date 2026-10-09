/*
 * HeavenBench — Property test: GPU bone-matrix LBS equals the CPU skin
 *
 * Feature: gpu-instancing-and-skinning,: GPU bone-matrix LBS equals the CPU skin result
 *   • Validates:
 *
 * GPU skinning uploads each Animated_Object's rest-pose vertices once and skins
 * them on the GPU from per-frame bone matrices. Those matrices come from the new
 * pure helper engine/skinned_mesh.cpp::bone_matrices_interpolated(frame_f), the
 * GPU-skinning counterpart of skin_interpolated:
 *
 *     bone_matrices[b] = bone_world(frame_f, b) * inverse(bone_world(bind, b))
 *
 * Filament then computes, for every vertex, the standard linear-blend-skinning
 * (LBS) sum
 *
 *     skinned_position = Σ wᵢ · bone_matrices[idxᵢ] · v
 *
 * over the rest-pose vertex v and its (index, weight) influences. Requirement
 * 4.3 / 5.3 require that this GPU LBS result match the CPU_Skinning_Path
 * (skin_interpolated) pose for the same animation frame within visual tolerance.
 *
 * This property pins exactly that equivalence on the pure CPU side (both halves
 * are GPU-free and host-testable): for random rest-pose vertices, bone weights,
 * bone indices and (possibly fractional) frames it evaluates the LBS sum with
 * the matrices bone_matrices_interpolated returns and asserts it equals the
 * position skin_interpolated produces for the same frame_f, within a small
 * float tolerance. Because both paths build their per-bone matrices from the
 * SAME bind reference (frame 0) and the SAME interpolation (lerp position/scale,
 * slerp rotation), the two results coincide up to floating-point rounding.
 *
 * The skeleton/frames/weights are constructed entirely in memory (no file I/O),
 * mirroring the sibling skinned-mesh property tests (prop_skin_bone_index.cpp,
 * prop_bone_chain_terminates.cpp). This file self-registers with the RapidCheck
 * harness in test_main.cpp via the heaven::testing::Register pattern, so the
 * file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the entry
 * point.
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

// Generate a small fuzzed value in [-2, 2] for bone/vertex coordinates.
float gen_coord() {
    return static_cast<float>(*rc::gen::inRange(-200, 200)) * 0.01f;
}

// Generate a unit quaternion from fuzzed components, with a safe fallback to
// identity if the random draw is degenerate.
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
// translation, near-unit scale, and rotation so the bone matrices genuinely
// move the bind pose (otherwise every matrix would be identity and the test
// would be trivially satisfied).
std::vector<heaven::BoneFrame> gen_frames(int num_bones, int num_frames) {
    std::vector<heaven::BoneFrame> frames(
        static_cast<std::size_t>(num_bones) *
        static_cast<std::size_t>(num_frames));
    for (auto& bf : frames) {
        // Keep scale near 1 (0.7..1.3) so composed world matrices and the LBS
        // sum stay modestly scaled and the float tolerance below is meaningful.
        const float scale =
            0.7f + static_cast<float>(*rc::gen::inRange(0, 60)) * 0.01f;
        bf.position = glm::vec4(gen_coord(), gen_coord(), gen_coord(), scale);
        bf.rotation = gen_unit_quat();
    }
    return frames;
}

// Generate up to four in-range weight slots for one vertex. Slot 0 always
// carries a positive weight on a real bone so the vertex is genuinely skinned;
// the remaining slots are independently empty or in-range. All non-empty slots
// reference bones in [0, num_bones) with weight > 0, matching the contract of
// the real ms08 loader (which drops/normalises weights). The CPU and GPU paths
// therefore accumulate exactly the same set of influences.
heaven::SkinnedVertex gen_vertex(int num_bones) {
    heaven::SkinnedVertex sv{};
    sv.px = gen_coord(); sv.py = gen_coord(); sv.pz = gen_coord();
    sv.nx = 0.0f; sv.ny = 1.0f; sv.nz = 0.0f;
    sv.u = 0.25f; sv.v = 0.75f;
    for (int w = 0; w < 4; ++w) {
        const bool active = (w == 0) || (*rc::gen::inRange(0, 2) == 0);
        if (active) {
            sv.bone_indices[w] = *rc::gen::inRange(0, num_bones - 1);
            sv.bone_weights[w] =
                static_cast<float>(*rc::gen::inRange(1, 100)) * 0.01f;
        } else {
            sv.bone_indices[w] = 0;
            sv.bone_weights[w] = 0.0f;
        }
    }
    return sv;
}

// Evaluate the GPU linear-blend-skinning sum Σ wᵢ · bone_matrices[idxᵢ] · v for
// a single rest-pose vertex, mirroring the influence selection skin_interpolated
// applies (skip non-positive weights and out-of-range bone indices) so the two
// paths sum the identical set of terms.
glm::vec3 gpu_lbs_position(const heaven::SkinnedVertex& sv,
                           const std::vector<glm::mat4>& bone_matrices) {
    const int num_bones = static_cast<int>(bone_matrices.size());
    const glm::vec4 pos(sv.px, sv.py, sv.pz, 1.0f);
    glm::vec4 acc(0.0f);
    for (int w = 0; w < 4; ++w) {
        const float weight = sv.bone_weights[w];
        if (weight <= 0.0f) continue;
        const int bi = sv.bone_indices[w];
        if (bi < 0 || bi >= num_bones) continue;
        acc += weight * (bone_matrices[static_cast<std::size_t>(bi)] * pos);
    }
    return {acc.x, acc.y, acc.z};
}

// Per-component closeness with a combined absolute + relative tolerance. The
// two paths build their bone matrices from the same operations, so the only
// divergence is float accumulation order; a tight tolerance is appropriate.
bool close(const glm::vec3& a, const glm::vec3& b) {
    for (int i = 0; i < 3; ++i) {
        const float tol = 1e-3f + 1e-3f * std::max(std::abs(a[i]), std::abs(b[i]));
        if (std::abs(a[i] - b[i]) > tol) return false;
    }
    return true;
}

bool run_p2_bone_matrix_lbs_equivalence() {
    return rc::check(
        "Feature: gpu-instancing-and-skinning, Property 2: GPU bone-matrix LBS "
        "equals the CPU skin result (Validates: Requirements 4.3, 5.3)",
        [] {
            const int num_bones = *rc::gen::inRange(1, 6);
            // Cover both the single-frame (bind-only) early-out and the
            // multi-frame interpolation path.
            const int num_frames = *rc::gen::inRange(1, 6);
            const int num_verts = *rc::gen::inRange(1, 16);

            heaven::SkinnedMesh mesh;
            mesh.name = "lbs_fuzz";
            mesh.path = "memory://lbs_fuzz.ms08";
            mesh.bones = gen_bones(num_bones);
            mesh.num_frames = num_frames;
            mesh.frames = gen_frames(num_bones, num_frames);

            mesh.surfaces.resize(1);
            mesh.surfaces[0].name = "surf_0";
            mesh.surfaces[0].vertices.reserve(
                static_cast<std::size_t>(num_verts));
            for (int v = 0; v < num_verts; ++v) {
                mesh.surfaces[0].vertices.push_back(gen_vertex(num_bones));
            }
            // A simple triangle-list index buffer so the CPU path's tangent
            // pass (run over the skinned output) has triangles to iterate; it
            // does not affect the skinned positions we compare.
            for (int v = 0; v + 2 < num_verts; ++v) {
                mesh.surfaces[0].indices.push_back(
                    static_cast<std::uint32_t>(v));
                mesh.surfaces[0].indices.push_back(
                    static_cast<std::uint32_t>(v + 1));
                mesh.surfaces[0].indices.push_back(
                    static_cast<std::uint32_t>(v + 2));
            }

            // A (possibly fractional) frame index across the full valid range,
            // exercising the integer-frame early-outs and the lerp/slerp branch.
            const float frame_f =
                num_frames <= 1
                    ? 0.0f
                    : static_cast<float>(
                          *rc::gen::inRange(0, (num_frames - 1) * 100)) *
                          0.01f;

            // CPU reference pose and GPU per-bone matrices for the SAME frame.
            const std::vector<heaven::Vertex> cpu =
                mesh.skin_interpolated(0, frame_f);
            const std::vector<glm::mat4> bone_matrices =
                mesh.bone_matrices_interpolated(frame_f);

            RC_ASSERT(cpu.size() == static_cast<std::size_t>(num_verts));
            RC_ASSERT(bone_matrices.size() ==
                      static_cast<std::size_t>(num_bones));

            for (int v = 0; v < num_verts; ++v) {
                const heaven::SkinnedVertex& sv =
                    mesh.surfaces[0].vertices[static_cast<std::size_t>(v)];
                const glm::vec3 gpu = gpu_lbs_position(sv, bone_matrices);
                const heaven::Vertex& c = cpu[static_cast<std::size_t>(v)];
                const glm::vec3 cpu_pos(c.px, c.py, c.pz);
                RC_ASSERT(close(gpu, cpu_pos));
            }
        });
}

const heaven::testing::Register g_register_bone_matrices{
    &run_p2_bone_matrix_lbs_equivalence};

} // namespace
