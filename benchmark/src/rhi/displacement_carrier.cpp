#include "rhi/displacement_carrier.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <unordered_map>

namespace heaven {
namespace {
filament::math::float3 float3(glm::dvec3 v) {
    return {float(v.x), float(v.y), float(v.z)};
}
filament::math::float4 float4(glm::dvec3 v, float w) {
    return {float(v.x), float(v.y), float(v.z), w};
}
bool finite(glm::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
} // namespace

Result<DisplacementCarrier> make_displacement_carrier(
        std::span<const Vertex> original_vertices,
        const PreparedDisplacementSurface& prepared) {
    if (!prepared.ready() || original_vertices.size() != prepared.source_vertices.size() ||
            prepared.geometry.nodes.size() < original_vertices.size() ||
            !std::isfinite(prepared.displacement.amplitude) ||
            prepared.displacement.amplitude <= 0 ||
            prepared.original_packed.tangent_frames.size() != original_vertices.size()) {
        return std::unexpected(Error{"Displacement carrier requires a prepared static source"});
    }
    try {
        const auto& geometry = prepared.geometry;
        const std::size_t node_count = geometry.nodes.size();
        if (node_count > std::numeric_limits<std::uint32_t>::max() ||
                geometry.triangles.size() > std::numeric_limits<std::uint32_t>::max() / 3)
            return std::unexpected(Error{"Displacement carrier exceeds the GPU index domain"});

        // Compute shared neutral/shading attributes once. Original corner
        // records retain exactly the original upload's packed tangent bytes.
        std::vector<Vertex> neutral(node_count);
        for (std::size_t i = 0; i < node_count; ++i) {
            const auto& node = geometry.nodes[i];
            if (i < original_vertices.size()) {
                neutral[i] = original_vertices[i];
                continue;
            }
            if (node.source_parent >= prepared.source_triangles.size() ||
                    !finite(node.source_barycentric))
                return std::unexpected(Error{"Displacement carrier has invalid node ancestry"});
            const auto ids = prepared.source_triangles[node.source_parent];
            glm::dvec3 p{}, n{};
            glm::dvec2 uv{}, uv1{};
            double magnitude = 0;
            for (std::size_t j = 0; j < 3; ++j) {
                if (ids[j] >= original_vertices.size())
                    return std::unexpected(Error{"Displacement carrier source index is invalid"});
                const auto& v = original_vertices[ids[j]];
                const double b = node.source_barycentric[static_cast<int>(j)];
                p += b * glm::dvec3(v.px, v.py, v.pz);
                n += b * glm::dvec3(v.nx, v.ny, v.nz);
                uv += b * glm::dvec2(v.u, v.v);
                uv1 += b * glm::dvec2(v.u1, v.v1);
                magnitude += b * v.displacement_normal_scale;
            }
            auto& v = neutral[i];
            v.px = float(p.x); v.py = float(p.y); v.pz = float(p.z);
            v.nx = float(n.x); v.ny = float(n.y); v.nz = float(n.z);
            v.u = float(uv.x); v.v = float(uv.y);
            v.u1 = float(uv1.x); v.v1 = float(uv1.y);
            v.displacement_normal_scale = float(magnitude);
        }
        std::vector<std::uint32_t> shared_indices;
        shared_indices.reserve(geometry.triangles.size() * 3);
        for (const auto& triangle : geometry.triangles) {
            for (const auto node : triangle.nodes) {
                if (node >= node_count)
                    return std::unexpected(Error{"Displacement carrier refined index is invalid"});
                shared_indices.push_back(node);
            }
        }
        auto shared = pack_static_surface(neutral, shared_indices);
        if (!shared) return std::unexpected(shared.error());
        for (std::size_t i = 0; i < original_vertices.size(); ++i) {
            shared->tangent_frames[i] = prepared.original_packed.tangent_frames[i];
            shared->displacement_custom[i] = prepared.original_packed.displacement_custom[i];
            shared->footprints[i] = prepared.original_packed.footprints[i];
        }
        // Refinement must never change the immutable mesh-LOD field. Although
        // the prepared branch uses original parent H, keep the source field in
        // CUSTOM0 rather than recalculating it from the finer topology.
        for (std::size_t i = original_vertices.size(); i < node_count; ++i) {
            const auto& node = geometry.nodes[i];
            const auto ids = prepared.source_triangles[node.source_parent];
            glm::dvec2 footprint{};
            for (std::size_t j = 0; j < 3; ++j)
                footprint += node.source_barycentric[static_cast<int>(j)] *
                        glm::dvec2(prepared.original_packed.footprints[ids[j]][0],
                                   prepared.original_packed.footprints[ids[j]][1]);
            shared->footprints[i] = {float(footprint.x), float(footprint.y)};
            shared->displacement_custom[i] = {neutral[i].displacement_normal_scale,
                    float(footprint.x), float(footprint.y), 0};
        }

        DisplacementCarrier result;
        const auto append = [&](std::uint32_t node_id, std::uint32_t parent,
                                glm::dvec3 barycentric) -> Result<std::uint32_t> {
            if (parent >= prepared.source_triangles.size() || !finite(barycentric) ||
                    std::min({barycentric.x, barycentric.y, barycentric.z}) < -1e-12 ||
                    std::abs(barycentric.x + barycentric.y + barycentric.z - 1) > 1e-10 ||
                    result.parents.size() >= std::numeric_limits<std::uint32_t>::max())
                return std::unexpected(Error{"Displacement carrier has invalid parent weights"});
            const auto ids = prepared.source_triangles[parent];
            DisplacementParentAttributes attributes{};
            for (std::size_t j = 0; j < 3; ++j) {
                if (ids[j] >= prepared.source_vertices.size())
                    return std::unexpected(Error{"Displacement carrier parent index is invalid"});
                const auto& source = prepared.source_vertices[ids[j]];
                attributes[j] = float4(source.neutral_position, float(barycentric[static_cast<int>(j)]));
                const auto d = source.displacement_direction *
                        (double(source.height) - 127.0 / 255.0) *
                        double(source.displacement_magnitude);
                attributes[j + 3] = float4(d, 0);
            }
            attributes[6] = float4(geometry.nodes[node_id].correction,
                                   prepared.displacement.amplitude);
            if (!finite(geometry.nodes[node_id].correction) ||
                    glm::length(geometry.nodes[node_id].correction) > 0.02000001)
                return std::unexpected(Error{"Displacement carrier exceeds its world movement bound"});
            const auto id = static_cast<std::uint32_t>(result.parents.size());
            result.parents.push_back(attributes);
            result.packed.positions.push_back(shared->positions[node_id]);
            result.packed.uv0.push_back(shared->uv0[node_id]);
            result.packed.uv1.push_back(shared->uv1[node_id]);
            result.packed.tangent_frames.push_back(shared->tangent_frames[node_id]);
            result.packed.displacement_custom.push_back(shared->displacement_custom[node_id]);
            return id;
        };
        std::unordered_map<std::uint64_t, std::uint32_t> attachments;
        attachments.reserve(node_count + original_vertices.size() * 3);
        result.indices.reserve(shared_indices.size());
        for (const auto& triangle : geometry.triangles) {
            for (std::size_t j = 0; j < 3; ++j) {
                const auto node = triangle.nodes[j];
                const auto barycentric = triangle.source_barycentric[j];
                if (!finite(barycentric) ||
                        std::min({barycentric.x, barycentric.y, barycentric.z}) < -1e-12 ||
                        std::abs(barycentric.x + barycentric.y + barycentric.z - 1) > 1e-10)
                    return std::unexpected(Error{"Displacement carrier has invalid parent weights"});
                const std::uint64_t key = (std::uint64_t(triangle.source_parent) << 32) | node;
                auto it = attachments.find(key);
                if (it == attachments.end()) {
                    const auto record = append(node, triangle.source_parent,
                                                triangle.source_barycentric[j]);
                    if (!record) return std::unexpected(record.error());
                    it = attachments.emplace(key, *record).first;
                } else {
                    // A shared node has one attachment per original parent.
                    // Verify the actual FLOAT weights that the shader consumes;
                    // otherwise deduplication could conceal conflicting ancestry.
                    const auto& stored = result.parents[it->second];
                    for (std::size_t k = 0; k < 3; ++k)
                        if (std::bit_cast<std::uint32_t>(stored[k].w) !=
                                std::bit_cast<std::uint32_t>(float(barycentric[static_cast<int>(k)])))
                            return std::unexpected(Error{"Displacement carrier has conflicting shared parent weights"});
                }
                result.indices.push_back(it->second);
            }
        }
        if (result.indices.empty())
            return std::unexpected(Error{"Displacement carrier has no indexed triangles"});
        result.bounds_min = glm::vec3(std::numeric_limits<float>::infinity());
        result.bounds_max = -result.bounds_min;
        double maximum_displacement = 0;
        for (std::size_t i = 0; i < prepared.source_vertices.size(); ++i) {
            const auto& source = prepared.source_vertices[i];
            const glm::vec3 p(source.neutral_position);
            result.bounds_min = glm::min(result.bounds_min, p);
            result.bounds_max = glm::max(result.bounds_max, p);
            maximum_displacement = std::max(maximum_displacement,
                    std::abs((double(source.height) - 127.0 / 255.0) *
                    source.displacement_magnitude * prepared.displacement.amplitude));
        }
        const float model_padding = float((maximum_displacement + .02001) / prepared.uniform_scale);
        result.bounds_min -= glm::vec3(model_padding);
        result.bounds_max += glm::vec3(model_padding);
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{"Displacement carrier allocation failed"});
    }
}
} // namespace heaven
