/*
 * HeavenBench — Skinned mesh loading (ms08) and animation
 *
 * Implements the Unigine ms08 skinned mesh binary format parser and
 * CPU-side linear blend skinning (LBS) for skeletal animation.
 *
 * Format layout (little-endian):
 *   magic            int  'm'|('s'<<8)|('0'<<16)|('8'<<24)
 *   num_bones        int
 *   bones[num_bones] { name_len(int), name(char[]), parent(int) }
 *   num_frames       int
 *   frame_data[num_bones * num_frames] { pos_scale(float[4]), rot(float[4]) }
 *   num_surfaces     int
 *   surf_names[num_surfaces] { name_len(int), name(char[]) }
 *   surf_data[num_surfaces] { vertices, uv0, uv1, triangles }
 */
#include "engine/skinned_mesh.h"
#include "engine/loader_validation.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <memory>
#include <algorithm>
#include <array>
#include <numeric>
#include <functional>
#include <utility>

namespace heaven {
namespace {

// ── Tangent computation (MikkTSpace-style, same as mesh.cpp) ────

void compute_tangents(std::vector<Vertex>& verts,
                             const std::vector<uint32_t>& indices) {
    for (auto& v : verts) { v.tx = v.ty = v.tz = 0.0f; v.tw = 1.0f; }

    std::vector<float> bitan(verts.size() * 3, 0.0f);

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        auto& v0 = verts[indices[i]];
        auto& v1 = verts[indices[i + 1]];
        auto& v2 = verts[indices[i + 2]];

        float dx1 = v1.px - v0.px, dy1 = v1.py - v0.py, dz1 = v1.pz - v0.pz;
        float dx2 = v2.px - v0.px, dy2 = v2.py - v0.py, dz2 = v2.pz - v0.pz;
        float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
        float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;

        float r = du1 * dv2 - du2 * dv1;
        if (std::abs(r) < 1e-8f) continue;
        r = 1.0f / r;

        float sx = (dv2 * dx1 - dv1 * dx2) * r;
        float sy = (dv2 * dy1 - dv1 * dy2) * r;
        float sz = (dv2 * dz1 - dv1 * dz2) * r;
        float bx = (du1 * dx2 - du2 * dx1) * r;
        float by = (du1 * dy2 - du2 * dy1) * r;
        float bz = (du1 * dz2 - du2 * dz1) * r;

        for (int j = 0; j < 3; j++) {
            const size_t idx = indices[i + j];
            verts[idx].tx += sx; verts[idx].ty += sy; verts[idx].tz += sz;
            bitan[idx * 3]     += bx;
            bitan[idx * 3 + 1] += by;
            bitan[idx * 3 + 2] += bz;
        }
    }

    // Orthogonalize and normalize (Gram-Schmidt)
    for (size_t i = 0; i < verts.size(); i++) {
        auto& v = verts[i];
        float nx = v.nx, ny = v.ny, nz = v.nz;
        float tx = v.tx, ty = v.ty, tz = v.tz;

        float dot_nt = nx * tx + ny * ty + nz * tz;
        tx -= nx * dot_nt; ty -= ny * dot_nt; tz -= nz * dot_nt;
        float len = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (len > 1e-6f) { tx /= len; ty /= len; tz /= len; }
        else { tx = 1; ty = 0; tz = 0; }

        v.tx = tx; v.ty = ty; v.tz = tz;

        float cx = ny * tz - nz * ty;
        float cy = nz * tx - nx * tz;
        float cz = nx * ty - ny * tx;
        float dot_cb = cx * bitan[i * 3] + cy * bitan[i * 3 + 1] + cz * bitan[i * 3 + 2];
        v.tw = (dot_cb < 0.0f) ? -1.0f : 1.0f;
    }
}

// ── Helper: build a mat4 from a BoneFrame ───────────────────────

glm::mat4 bone_frame_to_mat4(const BoneFrame& bf) {
    float scale = bf.position.w;
    glm::mat4 m = glm::mat4_cast(bf.rotation);
    m[0] *= scale;
    m[1] *= scale;
    m[2] *= scale;
    m[3] = glm::vec4(bf.position.x, bf.position.y, bf.position.z, 1.0f);
    return m;
}

// ── Bounded parent-chain walk ───────────────────────────────────

// Emit the "parent chain contains a cycle" diagnostic at most once for the
// lifetime of the process so a single malformed skeleton cannot flood stderr
// (called once per skinned vertex/bone/frame otherwise).
void warn_parent_cycle_once(int bone) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::fprintf(stderr,
            "  WARNING: bone %d parent chain contains a cycle; "
            "terminating walk at num_bones steps\n", bone);
    }
}

// Walk bone → parent → … to the root, composing each bone's local frame
// (supplied by `local_of`) into world space. The walk is bounded to
// `num_bones` parent-link traversals so a cyclic parent array terminates
// instead of looping forever; on hitting the cap `cycle_out` is set true.
// For a valid acyclic skeleton the root is reached in fewer than
// `num_bones` steps, so the composed matrix is identical (parity).

glm::mat4 walk_to_root(int bone,
                              const std::function<glm::mat4(int)>& local_of,
                              const std::vector<Bone>& bones,
                              bool& cycle_out) {
    cycle_out = false;
    glm::mat4 result = local_of(bone);
    const int num_bones = static_cast<int>(bones.size());
    int parent = bones[bone].parent;
    int steps = 0;
    while (parent >= 0 && steps < num_bones) {
        result = local_of(parent) * result;
        parent = bones[parent].parent;
        ++steps;
    }
    // Still have a parent after num_bones traversals → the chain cannot be
    // acyclic; stop and signal the cycle to the caller.
    if (parent >= 0) {
        cycle_out = true;
    }
    return result;
}

} // namespace

// ── SkinnedMesh methods ─────────────────────────────────────────

BoneFrame SkinnedMesh::get_frame(int bone, int frame) const {
    if (bones.empty() || num_frames == 0) {
        return BoneFrame{glm::vec4(0, 0, 0, 1), glm::quat(1, 0, 0, 0)};
    }
    frame = std::clamp(frame, 0, num_frames - 1);
    bone  = std::clamp(bone, 0, static_cast<int>(bones.size()) - 1);
    return frames[static_cast<size_t>(frame) * bones.size() + bone];
}

glm::mat4 SkinnedMesh::bone_world_transform(int bone, int frame) const {
    bool cycle = false;
    glm::mat4 result = walk_to_root(bone,
        [&](int b) { return bone_frame_to_mat4(get_frame(b, frame)); },
        bones, cycle);
    if (cycle) warn_parent_cycle_once(bone);
    return result;
}

std::vector<Vertex> SkinnedMesh::skin_frame(int surface, int frame) const {
    if (surface < 0 || std::cmp_greater_equal(surface, surfaces.size())) return {};
    const auto& surf = surfaces[surface];
    const int num_bones = static_cast<int>(bones.size());

    // Precompute world transforms and bind-inverse for all bones
    std::vector<glm::mat4> world_tf(num_bones);
    std::vector<glm::mat4> bind_inv(num_bones);
    for (int b = 0; b < num_bones; b++) {
        glm::mat4 bind_world = bone_world_transform(b, 0);
        bind_inv[b] = glm::inverse(bind_world);
        world_tf[b] = bone_world_transform(b, frame);
    }

    // Precompute skinning matrices: skin[b] = world_tf[b] * bind_inv[b]
    std::vector<glm::mat4> skin_mat(num_bones);
    for (int b = 0; b < num_bones; b++) {
        skin_mat[b] = world_tf[b] * bind_inv[b];
    }

    std::vector<Vertex> out(surf.vertices.size());
    for (size_t i = 0; i < surf.vertices.size(); i++) {
        const auto& sv = surf.vertices[i];
        glm::vec4 pos(sv.px, sv.py, sv.pz, 1.0f);
        glm::vec4 nrm(sv.nx, sv.ny, sv.nz, 0.0f);

        glm::vec4 skinned_pos(0.0f);
        glm::vec4 skinned_nrm(0.0f);

        for (int w = 0; w < 4; w++) {
            float weight = sv.bone_weights[w];
            if (weight <= 0.0f) continue;
            int bi = sv.bone_indices[w];
            if (!loadval::in_range(bi, 0, num_bones - 1)) continue;
            skinned_pos += weight * (skin_mat[bi] * pos);
            skinned_nrm += weight * (skin_mat[bi] * nrm);
        }

        // Normalize the normal
        glm::vec3 n = glm::vec3(skinned_nrm);
        float nlen = glm::length(n);
        if (nlen > 1e-6f) n /= nlen;

        auto& v = out[i];
        v.px = skinned_pos.x; v.py = skinned_pos.y; v.pz = skinned_pos.z;
        v.nx = n.x; v.ny = n.y; v.nz = n.z;
        v.u  = sv.u; v.v = sv.v;
        v.u1 = sv.u1; v.v1 = sv.v1;
        v.displacement_normal_scale = sv.displacement_normal_scale;
        v.tx = v.ty = v.tz = 0.0f; v.tw = 1.0f;
    }

    // Compute tangents on the skinned output
    compute_tangents(out, surf.indices);

    return out;
}

std::vector<Vertex> SkinnedMesh::skin_interpolated(int surface, float frame_f) const {
    if (surface < 0 || std::cmp_greater_equal(surface, surfaces.size())) return {};
    if (num_frames <= 1) return skin_frame(surface, 0);

    // Clamp to valid range
    frame_f = std::clamp(frame_f, 0.0f, static_cast<float>(num_frames - 1));

    int f0 = static_cast<int>(std::floor(frame_f));
    int f1 = static_cast<int>(std::ceil(frame_f));
    float t = frame_f - static_cast<float>(f0);

    f0 = std::clamp(f0, 0, num_frames - 1);
    f1 = std::clamp(f1, 0, num_frames - 1);

    // If no interpolation needed, just skin directly
    if (f0 == f1 || t < 1e-6f) return skin_frame(surface, f0);
    if (t > 1.0f - 1e-6f)      return skin_frame(surface, f1);

    const auto& surf = surfaces[surface];
    const int num_bones = static_cast<int>(bones.size());

    // Interpolate bone frames between f0 and f1
    std::vector<BoneFrame> interp_frames(num_bones);
    for (int b = 0; b < num_bones; b++) {
        const auto& bf0 = get_frame(b, f0);
        const auto& bf1 = get_frame(b, f1);
        interp_frames[b].position = glm::mix(bf0.position, bf1.position, t);
        interp_frames[b].rotation = glm::slerp(bf0.rotation, bf1.rotation, t);
    }

    // Build world transforms from interpolated frames by walking parent chains
    // We need a helper lambda since we can't use the member function (it reads
    // from the stored frames array, not our interpolated data).
    auto interp_world = [&](int bone) -> glm::mat4 {
        bool cycle = false;
        glm::mat4 result = walk_to_root(bone,
            [&](int b) { return bone_frame_to_mat4(interp_frames[b]); },
            bones, cycle);
        if (cycle) warn_parent_cycle_once(bone);
        return result;
    };

    // Precompute bind-inverse (frame 0) and interpolated world transforms
    std::vector<glm::mat4> skin_mat(num_bones);
    for (int b = 0; b < num_bones; b++) {
        glm::mat4 bind_inv = glm::inverse(bone_world_transform(b, 0));
        skin_mat[b] = interp_world(b) * bind_inv;
    }

    std::vector<Vertex> out(surf.vertices.size());
    for (size_t i = 0; i < surf.vertices.size(); i++) {
        const auto& sv = surf.vertices[i];
        glm::vec4 pos(sv.px, sv.py, sv.pz, 1.0f);
        glm::vec4 nrm(sv.nx, sv.ny, sv.nz, 0.0f);

        glm::vec4 skinned_pos(0.0f);
        glm::vec4 skinned_nrm(0.0f);

        for (int w = 0; w < 4; w++) {
            float weight = sv.bone_weights[w];
            if (weight <= 0.0f) continue;
            int bi = sv.bone_indices[w];
            if (!loadval::in_range(bi, 0, num_bones - 1)) continue;
            skinned_pos += weight * (skin_mat[bi] * pos);
            skinned_nrm += weight * (skin_mat[bi] * nrm);
        }

        glm::vec3 n = glm::vec3(skinned_nrm);
        float nlen = glm::length(n);
        if (nlen > 1e-6f) n /= nlen;

        auto& v = out[i];
        v.px = skinned_pos.x; v.py = skinned_pos.y; v.pz = skinned_pos.z;
        v.nx = n.x; v.ny = n.y; v.nz = n.z;
        v.u  = sv.u; v.v = sv.v;
        v.u1 = sv.u1; v.v1 = sv.v1;
        v.displacement_normal_scale = sv.displacement_normal_scale;
        v.tx = v.ty = v.tz = 0.0f; v.tw = 1.0f;
    }

    compute_tangents(out, surf.indices);

    return out;
}

std::vector<glm::mat4> SkinnedMesh::bone_matrices_interpolated(float frame_f) const {
    const int num_bones = static_cast<int>(bones.size());
    std::vector<glm::mat4> out(static_cast<size_t>(num_bones), glm::mat4(1.0f));
    if (num_bones == 0) return out;

    // Bind reference is frame 0 — the same convention skin_frame /
    // skin_interpolated use to build their per-bone skinning matrices.
    std::vector<glm::mat4> bind_inv(num_bones);
    for (int b = 0; b < num_bones; b++) {
        bind_inv[b] = glm::inverse(bone_world_transform(b, 0));
    }

    // With no animation (≤1 frame) the only pose is the bind pose, so every
    // bone matrix is world(0) * inverse(world(0)) == identity, matching
    // skin_interpolated's `num_frames <= 1 → skin_frame(0)` early-out.
    if (num_frames <= 1) {
        for (int b = 0; b < num_bones; b++) {
            out[b] = bone_world_transform(b, 0) * bind_inv[b];
        }
        return out;
    }

    // Clamp to the valid range and split into the bracketing integer frames and
    // the interpolation weight — identical arithmetic to skin_interpolated so
    // the two paths land on the same pose for any frame_f.
    frame_f = std::clamp(frame_f, 0.0f, static_cast<float>(num_frames - 1));

    int f0 = static_cast<int>(std::floor(frame_f));
    int f1 = static_cast<int>(std::ceil(frame_f));
    float t = frame_f - static_cast<float>(f0);

    f0 = std::clamp(f0, 0, num_frames - 1);
    f1 = std::clamp(f1, 0, num_frames - 1);

    // No interpolation needed → use the integer-frame world transforms directly,
    // mirroring skin_interpolated's early-outs that delegate to skin_frame.
    if (f0 == f1 || t < 1e-6f) {
        for (int b = 0; b < num_bones; b++) {
            out[b] = bone_world_transform(b, f0) * bind_inv[b];
        }
        return out;
    }
    if (t > 1.0f - 1e-6f) {
        for (int b = 0; b < num_bones; b++) {
            out[b] = bone_world_transform(b, f1) * bind_inv[b];
        }
        return out;
    }

    // Interpolate the per-bone local frames (lerp position/scale, slerp
    // rotation) exactly as skin_interpolated, then walk each bone's parent
    // chain to compose its interpolated world transform.
    std::vector<BoneFrame> interp_frames(num_bones);
    for (int b = 0; b < num_bones; b++) {
        const auto& bf0 = get_frame(b, f0);
        const auto& bf1 = get_frame(b, f1);
        interp_frames[b].position = glm::mix(bf0.position, bf1.position, t);
        interp_frames[b].rotation = glm::slerp(bf0.rotation, bf1.rotation, t);
    }

    auto interp_world = [&](int bone) -> glm::mat4 {
        bool cycle = false;
        glm::mat4 result = walk_to_root(bone,
            [&](int b) { return bone_frame_to_mat4(interp_frames[b]); },
            bones, cycle);
        if (cycle) warn_parent_cycle_once(bone);
        return result;
    };

    for (int b = 0; b < num_bones; b++) {
        out[b] = interp_world(b) * bind_inv[b];
    }
    return out;
}

// ── ms08 loader ─────────────────────────────────────────────────

Result<SkinnedMesh> load_smesh(const std::string& path,
                              AssetFileSystemRef assets) {
    auto opened =
        loadval::AssetBinaryReader::open(path, assets, "skinned mesh");
    if (!opened) return std::unexpected(opened.error());
    auto reader = std::move(*opened);

    using namespace heaven::loadval;

    // ── Magic ───────────────────────────────────────────────────
    uint32_t magic = 0;
    if (!read_pod(reader, magic)) {
        return std::unexpected(Error(
            std::format("Short read on magic in skinned mesh: {}", path)));
    }
    constexpr uint32_t expected = 'm' | ('s' << 8) | ('0' << 16) | ('8' << 24);
    if (magic != expected) {
        return std::unexpected(Error(
            std::format("Bad magic in skinned mesh: {} (got 0x{:08X}, expected 0x{:08X})",
                        path, magic, expected)));
    }

    SkinnedMesh mesh;
    mesh.path = path;

    // Extract a short name from the file path
    {
        size_t slash = path.find_last_of("/\\");
        size_t dot   = path.rfind('.');
        if (slash != std::string::npos && dot != std::string::npos && dot > slash)
            mesh.name = path.substr(slash + 1, dot - slash - 1);
        else if (dot != std::string::npos)
            mesh.name = path.substr(0, dot);
        else
            mesh.name = path;
    }

    // ── Bones ───────────────────────────────────────────────────
    int32_t num_bones = 0;
    if (!read_pod(reader, num_bones)) {
        return std::unexpected(Error(
            std::format("Short read on bone count in: {}", path)));
    }
    if (!in_range(num_bones, 0, kMaxBones)) {
        return std::unexpected(Error(
            std::format("Invalid bone count {} in: {}", num_bones, path)));
    }

    mesh.bones.resize(static_cast<size_t>(num_bones));
    for (int b = 0; b < num_bones; b++) {
        int32_t name_len = 0;
        if (!read_pod(reader, name_len)) {
            return std::unexpected(Error(
                std::format("Short read on bone name length for bone {} in: {}", b, path)));
        }
        if (!in_range(name_len, 1, kMaxNameLen)) {
            return std::unexpected(Error(
                std::format("Invalid bone name length {} for bone {} in: {}",
                            name_len, b, path)));
        }
        std::vector<char> nbuf(static_cast<size_t>(name_len));
        if (!read_array(reader, std::span(nbuf))) {
            return std::unexpected(Error(
                std::format("Short read on bone name for bone {} in: {}", b, path)));
        }
        // name_len includes null terminator
        mesh.bones[b].name = std::string(nbuf.data(),
            static_cast<size_t>(name_len > 0 ? name_len - 1 : 0));

        int32_t parent = 0;
        if (!read_pod(reader, parent)) {
            return std::unexpected(Error(
                std::format("Short read on bone parent for bone {} in: {}", b, path)));
        }
        // Validate the parent link is a real bone or the root sentinel (-1).
        // An out-of-range parent would index `bones` out of bounds during the
        // world-transform walk, so clamp it to -1 (root), continue loading,
        // and name the offending bone.
        if (!in_range(parent, -1, num_bones - 1)) {
            std::fprintf(stderr,
                "  WARNING: %s bone %d '%s': parent %d out of range [-1, %d); "
                "using -1 (root)\n",
                path.c_str(), b, mesh.bones[b].name.c_str(), parent, num_bones);
            parent = -1;
        }
        mesh.bones[b].parent = parent;
    }

    // ── Animation frames ────────────────────────────────────────
    int32_t num_frames = 0;
    if (!read_pod(reader, num_frames)) {
        return std::unexpected(Error(
            std::format("Short read on frame count in: {}", path)));
    }
    if (num_frames < 0 || num_frames > 100000) {
        return std::unexpected(Error(
            std::format("Invalid frame count {} in: {}", num_frames, path)));
    }
    mesh.num_frames = num_frames;

    size_t total_entries = static_cast<size_t>(num_bones) * static_cast<size_t>(num_frames);
    constexpr uint64_t frame_bytes = 8 * sizeof(float);
    if (!fits(reader.tell(), static_cast<uint64_t>(total_entries) * frame_bytes,
              reader.size())) {
        return std::unexpected(Error(
            std::format("Truncated animation frame payload in: {}", path)));
    }
    mesh.frames.resize(total_entries);

    for (size_t i = 0; i < total_entries; i++) {
        float pos_scale[4]; // xyz = position, w = uniform scale
        float rot[4];       // xyzw quaternion
        if (!read_array(reader, std::span(pos_scale)) ||
            !read_array(reader, std::span(rot))) {
            return std::unexpected(Error(
                std::format("Short read on frame {} in: {}", i, path)));
        }

        mesh.frames[i].position = glm::vec4(pos_scale[0], pos_scale[1],
                                             pos_scale[2], pos_scale[3]);
        mesh.frames[i].rotation = glm::quat(rot[3], rot[0], rot[1], rot[2]); // glm: w,x,y,z
    }

    // ── Surfaces ────────────────────────────────────────────────
    int32_t num_surfaces = 0;
    if (!read_pod(reader, num_surfaces)) {
        return std::unexpected(Error(
            std::format("Short read on surface count in: {}", path)));
    }
    if (!in_range(num_surfaces, 0, kMaxSurfaces)) {
        return std::unexpected(Error(
            std::format("Invalid surface count {} in: {}", num_surfaces, path)));
    }

    mesh.surfaces.resize(static_cast<size_t>(num_surfaces));

    // Read surface names
    for (int s = 0; s < num_surfaces; s++) {
        int32_t name_len = 0;
        if (!read_pod(reader, name_len)) {
            return std::unexpected(Error(
                std::format("Short read on surface name length for surface {} in: {}", s, path)));
        }
        if (!in_range(name_len, 1, kMaxNameLen)) {
            return std::unexpected(Error(
                std::format("Invalid surface name length {} for surface {} in: {}",
                            name_len, s, path)));
        }
        std::vector<char> nbuf(static_cast<size_t>(name_len));
        if (!read_array(reader, std::span(nbuf))) {
            return std::unexpected(Error(
                std::format("Short read on surface name for surface {} in: {}", s, path)));
        }
        mesh.surfaces[s].name = std::string(nbuf.data(),
            static_cast<size_t>(name_len > 0 ? name_len - 1 : 0));
    }

    // Read surface geometry data
    for (int s = 0; s < num_surfaces; s++) {
        auto& surf = mesh.surfaces[s];

        // ── Vertices ────────────────────────────────────────────
        int32_t num_verts = 0;
        if (!read_pod(reader, num_verts)) {
            return std::unexpected(Error(
                std::format("Short read on vertex count for surface {} in: {}", s, path)));
        }
        if (!in_range(num_verts, 0, kMaxElemCount)) {
            return std::unexpected(Error(
                std::format("Invalid vertex count {} for surface {} in: {}",
                            num_verts, s, path)));
        }
        // Every vertex has at least a position, compressed normal, and weight
        // count; the variable-length weight records are checked while reading.
        constexpr uint64_t min_vertex_bytes =
            3 * sizeof(float) + 3 * sizeof(uint16_t) + sizeof(uint8_t);
        if (!fits(reader.tell(), static_cast<uint64_t>(num_verts) * min_vertex_bytes,
                  reader.size())) {
            return std::unexpected(Error(
                std::format("Truncated vertex payload for surface {} in: {}", s, path)));
        }
        surf.vertices.resize(static_cast<size_t>(num_verts));

        for (int vi = 0; vi < num_verts; vi++) {
            auto& sv = surf.vertices[vi];

            // Position: float[3]
            float pos[3];
            if (!read_array(reader, std::span(pos))) {
                return std::unexpected(Error(
                    std::format("Short read on vertex position {} for surface {} in: {}",
                                vi, s, path)));
            }
            sv.px = pos[0]; sv.py = pos[1]; sv.pz = pos[2];

            // Compressed normal: unsigned short[3]
            uint16_t nrm[3];
            if (!read_array(reader, std::span(nrm))) {
                return std::unexpected(Error(
                    std::format("Short read on vertex normal {} for surface {} in: {}",
                                vi, s, path)));
            }
            sv.nx = (static_cast<float>(nrm[0]) / 65535.0f) * 2.0f - 1.0f;
            sv.ny = (static_cast<float>(nrm[1]) / 65535.0f) * 2.0f - 1.0f;
            sv.nz = (static_cast<float>(nrm[2]) / 65535.0f) * 2.0f - 1.0f;

            // Bone weights
            uint8_t num_weights = 0;
            if (!read_pod(reader, num_weights)) {
                return std::unexpected(Error(
                    std::format("Short read on weight count for vertex {} surface {} in: {}",
                                vi, s, path)));
            }

            // Read all weights into a temporary buffer
            struct RawWeight {
                uint16_t bone;
                uint8_t  weight;
            };
            std::vector<RawWeight> raw_weights(num_weights);
            for (int wi = 0; std::cmp_less(wi, num_weights); wi++) {
                if (!read_pod(reader, raw_weights[wi].bone) ||
                    !read_pod(reader, raw_weights[wi].weight)) {
                    return std::unexpected(Error(
                        std::format("Short read on weight {} for vertex {} surface {} in: {}",
                                    wi, vi, s, path)));
                }
            }

            // Sort by weight descending, take top 4
            std::sort(raw_weights.begin(), raw_weights.end(),
                      [](const RawWeight& a, const RawWeight& b) {
                          return a.weight > b.weight;
                      });

            int count = std::min(static_cast<int>(num_weights), 4);
            float total_weight = 0.0f;
            for (int wi = 0; wi < count; wi++) {
                sv.bone_indices[wi] = raw_weights[wi].bone;
                sv.bone_weights[wi] = static_cast<float>(raw_weights[wi].weight) / 255.0f;
                total_weight += sv.bone_weights[wi];
            }

            // Normalize weights so they sum to 1.0
            if (total_weight > 1e-6f) {
                for (int wi = 0; wi < count; wi++) {
                    sv.bone_weights[wi] /= total_weight;
                }
            } else if (num_bones > 0) {
                // Fallback: bind to bone 0 with full weight
                sv.bone_indices[0] = 0;
                sv.bone_weights[0] = 1.0f;
            }
        }

        // ── UV set 0 ───────────────────────────────────────────
        int32_t num_tc0 = 0;
        if (!read_pod(reader, num_tc0)) {
            return std::unexpected(Error(
                std::format("Short read on UV0 count for surface {} in: {}", s, path)));
        }
        if (!in_range(num_tc0, 0, kMaxElemCount)) {
            return std::unexpected(Error(
                std::format("Invalid UV0 count {} for surface {} in: {}",
                            num_tc0, s, path)));
        }
        for (int t = 0; t < num_tc0; t++) {
            float uv[2];
            if (!read_array(reader, std::span(uv))) {
                return std::unexpected(Error(
                    std::format("Short read on UV0 {} for surface {} in: {}", t, s, path)));
            }
            if (t < num_verts) {
                surf.vertices[t].u = uv[0];
                surf.vertices[t].v = uv[1];
            }
        }

        // ── UV set 1 ───────────────────────────────────────────
        // Detail modes overlay_1 / multiply_1 address baked atlas data
        // through this coordinate set.
        int32_t num_tc1 = 0;
        if (!read_pod(reader, num_tc1)) {
            return std::unexpected(Error(
                std::format("Short read on UV1 count for surface {} in: {}", s, path)));
        }
        if (!in_range(num_tc1, 0, kMaxElemCount)) {
            return std::unexpected(Error(
                std::format("Invalid UV1 count {} for surface {} in: {}",
                            num_tc1, s, path)));
        }
        for (int t = 0; t < num_tc1; t++) {
            float uv[2];
            if (!read_array(reader, std::span(uv))) {
                return std::unexpected(Error(
                    std::format("Short read on UV1 {} for surface {} in: {}", t, s, path)));
            }
            if (t < num_verts) {
                surf.vertices[t].u1 = uv[0];
                surf.vertices[t].v1 = uv[1];
            }
        }

        // ── Triangle indices ────────────────────────────────────
        int32_t num_tris = 0;
        if (!read_pod(reader, num_tris)) {
            return std::unexpected(Error(
                std::format("Short read on triangle count for surface {} in: {}", s, path)));
        }
        if (!in_range(num_tris, 0, kMaxElemCount)) {
            return std::unexpected(Error(
                std::format("Invalid triangle count {} for surface {} in: {}",
                            num_tris, s, path)));
        }
        if (num_verts == 0 && num_tris != 0) {
            return std::unexpected(Error(
                std::format("Triangles without vertices for surface {} in: {}", s, path)));
        }
        const uint64_t index_bytes = num_verts < 65536
            ? sizeof(uint16_t) : sizeof(uint32_t);
        if (!fits(reader.tell(), static_cast<uint64_t>(num_tris) * 3 * index_bytes,
                  reader.size())) {
            return std::unexpected(Error(
                std::format("Truncated index payload for surface {} in: {}", s, path)));
        }
        surf.indices.resize(static_cast<size_t>(num_tris) * 3);

        if (num_verts < 65536) {
            for (int t = 0; t < num_tris; t++) {
                uint16_t idx[3];
                if (!read_array(reader, std::span(idx))) {
                    return std::unexpected(Error(
                        std::format("Short read on triangle {} for surface {} in: {}",
                                    t, s, path)));
                }
                surf.indices[t * 3 + 0] = idx[0];
                surf.indices[t * 3 + 1] = idx[1];
                surf.indices[t * 3 + 2] = idx[2];
            }
        } else {
            if (!read_array(reader, std::span(surf.indices))) {
                return std::unexpected(Error(
                    std::format("Short read on triangle indices for surface {} in: {}", s, path)));
            }
        }

        // Validate indices
        for (size_t ii = 0; ii < surf.indices.size(); ii++) {
            if (std::cmp_greater_equal(surf.indices[ii], num_verts)) {
                std::fprintf(stderr,
                    "  WARNING: %s surf %d '%s': index[%zu]=%u >= num_verts=%d\n",
                    path.c_str(), s, surf.name.c_str(), ii, surf.indices[ii], num_verts);
                surf.indices[ii] = 0;
            }
        }
    }

    std::fprintf(stderr, "  Loaded smesh: %s — %d bones, %d frames, %d surfaces\n",
                 mesh.name.c_str(), num_bones, num_frames,
                 static_cast<int>(mesh.surfaces.size()));

    return mesh;
}

} // namespace heaven
