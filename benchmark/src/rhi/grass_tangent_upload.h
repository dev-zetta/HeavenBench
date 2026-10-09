/*
 * HeavenBench — Filament tangent-frame upload for procedural grass
 *
 * GrassVertex keeps an ordinary object-space normal so the CPU generator stays
 * renderer-independent. Filament consumes normals through a quaternion-packed
 * TANGENTS attribute, so this adapter builds one orthonormal frame per vertex
 * and uploads the packed SNORM16 quaternion into vertex-buffer slot 1.
 */
#ifndef HEAVENBENCH_SRC_RHI_GRASS_TANGENT_UPLOAD_H_
#define HEAVENBENCH_SRC_RHI_GRASS_TANGENT_UPLOAD_H_

#include "engine/grass_cpu.h"

#include <filament/Engine.h>
#include <filament/VertexBuffer.h>

#include <math/mat3.h>
#include <math/norm.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <cmath>
#include <memory>
#include <span>

namespace heaven {

inline void upload_grass_tangent_frames(
        filament::Engine& engine,
        filament::VertexBuffer& vertex_buffer,
        std::span<const GrassVertex> vertices) {
    using filament::math::float3;
    using filament::math::mat3f;
    using filament::math::packSnorm16;
    using filament::math::short4;

    auto frames = std::make_unique<short4[]>(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const GrassVertex& vertex = vertices[i];
        float3 normal{vertex.nx, vertex.ny, vertex.nz};
        if (dot(normal, normal) < 1.0e-8f) {
            normal = {0.0f, 0.0f, 1.0f};
        } else {
            normal = normalize(normal);
        }

        // Pick a reference axis that is not parallel to the blade normal.
        // No normal map is used, so tangent orientation is arbitrary; only the
        // frame's normal column affects Filament's lighting.
        const float3 reference = std::abs(normal.z) < 0.999f
            ? float3{0.0f, 0.0f, 1.0f}
            : float3{0.0f, 1.0f, 0.0f};
        const float3 tangent = normalize(cross(reference, normal));
        const float3 bitangent = cross(normal, tangent);
        const auto quaternion =
            mat3f::packTangentFrame(mat3f{tangent, bitangent, normal});
        frames[i] = packSnorm16(quaternion.xyzw);
    }

    vertex_buffer.setBufferAt(
        engine, 1,
        filament::VertexBuffer::BufferDescriptor(
            frames.release(), vertices.size() * sizeof(short4),
            [](void* buffer, size_t, void*) {
                delete[] static_cast<short4*>(buffer);
            }));
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_GRASS_TANGENT_UPLOAD_H_
