/*
 * HeavenBench — Skinned mesh loading (ms08) and animation
 *
 * Parses the Unigine ms08 skinned mesh format, which stores a bone
 * hierarchy, per-frame bone transforms, and weighted vertex data.
 * Provides CPU-side skeletal animation via linear blend skinning (LBS).
 */
#ifndef HEAVENBENCH_SRC_ENGINE_SKINNED_MESH_H_
#define HEAVENBENCH_SRC_ENGINE_SKINNED_MESH_H_

#include "engine/asset_file_system.h"
#include "engine/types.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>

namespace heaven {

// ── Skeleton ────────────────────────────────────────────────────

struct Bone {
    std::string name;
    int parent = -1;
};

struct BoneFrame {
    glm::vec4 position; // xyz = position, w = uniform scale
    glm::quat rotation;
};

// ── Skinned vertex (CPU side, before skinning) ──────────────────

struct SkinnedVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float u1, v1;
    float displacement_normal_scale = 1.0f;
    // Bone weights (up to 4)
    int   bone_indices[4] = {0, 0, 0, 0};
    float bone_weights[4] = {0, 0, 0, 0};
};

// ── Skinned surface ─────────────────────────────────────────────

struct SkinnedSurface {
    std::string name;
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
};

// ── Skinned mesh (skeleton + animation + geometry) ──────────────

struct SkinnedMesh {
    std::string name;
    std::string path;
    std::vector<Bone> bones;
    int num_frames = 0;
    std::vector<BoneFrame> frames; // bones.size() * num_frames entries
    std::vector<SkinnedSurface> surfaces;

    //Get the transform for a bone at a given frame.
    BoneFrame get_frame(int bone, int frame) const;

    //Compute the world-space transform for a bone at a given frame
    //by walking the parent chain.
    glm::mat4 bone_world_transform(int bone, int frame) const;

    //Skin the mesh at a given frame, producing standard Vertex data
    //suitable for rendering through the existing pipeline.
    std::vector<Vertex> skin_frame(int surface, int frame) const;

    //Interpolate between two frames (lerp position/scale, slerp rotation)
    //and skin the result. frame_f is a floating-point frame index.
    std::vector<Vertex> skin_interpolated(int surface, float frame_f) const;

    //Compute the per-bone skinning matrices for an interpolated frame, the
    //GPU-skinning counterpart of skin_interpolated:
    //bone_matrices[b] = bone_world(frame_f, b) * inverse(bone_world(bind, b))
    //Uses the SAME bind reference (frame 0) and the SAME interpolation
    //(lerp position/scale, slerp rotation) as skin_interpolated, so a
    //rest-pose vertex v maps to Σ wᵢ · bone_matrices[idxᵢ] · v, equalling
    //skin_interpolated's position for the same frame_f. Returns one matrix
    //per bone (bones.size() entries).
    std::vector<glm::mat4> bone_matrices_interpolated(float frame_f) const;
};

// ── I/O ─────────────────────────────────────────────────────────

//Load a skinned mesh from an ms08 file.
[[nodiscard]] Result<SkinnedMesh> load_smesh(
    const std::string& path,
    AssetFileSystemRef assets = std::nullopt);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_SKINNED_MESH_H_
