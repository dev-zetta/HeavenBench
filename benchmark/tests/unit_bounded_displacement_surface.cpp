#include "engine/bounded_displacement_surface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

namespace {

using Point = glm::dvec3;
using Triangle = std::array<std::uint32_t, 3>;

struct Source {
    std::vector<BoundedDisplacementVertex> vertices;
    std::vector<Triangle> triangles;
    [[nodiscard]] BoundedDisplacementInput input() const {
        return {vertices, triangles, 1.0, 0.7};
    }
};

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "[bounded_displacement_surface] %s\n", message);
    return condition;
}

Point original_position(const BoundedDisplacementInput& input, std::uint32_t id) {
    const auto& v = input.vertices[id];
    return input.uniform_scale * v.neutral_position + input.reference_amplitude *
        double(v.displacement_magnitude) * (double(v.height) - 127.0 / 255.0) *
        v.displacement_direction;
}

bool finite(Point p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool equal(Point a, Point b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

Source protected_triangle() {
    Source source;
    for (const Point p : {Point(0, 0, 0), Point(0.25, 0, 0), Point(0, 0.25, 0)}) {
        BoundedDisplacementVertex v;
        v.neutral_position = p;
        v.displacement_direction = {0, 0, 1};
        v.height = 0.75f;
        v.displacement_magnitude = 0.8f;
        v.uv0 = v.uv1 = glm::vec2(p.x, p.y);
        v.footprint = {0.25f, 0.25f};
        source.vertices.push_back(v);
    }
    source.triangles.push_back({0, 1, 2});
    return source;
}

Source curved_grid_with_isolated_triangle() {
    Source source;
    constexpr std::uint32_t side = 9;
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const double u = double(x) / (side - 1), v = double(y) / (side - 1);
            BoundedDisplacementVertex vertex;
            vertex.neutral_position = {0.5 * u - 0.25, 0.5 * v - 0.25, 0};
            vertex.displacement_direction = {0, 0, 1};
            vertex.uv0 = vertex.uv1 = {float(u), float(v)};
            vertex.footprint = {0.13f, 0.13f};
            const double sx = std::sin(std::numbers::pi * u), sy = std::sin(std::numbers::pi * v);
            vertex.height = float(127.0 / 255.0 + 0.08 * sx * sx * sy * sy);
            source.vertices.push_back(vertex);
        }
    }
    for (std::uint32_t y = 0; y + 1 < side; ++y) {
        for (std::uint32_t x = 0; x + 1 < side; ++x) {
            const auto a = y * side + x, b = a + 1, c = a + side, d = c + 1;
            source.triangles.push_back({a, b, d});
            source.triangles.push_back({a, d, c});
        }
    }
    // A disconnected fully protected component must keep its own original
    // triangles, even when a different component has active smooth support.
    const auto isolated = protected_triangle();
    const auto offset = static_cast<std::uint32_t>(source.vertices.size());
    for (auto vertex : isolated.vertices) {
        vertex.neutral_position.x += 2;
        source.vertices.push_back(vertex);
    }
    source.triangles.push_back({offset, offset + 1, offset + 2});
    return source;
}

BoundedDisplacementPolicy unit_policy() {
    BoundedDisplacementPolicy policy;
    policy.maximum_triangles = 12'000;
    policy.maximum_work_bytes = 64ull * 1024 * 1024;
    policy.maximum_patch_evaluations = 2'000'000;
    policy.maximum_sample_depth = 3;
    policy.normal_error_degrees = 2;
    return policy;
}

bool protected_plane_identity() {
    const auto source = protected_triangle();
    auto input = source.input();
    input.uniform_scale = 1.25;
    const auto result = build_bounded_displacement_surface(input, unit_policy());
    if (!check(bool(result), "protected plane returned an error")) return false;
    if (!check(result->status == BoundedDisplacementStatus::Complete &&
            result->nodes.size() == 3 && result->triangles.size() == 1 &&
            result->triangles[0].nodes == source.triangles[0] &&
            result->metrics.native_regular_patches == 0 &&
            result->metrics.native_extraordinary_patches == 0,
            "fully protected plane did not retain its original indexed geometry")) return false;
    for (std::size_t i = 0; i < result->nodes.size(); ++i) {
        const auto& node = result->nodes[i];
        if (!check(equal(node.old_raster_position, original_position(input, std::uint32_t(i))) &&
                equal(node.target_position, node.old_raster_position) && equal(node.correction, Point{}),
                "protected plane changed authoritative displacement or world-scale units")) return false;
    }
    auto plane = curved_grid_with_isolated_triangle();
    for (auto& vertex : plane.vertices) {
        vertex.height = 0.75f;
        vertex.displacement_magnitude = 0.8f;
    }
    auto plane_input = plane.input(); plane_input.uniform_scale = 1.25;
    const auto flat = build_bounded_displacement_surface(plane_input, unit_policy());
    if (!check(bool(flat) && flat->status == BoundedDisplacementStatus::Complete &&
            flat->nodes.size() == plane.vertices.size() && flat->triangles.size() == plane.triangles.size(),
            "large planar support did not retain its original representation")) return false;
    for (std::size_t i = 0; i < flat->nodes.size(); ++i)
        if (!check(equal(flat->nodes[i].old_raster_position, original_position(plane_input, std::uint32_t(i))) &&
                equal(flat->nodes[i].target_position, flat->nodes[i].old_raster_position) &&
                equal(flat->nodes[i].correction, Point{}), "large planar support acquired a geometry correction")) return false;
    for (std::size_t i = 0; i < flat->triangles.size(); ++i)
        if (!check(flat->triangles[i].nodes == plane.triangles[i], "large planar support changed its topology")) return false;
    return true;
}

// Independently inspect the final indexed output: world movement, actual
// triangle winding, parent-domain coverage and a two-component Euler count.
// These assertions use the public source/mesh, not private native patch masks.
bool inspect_curved_surface(const Source& source, const BoundedDisplacementSurface& surface,
        const BoundedDisplacementPolicy& policy) {
    const auto input = source.input();
    if (!check((surface.status == BoundedDisplacementStatus::Complete ||
                surface.status == BoundedDisplacementStatus::BudgetLimited) &&
            surface.metrics.orientation_failures == 0 &&
            surface.nodes.size() >= input.vertices.size() &&
            surface.triangles.size() <= policy.maximum_triangles &&
            surface.metrics.native_regular_patches > 0 &&
            surface.metrics.native_extraordinary_patches > 0,
            "curved support did not produce a safe explicit native result")) return false;
    bool moved = false;
    for (const auto& node : surface.nodes) {
        if (!check(finite(node.old_raster_position) && finite(node.target_position) && finite(node.correction) &&
                glm::length(node.correction) <= policy.correspondence_bound + 1e-10 &&
                glm::length(node.target_position - node.old_raster_position - node.correction) < 1e-12,
                "curved support violated the world movement contract")) return false;
        moved |= glm::length(node.correction) > 1e-6;
    }
    if (!check(moved, "curved fixture never exercised a nonzero geometry correction")) return false;

    struct Edge { unsigned count = 0; int direction = 0; Point a{}, b{}; };
    std::map<std::pair<std::uint32_t, std::uint32_t>, Edge> edges;
    std::set<std::uint32_t> used;
    std::vector<double> coverage(input.triangles.size());
    unsigned isolated_triangles = 0;
    const auto isolated_parent = static_cast<std::uint32_t>(input.triangles.size() - 1);
    for (const auto& triangle : surface.triangles) {
        if (!check(triangle.source_parent < input.triangles.size(), "output parent index is invalid")) return false;
        const auto parent = input.triangles[triangle.source_parent];
        std::array<Point, 3> original{}, target{}, neutral{};
        for (std::size_t k = 0; k < 3; ++k) {
            const auto node = triangle.nodes[k];
            const auto bary = triangle.source_barycentric[k];
            if (!check(node < surface.nodes.size() && finite(bary) &&
                    std::min({bary.x, bary.y, bary.z}) >= -1e-12 &&
                    std::abs(bary.x + bary.y + bary.z - 1) < 1e-10,
                    "output corner ancestry is invalid")) return false;
            used.insert(node);
            for (std::size_t j = 0; j < 3; ++j) {
                original[k] += bary[static_cast<int>(j)] * original_position(input, parent[j]);
                neutral[k] += bary[static_cast<int>(j)] * input.vertices[parent[j]].neutral_position;
            }
            target[k] = surface.nodes[node].target_position;
            if (!check(glm::length(original[k] - surface.nodes[node].old_raster_position) < 1e-10,
                    "shared node no longer corresponds to its original displaced parent")) return false;
        }
        const auto old_normal = glm::cross(original[1] - original[0], original[2] - original[0]);
        const auto new_normal = glm::cross(target[1] - target[0], target[2] - target[0]);
        if (!check(glm::dot(old_normal, new_normal) > 0 && glm::length(new_normal) > 1e-15,
                "curved output has a reversed or degenerate actual triangle")) return false;
        const auto a = triangle.source_barycentric[1] - triangle.source_barycentric[0];
        const auto b = triangle.source_barycentric[2] - triangle.source_barycentric[0];
        coverage[triangle.source_parent] += a.y * b.z - a.z * b.y;
        for (std::size_t k = 0; k < 3; ++k) {
            const auto next = (k + 1) % 3;
            const auto left = triangle.nodes[k], right = triangle.nodes[next];
            auto& edge = edges[std::minmax(left, right)];
            ++edge.count;
            edge.direction += left < right ? 1 : -1;
            edge.a = neutral[k]; edge.b = neutral[next];
        }
        if (triangle.source_parent == isolated_parent) {
            ++isolated_triangles;
            if (!check(triangle.nodes == input.triangles[isolated_parent],
                    "inactive disconnected component was replaced or welded")) return false;
        }
    }
    if (!check(isolated_triangles == 1 &&
            static_cast<std::int64_t>(used.size()) - static_cast<std::int64_t>(edges.size()) +
                static_cast<std::int64_t>(surface.triangles.size()) == 2,
            "disconnected output lost its two disk components")) return false;
    for (const auto& [key, edge] : edges) {
        if (!check(edge.count == 1 || (edge.count == 2 && edge.direction == 0),
                "output edge incidence or adjacent winding is nonconforming")) return false;
        if (edge.count == 1 && edge.a.x < 1 && edge.b.x < 1) {
            const bool outer = (std::abs(std::abs(edge.a.x) - 0.25) < 1e-12 &&
                    std::abs(edge.a.x - edge.b.x) < 1e-12) ||
                (std::abs(std::abs(edge.a.y) - 0.25) < 1e-12 &&
                    std::abs(edge.a.y - edge.b.y) < 1e-12);
            if (!check(outer, "curved grid has a hanging edge or interior opening")) return false;
        }
    }
    for (double area : coverage)
        if (!check(std::abs(area - 1) < 1e-9, "output does not cover an entire original parent")) return false;
    for (std::uint32_t i = 81; i < 84; ++i)
        if (!check(equal(surface.nodes[i].target_position, original_position(input, i)) &&
                equal(surface.nodes[i].correction, Point{}), "disconnected protected corner moved")) return false;
    if (surface.status == BoundedDisplacementStatus::BudgetLimited)
        return check(surface.metrics.sampled_quality_failures > 0,
                "budget-limited result concealed its unmet approximation criterion");
    return check(surface.metrics.sampled_quality_failures == 0,
            "complete result retained unmet approximation criteria");
}

bool deterministic(const BoundedDisplacementSurface& a, const BoundedDisplacementSurface& b) {
    if (a.status != b.status || a.nodes.size() != b.nodes.size() || a.triangles.size() != b.triangles.size() ||
            a.component_fallbacks.size() != b.component_fallbacks.size()) return false;
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        const auto& x = a.nodes[i]; const auto& y = b.nodes[i];
        if (!equal(x.old_raster_position, y.old_raster_position) || !equal(x.target_position, y.target_position) ||
                !equal(x.correction, y.correction) || x.source_parent != y.source_parent ||
                !equal(x.source_barycentric, y.source_barycentric)) return false;
    }
    for (std::size_t i = 0; i < a.triangles.size(); ++i) {
        const auto& x = a.triangles[i]; const auto& y = b.triangles[i];
        if (x.nodes != y.nodes || x.source_parent != y.source_parent) return false;
        for (std::size_t k = 0; k < 3; ++k)
            if (!equal(x.source_barycentric[k], y.source_barycentric[k])) return false;
    }
    for (std::size_t i = 0; i < a.component_fallbacks.size(); ++i) {
        const auto& x = a.component_fallbacks[i]; const auto& y = b.component_fallbacks[i];
        if (x.reason != y.reason || x.source_parents != y.source_parents || x.diagnostic != y.diagnostic) return false;
    }
    return a.metrics.patch_evaluations == b.metrics.patch_evaluations &&
        a.metrics.sampled_quality_failures == b.metrics.sampled_quality_failures &&
        a.metrics.maximum_target_bound == b.metrics.maximum_target_bound &&
        a.metrics.accepted_components == b.metrics.accepted_components &&
        a.metrics.rejected_components == b.metrics.rejected_components;
}

bool curved_support_and_budget() {
    const auto source = curved_grid_with_isolated_triangle();
    const auto input = source.input();
    const auto policy = unit_policy();
    const auto first = build_bounded_displacement_surface(input, policy);
    if (!check(bool(first), first ? "" : first.error().message.c_str()) ||
            !inspect_curved_surface(source, *first, policy)) return false;
    const auto second = build_bounded_displacement_surface(input, policy);
    if (!check(bool(second) && deterministic(*first, *second),
            "repeated builder input produced different shared geometry or ancestry")) return false;

    auto unmet_quality = policy;
    unmet_quality.maximum_sample_depth = 0;
    unmet_quality.position_error = 1e-9;
    unmet_quality.normal_error_degrees = 1e-5;
    const auto unresolved = build_bounded_displacement_surface(input, unmet_quality);
    if (!check(bool(unresolved) && unresolved->status == BoundedDisplacementStatus::BudgetLimited &&
            unresolved->metrics.sampled_quality_failures > 0,
            "unmet approximation quality was accepted as complete") ||
            !inspect_curved_surface(source, *unresolved, unmet_quality)) return false;

    auto low_budget = policy;
    low_budget.maximum_triangles = static_cast<std::uint32_t>(input.triangles.size());
    const auto limited = build_bounded_displacement_surface(input, low_budget);
    if (!check(bool(limited) && limited->status == BoundedDisplacementStatus::BudgetLimited &&
            limited->triangles.size() == input.triangles.size() &&
            limited->metrics.minimum_conforming_triangles == input.triangles.size() &&
            limited->metrics.sampled_quality_failures > 0,
            "original-parent budget did not return an explicit quality-limited mesh") ||
            !inspect_curved_surface(source, *limited, low_budget)) return false;
    --low_budget.maximum_triangles;
    if (!check(!build_bounded_displacement_surface(input, low_budget),
            "budget below the original-parent floor produced partial geometry")) return false;
    std::printf("[bounded_displacement_surface] curved %zu triangles, movement %.6g m; "
                "quality-limited %zu triangles, %llu misses; original-parent floor verified\n",
            first->triangles.size(), first->metrics.maximum_sampled_movement,
            unresolved->triangles.size(), static_cast<unsigned long long>(unresolved->metrics.sampled_quality_failures));
    return true;
}

bool incompatible_data() {
    auto source = protected_triangle();
    source.vertices[1].displacement_direction = {0, 0, 0};
    if (!check(!build_bounded_displacement_surface(source.input(), unit_policy()),
            "nonunit authoritative displacement direction was accepted")) return false;
    source = protected_triangle();
    source.vertices[1].height = std::numeric_limits<float>::quiet_NaN();
    if (!check(!build_bounded_displacement_surface(source.input(), unit_policy()),
            "nonfinite native height was accepted")) return false;
    source = protected_triangle();
    source.triangles[0][1] = 999;
    if (!check(!build_bounded_displacement_surface(source.input(), unit_policy()),
            "invalid source index was accepted")) return false;
    source = protected_triangle();
    auto input = source.input(); input.uniform_scale = -1;
    return check(!build_bounded_displacement_surface(input, unit_policy()),
            "improper canonical instance metric was accepted");
}

bool rejected_component_retains_original_geometry() {
    Source source;
    // Both components use identical neutral topology and native fields. Only
    // their relief amplitude differs: one target fits the 1mm envelope while
    // the other must return its original physical triangles.
    for (std::uint32_t part = 0; part < 2; ++part) {
        const auto first = static_cast<std::uint32_t>(source.vertices.size());
        for (std::uint32_t y = 0; y < 9; ++y) {
            for (std::uint32_t x = 0; x < 9; ++x) {
                const double u = x / 8.0, v = y / 8.0;
                BoundedDisplacementVertex vertex;
                vertex.neutral_position = {part + 0.5 * (u - 0.5), 0.5 * (v - 0.5), 0};
                vertex.displacement_direction = {0, 0, 1};
                vertex.uv0 = vertex.uv1 = {float(u), float(v)};
                vertex.footprint = {0.13f, 0.13f};
                const double s = std::sin(std::numbers::pi * u) * std::sin(std::numbers::pi * v);
                vertex.height = float(127.0 / 255.0 + (part ? 0.2 : 0.0005) * s * s);
                source.vertices.push_back(vertex);
            }
        }
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                const auto a = first + y * 9 + x, b = a + 1, c = a + 9, d = c + 1;
                source.triangles.push_back({a, b, c});
                source.triangles.push_back({b, d, c});
            }
        }
    }
    auto policy = unit_policy();
    policy.correspondence_bound = 0.001;
    policy.initial_node_bound = 0.000999;
    policy.maximum_triangles = 32'768;
    const auto input = source.input();
    const auto result = build_bounded_displacement_surface(input, policy);
    if (!check(bool(result), "mixed safe/rejected components returned an error")) return false;
    const auto& m = result->metrics;
    if (!check(result->status == BoundedDisplacementStatus::BudgetLimited &&
            m.accepted_components == 1 && m.rejected_components == 1 &&
            m.rejected_bound_components == 1 && m.rejected_source_triangles == 128 &&
            m.orientation_failures == 0 && m.sampled_quality_failures == 0 &&
            m.maximum_target_bound <= policy.correspondence_bound &&
            m.maximum_rejected_target_bound > policy.correspondence_bound &&
            result->component_fallbacks.size() == 1,
            "rejected-target diagnostics contaminated accepted integrity or were concealed")) return false;
    const auto& fallback = result->component_fallbacks[0];
    const std::set<std::uint32_t> parents(fallback.source_parents.begin(), fallback.source_parents.end());
    if (!check(fallback.reason == BoundedDisplacementStatus::TargetBoundFailed &&
            !fallback.diagnostic.empty() && parents.size() == 128 &&
            *parents.begin() == 128 && *parents.rbegin() == 255,
            "fallback does not identify the entire rejected source component")) return false;
    std::uint32_t retained = 0, refined = 0;
    double accepted_movement = 0;
    for (const auto& triangle : result->triangles) {
        if (!check(triangle.source_parent < input.triangles.size(), "mixed output parent out of range")) return false;
        if (triangle.source_parent >= 128) {
            ++retained;
            if (!check(triangle.nodes == source.triangles[triangle.source_parent],
                    "rejected component changed its original ordered triangles")) return false;
        } else ++refined;
        std::array<Point, 3> original{}, target{};
        const auto source_triangle = source.triangles[triangle.source_parent];
        for (std::size_t k = 0; k < 3; ++k) {
            if (!check(triangle.nodes[k] < result->nodes.size(), "mixed output node out of range")) return false;
            const auto& node = result->nodes[triangle.nodes[k]];
            const auto bary = triangle.source_barycentric[k];
            for (std::size_t j = 0; j < 3; ++j)
                original[k] += bary[static_cast<int>(j)] * original_position(input, source_triangle[j]);
            target[k] = node.target_position;
            if (!check(glm::length(node.correction) <= policy.correspondence_bound + 1e-10 &&
                    glm::length(original[k] - node.old_raster_position) < 1e-10,
                    "mixed output violated authoritative lineage or movement")) return false;
            if (triangle.source_parent < 128)
                accepted_movement = std::max(accepted_movement, glm::length(node.correction));
        }
        if (!check(glm::dot(glm::cross(original[1] - original[0], original[2] - original[0]),
                glm::cross(target[1] - target[0], target[2] - target[0])) > 0,
                "mixed output contains a physical winding reversal")) return false;
    }
    for (std::uint32_t i = 81; i < 162; ++i)
        if (!check(equal(result->nodes[i].target_position, original_position(input, i)) &&
                equal(result->nodes[i].correction, Point{}), "rejected component failed exact physical identity")) return false;
    if (!check(retained == 128 && refined >= 128 && accepted_movement > 1e-6,
            "component fallback also discarded safe smoothing work")) return false;
    const auto repeated = build_bounded_displacement_surface(input, policy);
    return check(bool(repeated) && deterministic(*result, *repeated),
            "component rejection/fallback is nondeterministic");
}

bool bounded_displacement_surface_tests() {
    const bool identity = protected_plane_identity();
    const bool curved = curved_support_and_budget();
    const bool invalid = incompatible_data();
    const bool fallback = rejected_component_retains_original_geometry();
    return identity && curved && invalid && fallback;
}

} // namespace

const Register bounded_displacement_surface{bounded_displacement_surface_tests};

} // namespace heaven::testing
