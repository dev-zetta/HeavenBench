#include "benchmark/microprofiler.h"
/*
 * HeavenBench — Mesh store
 *
 * Extracted verbatim out of FilamentBackend::upload_mesh /
 * update_mesh_vertices.
 */
#include "rhi/mesh_store.h"
#include "rhi/mesh_displacement.h"
#include "rhi/displacement_carrier.h"
#include "rhi/skinned_tangent_upload.h"

#include <filament/Engine.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>

#include <geometry/SurfaceOrientation.h>

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <new>
#include <vector>

namespace heaven {

MeshStore::~MeshStore() {
    for (auto& [id, md] : mesh_data_) {
        for (auto* vb : md.vertex_buffers) engine_.destroy(vb);
        for (auto* ib : md.index_buffers)  engine_.destroy(ib);
    }
    mesh_data_.clear();
}

void MeshStore::release(const GpuMesh& mesh) {
    for (const auto& surface : mesh.surfaces) {
        const auto it = mesh_data_.find(surface.handle);
        if (it == mesh_data_.end()) continue;
        for (auto* vb : it->second.vertex_buffers) engine_.destroy(vb);
        for (auto* ib : it->second.index_buffers) engine_.destroy(ib);
        mesh_data_.erase(it);
    }
}

GpuMesh MeshStore::upload(const Mesh& mesh) {
    profile::Scope profile_scope("Upload static mesh", "Upload");
    GpuMesh gm;
    gm.name = mesh.name;
    gm.path = mesh.path;

    // Compute accurate bounding box from actual vertex positions
    // (the mi08 header bbox may be stale after CPU tessellation)
    glm::vec3 computed_min(1e9f), computed_max(-1e9f);
    for (const auto& surf : mesh.surfaces) {
        for (const auto& v : surf.vertices) {
            computed_min.x = std::min(computed_min.x, v.px);
            computed_min.y = std::min(computed_min.y, v.py);
            computed_min.z = std::min(computed_min.z, v.pz);
            computed_max.x = std::max(computed_max.x, v.px);
            computed_max.y = std::max(computed_max.y, v.py);
            computed_max.z = std::max(computed_max.z, v.pz);
        }
    }
    if (computed_min.x < computed_max.x) {
        gm.bbox_min = computed_min;
        gm.bbox_max = computed_max;
    } else {
        gm.bbox_min = mesh.bbox_min;
        gm.bbox_max = mesh.bbox_max;
    }

    size_t source_surface_index = 0;
    for (const auto& surf : mesh.surfaces) {
        const size_t current_surface_index = source_surface_index++;
        if (surf.vertices.empty() || surf.indices.empty()) continue;

        const uint32_t vertex_count = static_cast<uint32_t>(surf.vertices.size());
        const uint32_t index_count  = static_cast<uint32_t>(surf.indices.size());

        // ── Convert tangent frames to quaternions ───────────────
        // Filament requires tangents as quaternions (short4).
        // We use SurfaceOrientation with positions+UVs+normals+indices
        // (Lengyel's method) to compute tangent frames aligned with the
        // UV mapping. This is critical for POM — the mesh's original
        // tangent data from mi08 files is unreliable on flat surfaces.

        std::vector<filament::math::float3> positions(vertex_count);
        std::vector<filament::math::float3> normals(vertex_count);
        std::vector<filament::math::float2> uvs(vertex_count);
        std::vector<filament::math::float2> uvs1(vertex_count);
        std::vector<filament::math::float4> displacement_normals(vertex_count);
        const auto footprints = mesh_displacement::calculate_footprints<Vertex>(
            surf.vertices, surf.indices);
        uint64_t deformation_signature = mesh_displacement::deformation_signature_seed;

        for (uint32_t i = 0; i < vertex_count; i++) {
            const auto& v = surf.vertices[i];
            positions[i] = { v.px, v.py, v.pz };
            normals[i]   = { v.nx, v.ny, v.nz };
            uvs[i]       = { v.u, v.v };
            uvs1[i]      = { v.u1, v.v1 };
            displacement_normals[i] = {
                v.displacement_normal_scale, footprints[i][0], footprints[i][1], 0};
            deformation_signature = mesh_displacement::append_deformation_signature(
                deformation_signature, v.u, v.v, v.displacement_normal_scale);
        }

        // Build triangle index array as uint3
        uint32_t tri_count = index_count / 3;
        std::vector<filament::math::uint3> triangles(tri_count);
        for (uint32_t i = 0; i < tri_count; i++) {
            triangles[i] = {
                surf.indices[i * 3 + 0],
                surf.indices[i * 3 + 1],
                surf.indices[i * 3 + 2]
            };
        }

        // Compute tangent quaternions from geometry (Lengyel's method)
        auto* orientation = filament::geometry::SurfaceOrientation::Builder()
            .vertexCount(vertex_count)
            .normals(normals.data())
            .uvs(uvs.data())
            .positions(positions.data())
            .triangleCount(tri_count)
            .triangles(triangles.data())
            .build();

        std::vector<filament::math::short4> quat_tangents(vertex_count);
        StaticTangentPacking tangent_packing = StaticTangentPacking::Unavailable;
        if (orientation) {
            orientation->getQuats(quat_tangents.data(), vertex_count);
            tangent_packing = StaticTangentPacking::PositionUvNormal;
            delete orientation;
        } else {
            // Fallback: normals only (arbitrary tangent orientation)
            auto* fallback = filament::geometry::SurfaceOrientation::Builder()
                .vertexCount(vertex_count)
                .normals(normals.data())
                .build();
            if (fallback) {
                fallback->getQuats(quat_tangents.data(), vertex_count);
                tangent_packing = StaticTangentPacking::NormalOnlyFallback;
                delete fallback;
            }
        }

        // ── Create interleaved vertex buffer ────────────────────
        // Layout: position (float3) + uv0 (float2) + uv1 (float2) = buffer 0
        //         tangents (short4, snorm) = buffer 1
        //         normal magnitude + UV footprint (float4 CUSTOM0) = buffer 2

        struct PosUV01 {
            filament::math::float3 position;
            filament::math::float2 uv0;
            filament::math::float2 uv1;
        };
        static_assert(sizeof(PosUV01) == 28);

        std::vector<PosUV01> pos_uv_data(vertex_count);
        for (uint32_t i = 0; i < vertex_count; i++) {
            pos_uv_data[i].position = positions[i];
            pos_uv_data[i].uv0 = uvs[i];
            pos_uv_data[i].uv1 = uvs1[i];
        }

        auto* vb = filament::VertexBuffer::Builder()
            .vertexCount(vertex_count)
            .bufferCount(4)
            .attribute(filament::VertexAttribute::POSITION, 0,
                       filament::VertexBuffer::AttributeType::FLOAT3,
                       0, sizeof(PosUV01))
            .attribute(filament::VertexAttribute::UV0, 0,
                       filament::VertexBuffer::AttributeType::FLOAT2,
                       offsetof(PosUV01, uv0), sizeof(PosUV01))
            .attribute(filament::VertexAttribute::UV1, 0,
                       filament::VertexBuffer::AttributeType::FLOAT2,
                       offsetof(PosUV01, uv1), sizeof(PosUV01))
            .attribute(filament::VertexAttribute::TANGENTS, 1,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .normalized(filament::VertexAttribute::TANGENTS)
            .attribute(filament::VertexAttribute::CUSTOM0, 2,
                       filament::VertexBuffer::AttributeType::FLOAT4,
                       0, sizeof(filament::math::float4))
            .attribute(filament::VertexAttribute::CUSTOM1, 3,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM2, 3,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM3, 3,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM4, 3,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM5, 3,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM6, 3,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM7, 3,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .build(engine_);

        // Upload position+UV data
        {
            size_t size = vertex_count * sizeof(PosUV01);
            auto* buf = new uint8_t[size];
            std::memcpy(buf, pos_uv_data.data(), size);
            vb->setBufferAt(engine_, 0,
                filament::VertexBuffer::BufferDescriptor(
                    buf, size, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        }

        // Upload tangent quaternion data
        {
            size_t size = vertex_count * sizeof(filament::math::short4);
            auto* buf = new uint8_t[size];
            std::memcpy(buf, quat_tangents.data(), size);
            vb->setBufferAt(engine_, 1,
                filament::VertexBuffer::BufferDescriptor(
                    buf, size, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        }
        {
            size_t size = vertex_count * sizeof(filament::math::float4);
            auto* buf = new uint8_t[size];
            std::memcpy(buf, displacement_normals.data(), size);
            vb->setBufferAt(engine_, 2,
                filament::VertexBuffer::BufferDescriptor(
                    buf, size, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        }

        {
            auto* zeros = new filament::math::short4[vertex_count]{};
            vb->setBufferAt(engine_, 3, filament::VertexBuffer::BufferDescriptor(
                zeros, vertex_count * sizeof(*zeros), [](void* b, size_t, void*) {
                    delete[] static_cast<filament::math::short4*>(b);
                }));
        }

        // ── Create index buffer ─────────────────────────────────
        auto* ib = filament::IndexBuffer::Builder()
            .indexCount(index_count)
            .bufferType(filament::IndexBuffer::IndexType::UINT)
            .build(engine_);

        {
            size_t size = index_count * sizeof(uint32_t);
            auto* buf = new uint8_t[size];
            std::memcpy(buf, surf.indices.data(), size);
            ib->setBuffer(engine_,
                filament::IndexBuffer::BufferDescriptor(
                    buf, size, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        }

        uint64_t id = next_mesh_id_++;

        GpuSurface gs;
        gs.handle      = id;
        gs.index_count = index_count;
        gs.name        = surf.name;

        // Compute per-surface AABB so the backend can later place lights
        // at the surface centre (rather than the whole-mesh centre,
        // which for a building puts the light deep inside a wall).
        if (!surf.vertices.empty()) {
            glm::vec3 mn(surf.vertices[0].px, surf.vertices[0].py, surf.vertices[0].pz);
            glm::vec3 mx = mn;
            for (const auto& v : surf.vertices) {
                glm::vec3 p(v.px, v.py, v.pz);
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }
            gs.bbox_min = mn;
            gs.bbox_max = mx;
        }

        gm.surfaces.push_back(std::move(gs));

        // Store mesh data keyed by surface handle
        mesh_data_[id] = {};
        mesh_data_[id].vertex_buffers.push_back(vb);
        mesh_data_[id].index_buffers.push_back(ib);
        mesh_data_[id].vertex_count = vertex_count;
        mesh_data_[id].deformation_signature = deformation_signature;
        mesh_data_[id].source_surface_index = current_surface_index;
        mesh_data_[id].source_signature = packed_static_source_signature(surf.vertices, surf.indices);
        mesh_data_[id].original_tangent_packing = tangent_packing;
        mesh_data_[id].original_tangents = std::move(quat_tangents);
        mesh_data_[id].original_custom = std::move(displacement_normals);
    }

    return gm;
}

GpuMesh MeshStore::upload_skinned(const SkinnedMesh& mesh) {
    profile::Scope profile_scope("Upload skinned mesh", "Upload");
    GpuMesh gm;
    gm.name = mesh.name;
    gm.path = mesh.path;

    // Decide whether Filament-native skinning can be enabled for this
    // mesh. Filament supports up to 255 bones; over that we still upload
    // the rest-pose geometry (so the object can render via the CPU
    // fallback) but leave the surfaces unmarked, and warn once.
    const size_t bone_count = mesh.bones.size();
    const bool   bones_in_range = bone_count >= 1 && bone_count <= 255;
    if (bone_count > 255 && !bone_overflow_warned_.contains(mesh.name)) {
        std::fprintf(stderr,
            "  MeshStore: skinned mesh '%s' has %zu bones, exceeding "
            "Filament's 255-bone GPU-skinning limit — falling back to CPU "
            "skinning for this object\n",
            mesh.name.c_str(), bone_count);
        bone_overflow_warned_.insert(mesh.name);
    }
    const uint32_t marker_bone_count =
        bones_in_range ? static_cast<uint32_t>(bone_count) : 0u;

    // Compute the whole-mesh bounding box from the rest-pose vertex
    // positions (the ms08 format carries no usable header bbox once the
    // surface may have been CPU-tessellated).
    glm::vec3 computed_min(1e9f), computed_max(-1e9f);
    for (const auto& surf : mesh.surfaces) {
        for (const auto& v : surf.vertices) {
            computed_min.x = std::min(computed_min.x, v.px);
            computed_min.y = std::min(computed_min.y, v.py);
            computed_min.z = std::min(computed_min.z, v.pz);
            computed_max.x = std::max(computed_max.x, v.px);
            computed_max.y = std::max(computed_max.y, v.py);
            computed_max.z = std::max(computed_max.z, v.pz);
        }
    }
    if (computed_min.x < computed_max.x) {
        gm.bbox_min = computed_min;
        gm.bbox_max = computed_max;
    }

    size_t source_surface_index = 0;
    for (const auto& surf : mesh.surfaces) {
        const size_t current_surface_index = source_surface_index++;
        if (surf.vertices.empty() || surf.indices.empty()) continue;

        const uint32_t vertex_count = static_cast<uint32_t>(surf.vertices.size());
        const uint32_t index_count  = static_cast<uint32_t>(surf.indices.size());

        // ── Tangent frames over the REST pose ───────────────────
        // Identical to upload(): SurfaceOrientation (Lengyel) over the
        // rest-pose positions/UVs/normals. Filament skins the tangent
        // frame on the GPU, so this is computed exactly once.

        std::vector<filament::math::float3> positions(vertex_count);
        std::vector<filament::math::float3> normals(vertex_count);
        std::vector<filament::math::float2> uvs(vertex_count);
        std::vector<filament::math::float2> uvs1(vertex_count);
        std::vector<filament::math::float4> displacement_normals(vertex_count);
        const auto footprints = mesh_displacement::calculate_footprints<SkinnedVertex>(
            surf.vertices, surf.indices);
        uint64_t deformation_signature = mesh_displacement::deformation_signature_seed;

        for (uint32_t i = 0; i < vertex_count; i++) {
            const auto& v = surf.vertices[i];
            positions[i] = { v.px, v.py, v.pz };
            normals[i]   = { v.nx, v.ny, v.nz };
            uvs[i]       = { v.u, v.v };
            uvs1[i]      = { v.u1, v.v1 };
            displacement_normals[i] = {
                v.displacement_normal_scale, footprints[i][0], footprints[i][1], 0};
            deformation_signature = mesh_displacement::append_deformation_signature(
                deformation_signature, v.u, v.v, v.displacement_normal_scale);
        }

        uint32_t tri_count = index_count / 3;
        std::vector<filament::math::uint3> triangles(tri_count);
        for (uint32_t i = 0; i < tri_count; i++) {
            triangles[i] = {
                surf.indices[i * 3 + 0],
                surf.indices[i * 3 + 1],
                surf.indices[i * 3 + 2]
            };
        }

        auto* orientation = filament::geometry::SurfaceOrientation::Builder()
            .vertexCount(vertex_count)
            .normals(normals.data())
            .uvs(uvs.data())
            .positions(positions.data())
            .triangleCount(tri_count)
            .triangles(triangles.data())
            .build();

        std::vector<filament::math::short4> quat_tangents(vertex_count);
        if (orientation) {
            orientation->getQuats(quat_tangents.data(), vertex_count);
            delete orientation;
        } else {
            auto* fallback = filament::geometry::SurfaceOrientation::Builder()
                .vertexCount(vertex_count)
                .normals(normals.data())
                .build();
            if (fallback) {
                fallback->getQuats(quat_tangents.data(), vertex_count);
                delete fallback;
            }
        }

        // ── Build the rest-pose vertex buffer ───────────────────
        // Layout:
        //   buffer 0: position (float3) + uv0 (float2) + uv1 (float2)
        //   buffer 1: tangents (short4, snorm)
        //   buffer 2: BONE_INDICES (ushort4, integer / non-normalized)
        //   buffer 3: BONE_WEIGHTS (float4)
        //   buffer 4: normal magnitude + UV footprint (float4 CUSTOM0)
        // Buffer 0 keeps the exact PosUV01 layout used by upload()/
        // update_vertices, so the CPU-skinning fallback can still
        // re-upload positions into this same buffer if needed.

        struct PosUV01 {
            filament::math::float3 position;
            filament::math::float2 uv0;
            filament::math::float2 uv1;
        };
        static_assert(sizeof(PosUV01) == 28);

        std::vector<PosUV01> pos_uv_data(vertex_count);
        std::vector<filament::math::ushort4> bone_indices(vertex_count);
        std::vector<filament::math::float4>  bone_weights(vertex_count);
        for (uint32_t i = 0; i < vertex_count; i++) {
            const auto& v = surf.vertices[i];
            pos_uv_data[i].position = positions[i];
            pos_uv_data[i].uv0      = uvs[i];
            pos_uv_data[i].uv1      = uvs1[i];
            // Clamp bone indices into the unsigned-short range. Indices
            // are < bone_count (<= 255) in our data, so this never
            // actually clamps; it only guards against malformed input.
            auto clamp_idx = [](int b) -> uint16_t {
                if (b < 0) return 0;
                if (b > 65535) return 65535;
                return static_cast<uint16_t>(b);
            };
            bone_indices[i] = {
                clamp_idx(v.bone_indices[0]), clamp_idx(v.bone_indices[1]),
                clamp_idx(v.bone_indices[2]), clamp_idx(v.bone_indices[3]) };
            bone_weights[i] = {
                v.bone_weights[0], v.bone_weights[1],
                v.bone_weights[2], v.bone_weights[3] };
        }

        // BONE_INDICES is auto-flagged as an integer target by Filament
        // (see VertexBuffer.cpp) and must NOT be marked normalized.
        auto* vb = filament::VertexBuffer::Builder()
            .vertexCount(vertex_count)
            .bufferCount(6)
            .attribute(filament::VertexAttribute::POSITION, 0,
                       filament::VertexBuffer::AttributeType::FLOAT3,
                       0, sizeof(PosUV01))
            .attribute(filament::VertexAttribute::UV0, 0,
                       filament::VertexBuffer::AttributeType::FLOAT2,
                       offsetof(PosUV01, uv0), sizeof(PosUV01))
            .attribute(filament::VertexAttribute::UV1, 0,
                       filament::VertexBuffer::AttributeType::FLOAT2,
                       offsetof(PosUV01, uv1), sizeof(PosUV01))
            .attribute(filament::VertexAttribute::TANGENTS, 1,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .normalized(filament::VertexAttribute::TANGENTS)
            .attribute(filament::VertexAttribute::BONE_INDICES, 2,
                       filament::VertexBuffer::AttributeType::USHORT4,
                       0, sizeof(filament::math::ushort4))
            .attribute(filament::VertexAttribute::BONE_WEIGHTS, 3,
                       filament::VertexBuffer::AttributeType::FLOAT4,
                       0, sizeof(filament::math::float4))
            .attribute(filament::VertexAttribute::CUSTOM0, 4,
                       filament::VertexBuffer::AttributeType::FLOAT4,
                       0, sizeof(filament::math::float4))
            .attribute(filament::VertexAttribute::CUSTOM1, 5,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM2, 5,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM3, 5,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM4, 5,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM5, 5,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM6, 5,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .attribute(filament::VertexAttribute::CUSTOM7, 5,
                       filament::VertexBuffer::AttributeType::SHORT4,
                       0, sizeof(filament::math::short4))
            .build(engine_);

        // Upload position+UV data (buffer 0)
        {
            size_t size = vertex_count * sizeof(PosUV01);
            auto* buf = new uint8_t[size];
            std::memcpy(buf, pos_uv_data.data(), size);
            vb->setBufferAt(engine_, 0,
                filament::VertexBuffer::BufferDescriptor(
                    buf, size, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        }

        // Upload tangent quaternion data (buffer 1)
        {
            size_t size = vertex_count * sizeof(filament::math::short4);
            auto* buf = new uint8_t[size];
            std::memcpy(buf, quat_tangents.data(), size);
            vb->setBufferAt(engine_, 1,
                filament::VertexBuffer::BufferDescriptor(
                    buf, size, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        }

        // Upload bone indices (buffer 2)
        {
            size_t size = vertex_count * sizeof(filament::math::ushort4);
            auto* buf = new uint8_t[size];
            std::memcpy(buf, bone_indices.data(), size);
            vb->setBufferAt(engine_, 2,
                filament::VertexBuffer::BufferDescriptor(
                    buf, size, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        }

        // Upload bone weights (buffer 3)
        {
            size_t size = vertex_count * sizeof(filament::math::float4);
            auto* buf = new uint8_t[size];
            std::memcpy(buf, bone_weights.data(), size);
            vb->setBufferAt(engine_, 3,
                filament::VertexBuffer::BufferDescriptor(
                    buf, size, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        }
        {
            size_t size = vertex_count * sizeof(filament::math::float4);
            auto* buf = new uint8_t[size];
            std::memcpy(buf, displacement_normals.data(), size);
            vb->setBufferAt(engine_, 4,
                filament::VertexBuffer::BufferDescriptor(
                    buf, size, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        }

        // ── Index buffer (identical to upload()) ────────────────
        {
            auto* zeros = new filament::math::short4[vertex_count]{};
            vb->setBufferAt(engine_, 5, filament::VertexBuffer::BufferDescriptor(
                zeros, vertex_count * sizeof(*zeros), [](void* b, size_t, void*) {
                    delete[] static_cast<filament::math::short4*>(b);
                }));
        }

        auto* ib = filament::IndexBuffer::Builder()
            .indexCount(index_count)
            .bufferType(filament::IndexBuffer::IndexType::UINT)
            .build(engine_);

        {
            size_t size = index_count * sizeof(uint32_t);
            auto* buf = new uint8_t[size];
            std::memcpy(buf, surf.indices.data(), size);
            ib->setBuffer(engine_,
                filament::IndexBuffer::BufferDescriptor(
                    buf, size, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        }

        uint64_t id = next_mesh_id_++;

        GpuSurface gs;
        gs.handle      = id;
        gs.index_count = index_count;
        gs.name        = surf.name;

        // Per-surface AABB from the rest-pose positions (same as upload()).
        {
            glm::vec3 mn(surf.vertices[0].px, surf.vertices[0].py, surf.vertices[0].pz);
            glm::vec3 mx = mn;
            for (const auto& v : surf.vertices) {
                glm::vec3 p(v.px, v.py, v.pz);
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }
            gs.bbox_min = mn;
            gs.bbox_max = mx;
        }

        gm.surfaces.push_back(std::move(gs));

        // Store mesh data keyed by surface handle, recording the skinning
        // marker so SceneBuilder enables.skinning() on the renderable.
        MeshData md;
        md.vertex_buffers.push_back(vb);
        md.index_buffers.push_back(ib);
        md.vertex_count = vertex_count;
        md.deformation_signature = deformation_signature;
        md.source_surface_index = current_surface_index;
        md.skinned      = bones_in_range;
        md.bone_count   = marker_bone_count;
        md.skinned_source = true;
        mesh_data_[id]  = std::move(md);
    }

    return gm;
}

Result<PackedStaticSurface> MeshStore::take_original_packing(
        uint64_t handle, std::span<const Vertex> vertices,
        std::span<const uint32_t> indices) {
    auto it = mesh_data_.find(handle);
    if (it == mesh_data_.end() || it->second.skinned ||
            it->second.original_tangents.size() != vertices.size() ||
            it->second.original_custom.size() != vertices.size() ||
            it->second.source_signature != packed_static_source_signature(vertices, indices))
        return std::unexpected(Error{"Original static upload identity is unavailable"});
    auto& md = it->second;
    PackedStaticSurface result;
    result.tangent_frames = std::move(md.original_tangents);
    result.displacement_custom = std::move(md.original_custom);
    result.source_signature = md.source_signature;
    result.deformation_signature = md.deformation_signature;
    result.tangent_packing = md.original_tangent_packing;
    result.displacement_directions_valid = result.tangent_packing != StaticTangentPacking::Unavailable;
    result.positions.reserve(vertices.size());
    result.uv0.reserve(vertices.size());
    result.uv1.reserve(vertices.size());
    result.footprints.reserve(vertices.size());
    result.unit_displacement_directions.reserve(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const auto& v = vertices[i];
        result.positions.emplace_back(v.px, v.py, v.pz);
        result.uv0.emplace_back(v.u, v.v);
        result.uv1.emplace_back(v.u1, v.v1);
        result.footprints.push_back({result.displacement_custom[i].y,
                                     result.displacement_custom[i].z});
        const auto direction = decode_static_displacement_direction(result.tangent_frames[i]);
        const float normal_length2 = v.nx * v.nx + v.ny * v.ny + v.nz * v.nz;
        if (!direction || !std::isfinite(normal_length2) || normal_length2 <= 1e-20f)
            result.displacement_directions_valid = false;
        result.unit_displacement_directions.push_back(direction ? *direction : filament::math::float3{});
    }
    return result;
}

bool MeshStore::is_displacement_prepared(uint64_t handle) const {
    const auto found = mesh_data_.find(handle);
    return found != mesh_data_.end() && found->second.prepared_displacement;
}

std::optional<size_t> MeshStore::source_surface_index(uint64_t handle) const {
    const auto it = mesh_data_.find(handle);
    if (it == mesh_data_.end()) return std::nullopt;
    return it->second.source_surface_index;
}

void MeshStore::discard_original_packing() {
    for (auto& [handle, md] : mesh_data_) {
        std::vector<filament::math::short4>().swap(md.original_tangents);
        std::vector<filament::math::float4>().swap(md.original_custom);
    }
}

std::span<const SpatialDrawRange> MeshStore::prepared_draw_ranges(uint64_t handle) const {
    const auto it = mesh_data_.find(handle);
    if (it == mesh_data_.end() || !it->second.prepared_displacement) return {};
    return it->second.prepared_ranges;
}

Result<uint32_t> MeshStore::replace_displacement_surface(
        GpuSurface& surface, std::span<const Vertex> vertices,
        const PreparedDisplacementSurface& prepared) {
    profile::Scope profile_scope("Upload prepared displacement", "Upload");
    auto it = mesh_data_.find(surface.handle);
    if (it == mesh_data_.end() || it->second.skinned || it->second.prepared_displacement)
        return std::unexpected(Error{"Static displacement buffer replacement is unavailable"});
    if (it->second.source_signature != prepared.original_packed.source_signature ||
            it->second.vertex_count != vertices.size() ||
            it->second.source_signature != packed_static_source_signature(
                vertices, prepared.source_triangles))
        return std::unexpected(Error{"Static displacement replacement does not match its uploaded owner"});
    auto carrier = make_displacement_carrier(vertices, prepared);
    if (!carrier) return std::unexpected(carrier.error());
    struct PendingBuffers {
        filament::Engine& engine;
        filament::VertexBuffer* vertices = nullptr;
        filament::IndexBuffer* indices = nullptr;
        ~PendingBuffers() {
            if (vertices) engine.destroy(vertices);
            if (indices) engine.destroy(indices);
        }
    } pending{engine_};
    try {
        // Allocate the replacement owners before retiring anything. The commit
        // below only swaps vectors; allocation failure preserves the original mesh.
        std::vector<filament::VertexBuffer*> replacement_vertices(1);
        std::vector<filament::IndexBuffer*> replacement_indices(1);
        const auto count = static_cast<uint32_t>(carrier->packed.positions.size());
        std::vector<glm::vec3> neutral_positions(count);
        for (size_t i = 0; i < count; ++i) {
            const auto p = carrier->packed.positions[i];
            neutral_positions[i] = {p.x, p.y, p.z};
        }
        // Retain the carrier's original shader envelope. Target positions do
        // not bound per-parent runtime fades or the amplitude-scaled correction.
        glm::vec3 source_min(std::numeric_limits<float>::infinity());
        glm::vec3 source_max(-std::numeric_limits<float>::infinity());
        for (const auto& source : prepared.source_vertices) {
            source_min = glm::min(source_min, glm::vec3(source.neutral_position));
            source_max = glm::max(source_max, glm::vec3(source.neutral_position));
        }
        glm::vec3 padding = glm::max(source_min - carrier->bounds_min,
                                    carrier->bounds_max - source_max);
        for (int axis = 0; axis < 3; ++axis)
            padding[axis] = std::nextafter(padding[axis], std::numeric_limits<float>::infinity());
        auto partition = partition_spatial_draws(neutral_positions, carrier->indices, padding);
        if (!partition) return std::unexpected(partition.error());
        carrier->indices.swap(partition->indices);
        struct PosUV01 {
            filament::math::float3 position;
            filament::math::float2 uv0, uv1;
        };
        static_assert(sizeof(PosUV01) == 28);
        std::vector<PosUV01> positions(count);
        for (size_t i = 0; i < count; ++i)
            positions[i] = {carrier->packed.positions[i], carrier->packed.uv0[i], carrier->packed.uv1[i]};
        auto builder = filament::VertexBuffer::Builder().vertexCount(count).bufferCount(4)
            .attribute(filament::VertexAttribute::POSITION, 0,
                       filament::VertexBuffer::AttributeType::FLOAT3, 0, sizeof(PosUV01))
            .attribute(filament::VertexAttribute::UV0, 0,
                       filament::VertexBuffer::AttributeType::FLOAT2, offsetof(PosUV01, uv0), sizeof(PosUV01))
            .attribute(filament::VertexAttribute::UV1, 0,
                       filament::VertexBuffer::AttributeType::FLOAT2, offsetof(PosUV01, uv1), sizeof(PosUV01))
            .attribute(filament::VertexAttribute::TANGENTS, 1,
                       filament::VertexBuffer::AttributeType::SHORT4, 0, sizeof(filament::math::short4))
            .normalized(filament::VertexAttribute::TANGENTS)
            .attribute(filament::VertexAttribute::CUSTOM0, 2,
                       filament::VertexBuffer::AttributeType::FLOAT4, 0, sizeof(filament::math::float4));
        for (uint8_t i = 0; i < 7; ++i)
            builder.attribute(static_cast<filament::VertexAttribute>(
                    uint8_t(filament::VertexAttribute::CUSTOM1) + i), 3,
                    filament::VertexBuffer::AttributeType::FLOAT4,
                    i * sizeof(filament::math::float4), sizeof(DisplacementParentAttributes));
        auto* vb = pending.vertices = builder.build(engine_);
        if (!vb) return std::unexpected(Error{"Cannot allocate bounded displacement vertices"});
        const auto upload = [&](uint8_t slot, const auto& data) {
            const size_t bytes = data.size() * sizeof(data[0]);
            auto* copy = new uint8_t[bytes];
            std::memcpy(copy, data.data(), bytes);
            vb->setBufferAt(engine_, slot, filament::VertexBuffer::BufferDescriptor(
                copy, bytes, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        };
        upload(0, positions);
        upload(1, carrier->packed.tangent_frames);
        upload(2, carrier->packed.displacement_custom);
        upload(3, carrier->parents);
        auto* ib = pending.indices = filament::IndexBuffer::Builder()
            .indexCount(static_cast<uint32_t>(carrier->indices.size()))
            .bufferType(filament::IndexBuffer::IndexType::UINT).build(engine_);
        if (!ib) return std::unexpected(Error{"Cannot allocate bounded displacement indices"});
        const size_t bytes = carrier->indices.size() * sizeof(uint32_t);
        auto* copy = new uint8_t[bytes];
        std::memcpy(copy, carrier->indices.data(), bytes);
        ib->setBuffer(engine_, filament::IndexBuffer::BufferDescriptor(
            copy, bytes, [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); }));
        auto& md = it->second;
        replacement_vertices[0] = vb;
        replacement_indices[0] = ib;
        md.vertex_buffers.swap(replacement_vertices);
        md.index_buffers.swap(replacement_indices);
        md.prepared_ranges.swap(partition->ranges);
        pending.vertices = nullptr;
        pending.indices = nullptr;
        md.vertex_count = count;
        md.prepared_displacement = true;
        surface.index_count = static_cast<uint32_t>(carrier->indices.size());
        surface.bbox_min = carrier->bounds_min;
        surface.bbox_max = carrier->bounds_max;
        for (auto* old : replacement_vertices) engine_.destroy(old);
        for (auto* old : replacement_indices) engine_.destroy(old);
        return count;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{"Bounded displacement buffer allocation failed"});
    }
}

Result<void> MeshStore::update_vertices(uint64_t surface_handle,
                                const Vertex* vertices,
                                uint32_t vertex_count) {
    profile::Scope profile_scope("Upload deformed vertices", "Upload");
    auto md_it = mesh_data_.find(surface_handle);
    if (md_it == mesh_data_.end()) return {};
    auto& md = md_it->second;
    if (md.vertex_buffers.empty()) return {};
    if (md.prepared_displacement)
        return std::unexpected(Error{"Prepared static displacement cannot accept CPU deformation"});

    // Reject a re-upload whose vertex count does not match the buffer
    // built in upload(): Filament would copy vertex_count * sizeof(PosUV01)
    // into a buffer sized for the original count, corrupting it. CPU
    // skinning always preserves the bind-mesh vertex count, so the valid
    // path always matches and re-uploads exactly as before.
    if (vertex_count != md.vertex_count) {
        return std::unexpected(Error{
            "MeshStore::update_vertices: vertex count mismatch for surface "
            + std::to_string(surface_handle) + " (expected "
            + std::to_string(md.vertex_count) + ", got "
            + std::to_string(vertex_count) + ")"});
    }

    auto* vb = md.vertex_buffers[0];

    // Upload position+UV data (buffer 0)
    struct PosUV01 {
        filament::math::float3 position;
        filament::math::float2 uv0;
        filament::math::float2 uv1;
    };
    {
        size_t size = vertex_count * sizeof(PosUV01);
        auto* buf = new PosUV01[vertex_count];
        uint64_t deformation_signature = mesh_displacement::deformation_signature_seed;
        for (uint32_t i = 0; i < vertex_count; i++) {
            buf[i].position = {vertices[i].px, vertices[i].py, vertices[i].pz};
            buf[i].uv0 = {vertices[i].u, vertices[i].v};
            buf[i].uv1 = {vertices[i].u1, vertices[i].v1};
            deformation_signature = mesh_displacement::append_deformation_signature(
                deformation_signature, vertices[i].u, vertices[i].v,
                vertices[i].displacement_normal_scale);
        }
        if (deformation_signature != md.deformation_signature) {
            delete[] buf;
            return std::unexpected(Error{
                "MeshStore::update_vertices: UV0 or displacement-normal magnitude "
                "changed; a full mesh upload is required for surface "
                + std::to_string(surface_handle)});
        }
        vb->setBufferAt(engine_, 0,
            filament::VertexBuffer::BufferDescriptor(
                buf, size, [](void* b, size_t, void*) { delete[] static_cast<PosUV01*>(b); }));
    }

    // Skeletal CPU skinning provides UV-aligned tangents; keep the same frame
    // orientation as the GPU skin instead of inventing a normal-only frame.
    // Cloth/static deformation retains its existing normal-only update path.
    if (md.skinned_source) {
        const auto quats = pack_cpu_skinned_tangents(
            std::span<const Vertex>{vertices, vertex_count});
        if (quats.size() != vertex_count)
            return std::unexpected(Error{"CPU-skinned tangent packing failed"});
        auto* buf = new filament::math::short4[vertex_count];
        std::copy(quats.begin(), quats.end(), buf);
        vb->setBufferAt(engine_, 1,
            filament::VertexBuffer::BufferDescriptor(
                buf, vertex_count * sizeof(filament::math::short4),
                [](void* b, size_t, void*) {
                    delete[] static_cast<filament::math::short4*>(b);
                }));
        return {};
    }

    // Recompute tangent quaternions from normals only for other deformations.
    std::vector<filament::math::float3> normals(vertex_count);
    for (uint32_t i = 0; i < vertex_count; i++)
        normals[i] = {vertices[i].nx, vertices[i].ny, vertices[i].nz};

    auto* orientation = filament::geometry::SurfaceOrientation::Builder()
        .vertexCount(vertex_count)
        .normals(normals.data())
        .build();

    if (orientation) {
        auto* buf = new filament::math::short4[vertex_count];
        orientation->getQuats(buf, vertex_count);
        delete orientation;
        vb->setBufferAt(engine_, 1,
            filament::VertexBuffer::BufferDescriptor(
                buf, vertex_count * sizeof(filament::math::short4),
                [](void* b, size_t, void*) { delete[] static_cast<filament::math::short4*>(b); }));
    }

    // CUSTOM0 contains bind-mesh attributes. Keeping that buffer intact also
    // avoids a redundant upload and preserves the shared displacement footprint.

    return {};
}

MeshStore::SurfaceBuffers MeshStore::find(uint64_t surface_handle) const {
    auto it = mesh_data_.find(surface_handle);
    if (it == mesh_data_.end()) return {};
    const auto& md = it->second;
    if (md.vertex_buffers.empty() || md.index_buffers.empty()) return {};
    return { md.vertex_buffers[0], md.index_buffers[0] };
}

MeshStore::SkinnedInfo MeshStore::skinned_info(uint64_t surface_handle) const {
    auto it = mesh_data_.find(surface_handle);
    if (it == mesh_data_.end()) return {};
    return { it->second.skinned, it->second.bone_count, it->second.skinned_source };
}

} // namespace heaven
