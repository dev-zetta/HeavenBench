/*
 * HeavenBench — Mesh loading (mi08)
 */
#include "engine/mesh.h"
#include "engine/loader_validation.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <memory>
#include <utility>

namespace heaven {
namespace {

// ── Tangent computation (MikkTSpace-style) ──────────────────────

void compute_tangents(Surface& surf) {
    auto& verts = surf.vertices;
    auto& indices = surf.indices;

    // Zero out tangents
    for (auto& v : verts) { v.tx = v.ty = v.tz = 0.0f; v.tw = 1.0f; }

    // Accumulate tangent per triangle
    std::vector<float> bitan(verts.size() * 3, 0.0f);

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        auto& v0 = verts[indices[i]];
        auto& v1 = verts[indices[i+1]];
        auto& v2 = verts[indices[i+2]];

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
            bitan[idx*3]  += bx; bitan[idx*3+1] += by; bitan[idx*3+2] += bz;
        }
    }

    // Orthogonalize and normalize (Gram-Schmidt)
    for (size_t i = 0; i < verts.size(); i++) {
        auto& v = verts[i];
        float nx = v.nx, ny = v.ny, nz = v.nz;
        float tx = v.tx, ty = v.ty, tz = v.tz;

        // t = normalize(t - n * dot(n, t))
        float dot_nt = nx*tx + ny*ty + nz*tz;
        tx -= nx * dot_nt; ty -= ny * dot_nt; tz -= nz * dot_nt;
        float len = std::sqrt(tx*tx + ty*ty + tz*tz);
        if (len > 1e-6f) { tx /= len; ty /= len; tz /= len; }
        else { tx = 1; ty = 0; tz = 0; } // fallback

        v.tx = tx; v.ty = ty; v.tz = tz;

        // Handedness: sign of dot(cross(n, t), bitangent)
        float cx = ny*tz - nz*ty, cy = nz*tx - nx*tz, cz = nx*ty - ny*tx;
        float dot_cb = cx*bitan[i*3] + cy*bitan[i*3+1] + cz*bitan[i*3+2];
        v.tw = (dot_cb < 0.0f) ? -1.0f : 1.0f;
    }
}

} // namespace

// ── mi08 loader ─────────────────────────────────────────────────

Result<Mesh> load_mi08(const std::string& path,
                       AssetFileSystemRef assets) {
    auto opened =
        loadval::AssetBinaryReader::open(path, assets, "mesh");
    if (!opened) return std::unexpected(opened.error());
    auto reader = std::move(*opened);

    using namespace heaven::loadval;

    uint32_t magic = 0;
    if (!read_pod(reader, magic)) {
        return std::unexpected(Error(std::format("Short read on magic in mesh: {}", path)));
    }
    constexpr uint32_t expected = 'm' | ('i' << 8) | ('0' << 16) | ('8' << 24);
    if (magic != expected) {
        return std::unexpected(Error(std::format("Bad magic in mesh: {}", path)));
    }

    // Read bounding volume: AABB min(3f) + max(3f) + center(3f) + radius(1f) = 40 bytes.
    // Zero-initialised so no uninitialised value can reach bbox_min/max even on a
    // short read (which is itself rejected below).
    float bv[10] = {};
    if (!read_array(reader, std::span(bv))) {
        return std::unexpected(Error(
            std::format("Short read on bounding volume in mesh: {}", path)));
    }

    int32_t num_surfaces = 0;
    if (!read_pod(reader, num_surfaces)) {
        return std::unexpected(Error(
            std::format("Short read on surface count in mesh: {}", path)));
    }
    if (!in_range(num_surfaces, 1, kMaxSurfaces)) {
        return std::unexpected(Error(
            std::format("Invalid surface count {} in: {}", num_surfaces, path)));
    }

    Mesh mesh;
    mesh.path = path;
    mesh.bbox_min = glm::vec3(bv[0], bv[1], bv[2]);
    mesh.bbox_max = glm::vec3(bv[3], bv[4], bv[5]);
    mesh.surfaces.resize(static_cast<size_t>(num_surfaces));

    // Read surface headers
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
        mesh.surfaces[s].name = std::string(nbuf.data(), static_cast<size_t>(name_len - 1));
        if (!reader.seek_relative(40)) {
            return std::unexpected(Error(
                std::format("Short read on surface bounds in mesh: {}", path)));
        }
    }

    // Read surface data
    for (int s = 0; s < num_surfaces; s++) {
        auto& surf = mesh.surfaces[s];

        // Vertices (position + compressed normal)
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
        // Check the encoded payload before allocating the larger CPU vertex
        // representation. A count inside the cap can still describe a tiny,
        // truncated file and otherwise force hundreds of megabytes of allocation.
        constexpr uint64_t vertex_bytes = 3 * sizeof(float) + 3 * sizeof(uint16_t);
        if (!fits(reader.tell(), static_cast<uint64_t>(num_verts) * vertex_bytes,
                  reader.size())) {
            return std::unexpected(Error(
                std::format("Truncated vertex payload for surface {} in: {}", s, path)));
        }
        surf.vertices.resize(static_cast<size_t>(num_verts));

        for (int vi = 0; vi < num_verts; vi++) {
            float pos[3];
            uint16_t nrm[3];
            if (!read_array(reader, std::span(pos)) ||
                !read_array(reader, std::span(nrm))) {
                return std::unexpected(Error(
                    std::format("Short read on vertex {} for surface {} in: {}", vi, s, path)));
            }

            auto& v = surf.vertices[vi];
            v.px = pos[0]; v.py = pos[1]; v.pz = pos[2];
            v.nx = (static_cast<float>(nrm[0]) / 65535.0f) * 2.0f - 1.0f;
            v.ny = (static_cast<float>(nrm[1]) / 65535.0f) * 2.0f - 1.0f;
            v.nz = (static_cast<float>(nrm[2]) / 65535.0f) * 2.0f - 1.0f;
        }

        // UV set 0
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

        // UV set 1. Unigine detail modes overlay_1 / multiply_1 sample this
        // independently-authored atlas coordinate set rather than UV0.
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

        // Triangle indices
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
        // Index recovery to vertex 0 is possible only for a nonempty surface.
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
                        std::format("Short read on triangle {} for surface {} in: {}", t, s, path)));
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

        // Validate indices: a triangle index that references a vertex outside the
        // surface is replaced with 0 and loading continues (skip-and-recover).

        for (size_t ii = 0; ii < surf.indices.size(); ii++) {
            if (std::cmp_greater_equal(surf.indices[ii], num_verts)) {
                std::fprintf(stderr, "  WARNING: %s surf %d '%s': index[%zu]=%u >= num_verts=%d\n",
                             path.c_str(), s, surf.name.c_str(), ii, surf.indices[ii], num_verts);
                surf.indices[ii] = 0;
            }
        }
    }

    // Compute tangent vectors from UV mapping for normal/parallax mapping
    for (auto& surf : mesh.surfaces) {
        compute_tangents(surf);
    }

    return mesh;
}

} // namespace heaven
