/* HeavenBench — material-resolved startup preparation, never per-frame work. */
#include "rhi/displacement_surface_preparer.h"

#include <filament/Texture.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <tuple>

namespace heaven {
namespace {
template<class V> bool finite3(const V& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool valid_policy(const DisplacementPreparationPolicy& p) {
    return p.maximum_source_vertices > 0 &&
            std::isfinite(p.geometry.correspondence_bound) &&
            p.geometry.correspondence_bound > 0 &&
            std::isfinite(p.geometry.initial_node_bound) && p.geometry.initial_node_bound >= 0 &&
            std::isfinite(p.geometry.position_error) && p.geometry.position_error > 0 &&
            std::isfinite(p.uniform_metric_tolerance) && p.uniform_metric_tolerance >= 0 &&
            p.uniform_metric_tolerance <= 1e-4 &&
            std::isfinite(p.sharing_scale_tolerance) && p.sharing_scale_tolerance >= 0 &&
            p.sharing_scale_tolerance <= 1e-4;
}
bool matches_original_source(const StaticDisplacementSurfaceRequest& r) {
    if (!r.uploaded_source || r.vertices.empty() || r.indices.empty() || r.indices.size() % 3)
        return false;
    const auto& p = *r.uploaded_source;
    const auto n = r.vertices.size();
    if (!p.displacement_directions_valid || p.positions.size() != n || p.uv0.size() != n ||
            p.uv1.size() != n || p.tangent_frames.size() != n || p.footprints.size() != n ||
            p.displacement_custom.size() != n || p.unit_displacement_directions.size() != n ||
            p.source_signature != packed_static_source_signature(r.vertices, r.indices))
        return false;
    for (const auto index : r.indices) if (index >= n) return false;
    const auto same = [](float a, float b) {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const auto& v = r.vertices[i];
        const auto& pos = p.positions[i];
        const auto& custom = p.displacement_custom[i];
        const auto decoded = decode_static_displacement_direction(p.tangent_frames[i]);
        if (!same(pos.x, v.px) || !same(pos.y, v.py) || !same(pos.z, v.pz) ||
                !same(p.uv0[i].x, v.u) || !same(p.uv0[i].y, v.v) ||
                !same(p.uv1[i].x, v.u1) || !same(p.uv1[i].y, v.v1) ||
                !same(custom.x, v.displacement_normal_scale) ||
                !same(custom.y, p.footprints[i][0]) || !same(custom.z, p.footprints[i][1]) ||
                custom.w != 0 || !std::isfinite(custom.x) || custom.x < 0 ||
                !std::isfinite(custom.y) || custom.y < 0 ||
                !std::isfinite(custom.z) || custom.z < 0 || !decoded)
            return false;
        const auto& unit = p.unit_displacement_directions[i];
        if (!same(unit.x, decoded->x) || !same(unit.y, decoded->y) || !same(unit.z, decoded->z))
            return false;
    }
    return true;
}

struct Metric { double scale = 1, error = 0, homogeneous_w = 1; };
Result<Metric> instance_metric(const glm::mat4& m, const DisplacementPreparationPolicy& p) {
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        if (!std::isfinite(m[c][r]))
            return std::unexpected(Error{"Nonfinite instance transform"});
    if (m[0][3] != 0 || m[1][3] != 0 || m[2][3] != 0)
        return std::unexpected(Error{"Projective instance transform is unsupported"});
    const double homogeneous_w = double(m[3][3]);
    if (homogeneous_w <= 0)
        return std::unexpected(Error{"Instance homogeneous W must be positive"});
    const glm::dvec3 x(m[0]), y(m[1]), z(m[2]);
    const double sx = glm::length(x), sy = glm::length(y), sz = glm::length(z);
    const double scale = (sx + sy + sz) / 3;
    if (!std::isfinite(scale) || scale <= 1e-8)
        return std::unexpected(Error{"Singular instance transform is unsupported"});
    const double scale_error = std::max({std::abs(sx / scale - 1),
            std::abs(sy / scale - 1), std::abs(sz / scale - 1)});
    const double dot_error = std::max({std::abs(glm::dot(x, y) / (scale * scale)),
            std::abs(glm::dot(x, z) / (scale * scale)),
            std::abs(glm::dot(y, z) / (scale * scale))});
    const double determinant = glm::dot(x, glm::cross(y, z)) / (scale * scale * scale);
    const double error = std::max({scale_error, dot_error, std::abs(determinant - 1)});
    if (!std::isfinite(error) || determinant <= 0 || error > p.uniform_metric_tolerance)
        return std::unexpected(Error{"Instance is not a proper uniform spatial metric"});
    return Metric{scale, error, homogeneous_w};
}

void set_effective_policy(PreparedDisplacementSurface& source,
        const BoundedDisplacementPolicy& policy) {
    const double homogeneous_bound_scale = std::min(1.0, source.minimum_homogeneous_w);
    source.effective_correspondence_bound = policy.correspondence_bound * homogeneous_bound_scale;
    source.effective_initial_node_bound = policy.initial_node_bound * homogeneous_bound_scale;
    source.effective_position_error = policy.position_error * homogeneous_bound_scale;
}

bool valid_frozen_source(const PreparedDisplacementSurface& source) {
    const auto& packed = source.original_packed;
    const auto n = source.source_vertices.size();
    if (!n || source.source_triangles.empty() || !packed.displacement_directions_valid ||
            packed.positions.size() != n || packed.uv0.size() != n || packed.uv1.size() != n ||
            packed.tangent_frames.size() != n || packed.footprints.size() != n ||
            packed.displacement_custom.size() != n || packed.unit_displacement_directions.size() != n ||
            source.native_height.size() != n)
        return false;
    const auto same = [](float a, float b) {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const auto& v = source.source_vertices[i];
        const auto& p = packed.positions[i];
        const auto& d = packed.unit_displacement_directions[i];
        const auto& custom = packed.displacement_custom[i];
        const auto& h = source.native_height[i];
        const auto decoded = decode_static_displacement_direction(packed.tangent_frames[i]);
        if (!decoded || !same(d.x, decoded->x) || !same(d.y, decoded->y) || !same(d.z, decoded->z) ||
                v.neutral_position != glm::dvec3(p.x, p.y, p.z) ||
                v.displacement_direction != glm::dvec3(d.x, d.y, d.z) ||
                !same(v.uv0.x, packed.uv0[i].x) || !same(v.uv0.y, packed.uv0[i].y) ||
                !same(v.uv1.x, packed.uv1[i].x) || !same(v.uv1.y, packed.uv1[i].y) ||
                !same(v.footprint.x, packed.footprints[i][0]) ||
                !same(v.footprint.y, packed.footprints[i][1]) ||
                !same(custom.y, v.footprint.x) || !same(custom.z, v.footprint.y) ||
                !same(v.displacement_magnitude, custom.x) || !same(v.height, h.height) ||
                h.id != i || !std::isfinite(h.height) || h.height < 0 || h.height > 1 ||
                !std::isfinite(h.mesh_lod) || h.mesh_lod < 1 ||
                !std::isfinite(h.selected_lod) || h.selected_lod != h.mesh_lod)
            return false;
    }
    for (const auto& triangle : source.source_triangles)
        for (const auto index : triangle) if (index >= n) return false;
    return true;
}

// Tokens only declare exact-compatible duplicates. The builder preserves index
// charts and pins these groups; this does not weld topology or approximate UVs.
void assign_compatible_seams(std::vector<BoundedDisplacementVertex>& vertices,
        const PackedStaticSurface& packed) {
    using Key = std::array<std::uint32_t, 15>;
    const auto bits = [](float v) { return std::bit_cast<std::uint32_t>(v); };
    const auto key = [&](std::uint32_t i) {
        const auto& v = vertices[i]; const auto& q = packed.tangent_frames[i];
        return Key{bits(packed.positions[i].x), bits(packed.positions[i].y),
                bits(packed.positions[i].z), bits(v.uv0.x), bits(v.uv0.y),
                bits(v.uv1.x), bits(v.uv1.y), bits(v.displacement_magnitude),
                bits(v.footprint.x), bits(v.footprint.y), bits(v.height),
                std::uint32_t(std::uint16_t(q.x)), std::uint32_t(std::uint16_t(q.y)),
                std::uint32_t(std::uint16_t(q.z)), std::uint32_t(std::uint16_t(q.w))};
    };
    std::vector<std::uint32_t> order(vertices.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return key(a) < key(b); });
    std::uint64_t token = 1;
    for (std::size_t first = 0; first < order.size();) {
        std::size_t end = first + 1;
        while (end < order.size() && key(order[first]) == key(order[end])) ++end;
        if (end - first > 1) {
            for (auto i = first; i < end; ++i) vertices[order[i]].compatible_seam = token;
            ++token;
        }
        first = end;
    }
}

bool valid_output(const PreparedDisplacementSurface& p, double bound) {
    const auto& g = p.geometry;
    const double bound_allowance = 1e-7 * std::min(1.0, p.minimum_homogeneous_w);
    if (g.nodes.size() < p.source_vertices.size() || g.triangles.empty() ||
            g.metrics.orientation_failures != 0 ||
            !std::isfinite(g.metrics.maximum_target_bound) ||
            g.metrics.maximum_target_bound > bound + bound_allowance)
        return false;
    for (const auto& n : g.nodes) {
        if (!finite3(n.old_raster_position) || !finite3(n.target_position) ||
                !finite3(n.correction) || glm::length(n.correction) > bound + bound_allowance ||
                glm::length(n.target_position - n.old_raster_position - n.correction) > 1e-7)
            return false;
    }
    for (const auto& t : g.triangles) {
        if (t.source_parent >= p.source_triangles.size()) return false;
        const auto& ids = p.source_triangles[t.source_parent];
        std::array<glm::dvec3, 3> reference;
        for (int c = 0; c < 3; ++c) {
            if (t.nodes[c] >= g.nodes.size()) return false;
            const auto b = t.source_barycentric[c];
            if (!finite3(b) || std::min({b.x, b.y, b.z}) < -1e-10 ||
                    std::max({b.x, b.y, b.z}) > 1 + 1e-10 ||
                    std::abs(b.x + b.y + b.z - 1) > 1e-10)
                return false;
            glm::dvec3 old(0);
            for (int j = 0; j < 3; ++j) {
                const auto& v = p.source_vertices[ids[j]];
                old += b[j] * (p.uniform_scale * v.neutral_position +
                        double(p.displacement.amplitude) * v.displacement_direction *
                        double(v.displacement_magnitude) * (double(v.height) - 127.0 / 255.0));
            }
            reference[c] = old;
            if (glm::length(old - g.nodes[t.nodes[c]].old_raster_position) > 1e-7)
                return false;
        }
        const auto old_n = glm::cross(reference[1] - reference[0], reference[2] - reference[0]);
        const auto& a = g.nodes[t.nodes[0]].target_position;
        const auto new_n = glm::cross(g.nodes[t.nodes[1]].target_position - a,
                g.nodes[t.nodes[2]].target_position - a);
        if (!finite3(old_n) || !finite3(new_n) || glm::length(old_n) <= 1e-15 ||
                glm::length(new_n) <= 1e-15 || glm::dot(old_n, new_n) <= 0)
            return false;
    }
    return true;
}
} // namespace

Result<PreparedDisplacementSurface> DisplacementSurfacePreparer::sample_source(
        const StaticDisplacementSurfaceRequest& r) const {
    if (!valid_policy(policy_))
        return std::unexpected(Error{"Invalid static displacement preparation policy"});
    try {
        PreparedDisplacementSurface result;
        result.displacement = r.displacement;
        const auto fallback = [&](DisplacementPreparationDisposition disposition, std::string reason) {
            result.disposition = disposition; result.reason = std::move(reason);
            return std::move(result);
        };
        if (r.vertices.size() > policy_.maximum_source_vertices ||
                r.vertices.size() > std::numeric_limits<std::uint32_t>::max())
            return fallback(DisplacementPreparationDisposition::BudgetLimited,
                    "Original source exceeds the bounded native preparation vertex budget");
        if (r.uploaded_source) result.original_packed = *r.uploaded_source;
        if (!matches_original_source(r))
            return fallback(DisplacementPreparationDisposition::InvalidPackedSource,
                    "Original uploaded static packing does not match its immutable source");
        if (!r.displacement.enabled)
            return fallback(DisplacementPreparationDisposition::Disabled, "Geometry displacement is disabled");
        if (!r.static_vertices)
            return fallback(DisplacementPreparationDisposition::NonStatic, "Vertex deformation is unsupported");
        if (r.height_route != DisplacementHeightRoute::TextureLod)
            return fallback(DisplacementPreparationDisposition::UnsupportedHeightRoute,
                    "This material's native height reconstruction route is unsupported");
        if (!r.selected_height || !std::isfinite(r.displacement.amplitude) ||
                r.displacement.amplitude <= 0 || !std::isfinite(r.displacement.power) ||
                r.displacement.power <= 0 || !std::isfinite(r.displacement.fade.threshold) ||
                !std::isfinite(r.displacement.fade.range) ||
                !r.displacement.fade.enabled() ||
                !std::ranges::all_of(r.base_transform, [](float x) { return std::isfinite(x); }))
            return fallback(DisplacementPreparationDisposition::Disabled,
                    "Resolved native texture or effective displacement descriptor is invalid");
        if (r.instance_transforms.empty())
            return fallback(DisplacementPreparationDisposition::UnsupportedInstanceMetric,
                    "All sharing instance metrics must be supplied");
        bool first = true;
        for (const auto& transform : r.instance_transforms) {
            const auto metric = instance_metric(transform, policy_);
            if (!metric)
                return fallback(DisplacementPreparationDisposition::UnsupportedInstanceMetric,
                        metric.error().message);
            if (first) {
                result.uniform_scale = metric->scale;
                result.minimum_homogeneous_w = metric->homogeneous_w;
                first = false;
            }
            else if (std::abs(metric->scale / result.uniform_scale - 1) > policy_.sharing_scale_tolerance)
                return fallback(DisplacementPreparationDisposition::IncompatibleInstanceScale,
                        "Sharing instances have incompatible uniform scales");
            result.maximum_uniform_metric_error = std::max(result.maximum_uniform_metric_error, metric->error);
            result.minimum_homogeneous_w = std::min(result.minimum_homogeneous_w, metric->homogeneous_w);
        }
        set_effective_policy(result, policy_.geometry);
        std::vector<HeightSampleQuery> queries;
        queries.reserve(r.vertices.size());
        const auto& packed = result.original_packed;
        for (std::size_t i = 0; i < r.vertices.size(); ++i) {
            const auto& uv = packed.uv0[i]; const auto& f = packed.footprints[i];
            const float x = f[0] * std::abs(r.base_transform[0]) * float(r.selected_height->getWidth());
            const float y = f[1] * std::abs(r.base_transform[1]) * float(r.selected_height->getHeight());
            const float lod = std::log2(std::max({x, y, 1.0f}));
            if (!std::isfinite(lod) || lod < 1)
                return fallback(DisplacementPreparationDisposition::MeshLodBelowOne,
                        "Original native height remains distance-LOD dependent");
            queries.push_back({std::uint64_t(i), {uv.x, uv.y}, f});
        }
        auto heights = height_batch_.sample(r.selected_height, queries, r.base_transform);
        if (!heights)
            return fallback(DisplacementPreparationDisposition::NativeHeightFailed, heights.error().message);
        if (heights->size() != r.vertices.size())
            return fallback(DisplacementPreparationDisposition::NativeHeightFailed, "Incomplete native height table");
        result.native_height = std::move(*heights);
        result.source_vertices.reserve(r.vertices.size());
        for (std::size_t i = 0; i < r.vertices.size(); ++i) {
            const auto& h = result.native_height[i];
            if (h.id != i || !std::isfinite(h.height) || h.height < 0 || h.height > 1 ||
                    !std::isfinite(h.mesh_lod) || h.mesh_lod < 1 ||
                    !std::isfinite(h.selected_lod) || h.selected_lod != h.mesh_lod)
                return fallback(DisplacementPreparationDisposition::NativeHeightFailed,
                        "Native height readback identity/value/LOD contract failed");
            const auto& pos = packed.positions[i]; const auto& direction = packed.unit_displacement_directions[i];
            result.source_vertices.push_back({{pos.x, pos.y, pos.z},
                    {direction.x, direction.y, direction.z},
                    {packed.uv0[i].x, packed.uv0[i].y}, {packed.uv1[i].x, packed.uv1[i].y},
                    {packed.footprints[i][0], packed.footprints[i][1]},
                    packed.displacement_custom[i].x, h.height, 0});
        }
        assign_compatible_seams(result.source_vertices, packed);
        result.source_triangles.resize(r.indices.size() / 3);
        for (std::size_t i = 0; i < result.source_triangles.size(); ++i)
            result.source_triangles[i] = {r.indices[3 * i], r.indices[3 * i + 1], r.indices[3 * i + 2]};
        result.source_ready = true;
        result.disposition = DisplacementPreparationDisposition::SourceSampled;
        result.reason = "Validated original packing and native height are frozen; geometry is not refined";
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{"Static displacement source sampling allocation failed"});
    }
}

Result<PreparedDisplacementSurface> DisplacementSurfacePreparer::refine_source(
        PreparedDisplacementSurface result) const {
    if (!valid_policy(policy_))
        return std::unexpected(Error{"Invalid static displacement preparation policy"});
    if (!result.source_ready)
        return std::unexpected(Error{"Static displacement refinement requires a frozen sampled source"});
    try {
        result.geometry = {};
        result.geometry_integrity_valid = false;
        result.requested_quality_met = false;
        const auto fallback = [&](std::string reason) {
            result.disposition = DisplacementPreparationDisposition::OperatorRejected;
            result.reason = std::move(reason);
            return std::move(result);
        };
        if (!std::isfinite(result.uniform_scale) || result.uniform_scale <= 0 ||
                !std::isfinite(result.minimum_homogeneous_w) || result.minimum_homogeneous_w <= 0 ||
                !result.displacement.enabled || !std::isfinite(result.displacement.amplitude) ||
                result.displacement.amplitude <= 0 ||
                result.source_vertices.size() > policy_.maximum_source_vertices || !valid_frozen_source(result))
            return fallback("Frozen native source contract is invalid");
        set_effective_policy(result, policy_.geometry);
        auto geometry_policy = policy_.geometry;
        geometry_policy.correspondence_bound = result.effective_correspondence_bound;
        geometry_policy.initial_node_bound = result.effective_initial_node_bound;
        geometry_policy.position_error = result.effective_position_error;
        auto geometry = build_bounded_displacement_surface({result.source_vertices,
                result.source_triangles, result.uniform_scale, double(result.displacement.amplitude)}, geometry_policy);
        if (!geometry)
            return fallback(geometry.error().message);
        result.geometry = std::move(*geometry);
        const bool acceptable_status = result.geometry.status == BoundedDisplacementStatus::Complete ||
                result.geometry.status == BoundedDisplacementStatus::BudgetLimited;
        result.geometry_integrity_valid = acceptable_status &&
                valid_output(result, result.effective_correspondence_bound);
        result.requested_quality_met = result.geometry.status == BoundedDisplacementStatus::Complete &&
                result.geometry.metrics.sampled_quality_failures == 0;
        if (!result.geometry_integrity_valid)
            return fallback("CPU target/sampling failed integrity, correspondence, lineage or orientation");
        result.disposition = result.requested_quality_met
                ? DisplacementPreparationDisposition::Prepared : DisplacementPreparationDisposition::BudgetLimited;
        result.reason = result.requested_quality_met ? "Prepared under the operator's declared quality criterion"
                : "Integrity passes; requested approximation quality remains unmet and must be reported";
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{"Static displacement refinement allocation failed"});
    }
}

Result<PreparedDisplacementSurface> DisplacementSurfacePreparer::prepare(
        const StaticDisplacementSurfaceRequest& request) const {
    auto source = sample_source(request);
    if (!source || !source->source_ready) return source;
    return refine_source(std::move(*source));
}
} // namespace heaven
