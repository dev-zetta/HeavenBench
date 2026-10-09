#include "engine/displacement_surface_priority.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace heaven {
namespace {
constexpr double radians_to_degrees = 180.0 / std::numbers::pi;

bool finite(const glm::dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct Face {
    glm::dvec3 normal{};
    double area = 0.0;
    double minimum_angle = 180.0;
    bool degenerate = true;
};

Face face(const std::array<glm::dvec3, 3>& p) {
    Face f;
    const glm::dvec3 e0 = p[1] - p[0], e1 = p[2] - p[0], e2 = p[2] - p[1];
    const glm::dvec3 cross = glm::cross(e0, e1);
    const double cross_length = glm::length(cross);
    const double maximum_edge_square = std::max({glm::dot(e0, e0),
        glm::dot(e1, e1), glm::dot(e2, e2)});
    if (!std::isfinite(cross_length) || !std::isfinite(maximum_edge_square) ||
        maximum_edge_square <= 0.0 ||
        cross_length <= maximum_edge_square * 64.0 * std::numeric_limits<double>::epsilon())
        return f;
    f.degenerate = false;
    f.area = cross_length * 0.5;
    f.normal = cross / cross_length;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto a = p[(i + 1) % 3] - p[i];
        const auto b = p[(i + 2) % 3] - p[i];
        f.minimum_angle = std::min(f.minimum_angle,
            std::atan2(cross_length, glm::dot(a, b)) * radians_to_degrees);
    }
    return f;
}

double angle(const glm::dvec3& a, const glm::dvec3& b) {
    return std::atan2(glm::length(glm::cross(a, b)),
        std::clamp(glm::dot(a, b), -1.0, 1.0));
}

void histogram(DisplacementPriorityHistogram& h, double degrees, double area) {
    const auto upper = std::lower_bound(kDisplacementPriorityAngleBins.begin(),
        kDisplacementPriorityAngleBins.end(), degrees);
    const auto bin = std::min<std::size_t>(std::distance(
        kDisplacementPriorityAngleBins.begin(), upper), h.triangles.size() - 1);
    ++h.triangles[bin];
    h.neutral_area[bin] += area;
}
} // namespace

Result<DisplacementSurfacePriority> analyze_displacement_surface_priority(
        const BoundedDisplacementInput& input) {
    if (input.vertices.empty() || input.triangles.empty() ||
        !std::isfinite(input.uniform_scale) || input.uniform_scale <= 0.0 ||
        !std::isfinite(input.reference_amplitude) || input.reference_amplitude < 0.0)
        return std::unexpected(Error{"Invalid displacement priority input metric"});
    for (const auto& v : input.vertices) {
        const double length = glm::length(v.displacement_direction);
        if (!finite(v.neutral_position) || !finite(v.displacement_direction) ||
            !std::isfinite(length) || std::abs(length - 1.0) > 1e-5 ||
            !std::isfinite(v.height) || v.height < 0.0f || v.height > 1.0f ||
            !std::isfinite(v.displacement_magnitude) || v.displacement_magnitude < 0.0f)
            return std::unexpected(Error{"Invalid displacement priority source field"});
    }
    DisplacementSurfacePriority result;
    result.source_triangles = input.triangles.size();
    for (const auto& tri : input.triangles) {
        std::array<glm::dvec3, 3> p{}, q{};
        glm::dvec3 raw_reference{};
        for (std::size_t j = 0; j < 3; ++j) {
            if (tri[j] >= input.vertices.size())
                return std::unexpected(Error{"Displacement priority triangle index is out of range"});
            const auto& v = input.vertices[tri[j]];
            p[j] = input.uniform_scale * v.neutral_position;
            const glm::dvec3 offset = input.reference_amplitude *
                double(v.displacement_magnitude) *
                (double(v.height) - 127.0 / 255.0) * v.displacement_direction;
            q[j] = p[j] + offset;
            if (!finite(p[j]) || !finite(q[j]))
                return std::unexpected(Error{"Displacement priority position is nonfinite"});
            result.maximum_vertex_offset = std::max(result.maximum_vertex_offset,
                glm::length(offset));
            raw_reference += v.displacement_direction;
        }
        const auto neutral = face(p), displaced = face(q);
        result.degenerate_neutral_triangles += neutral.degenerate;
        result.degenerate_displaced_triangles += displaced.degenerate;
        if (neutral.degenerate || displaced.degenerate) continue;
        ++result.measured_triangles;
        result.neutral_area += neutral.area;
        result.displaced_area += displaced.area;
        result.minimum_neutral_triangle_angle_degrees = std::min(
            result.minimum_neutral_triangle_angle_degrees, neutral.minimum_angle);
        result.minimum_displaced_triangle_angle_degrees = std::min(
            result.minimum_displaced_triangle_angle_degrees, displaced.minimum_angle);
        const double orientation = glm::dot(neutral.normal, displaced.normal);
        result.minimum_orientation_cosine = std::min(result.minimum_orientation_cosine, orientation);
        result.reversed_displaced_triangles += orientation <= 0.0;
        const double rotation = angle(neutral.normal, displaced.normal);
        result.height_rotation_energy += neutral.area * rotation * rotation;
        result.maximum_height_rotation_degrees = std::max(
            result.maximum_height_rotation_degrees, rotation * radians_to_degrees);
        histogram(result.height_rotation_histogram, rotation * radians_to_degrees, neutral.area);
        const double raw_length = glm::length(raw_reference);
        if (!std::isfinite(raw_length) || raw_length <= 1e-12) {
            ++result.undefined_raw_reference_triangles;
            continue;
        }
        raw_reference /= raw_length;
        const double old_mismatch = angle(neutral.normal, raw_reference);
        const double new_mismatch = angle(displaced.normal, raw_reference);
        result.neutral_raw_mismatch_energy += neutral.area * old_mismatch * old_mismatch;
        result.raw_mismatch_energy += neutral.area * new_mismatch * new_mismatch;
        result.increased_raw_mismatch_energy += neutral.area *
            std::max(0.0, new_mismatch * new_mismatch - old_mismatch * old_mismatch);
        result.maximum_raw_mismatch_degrees = std::max(
            result.maximum_raw_mismatch_degrees, new_mismatch * radians_to_degrees);
        histogram(result.raw_mismatch_histogram, new_mismatch * radians_to_degrees, neutral.area);
    }
    result.priority_energy = result.height_rotation_energy + result.increased_raw_mismatch_energy;
    result.priority_per_source_triangle = result.priority_energy / double(result.source_triangles);
    if (result.neutral_area > 0.0) {
        result.height_rotation_rms_degrees = std::sqrt(
            result.height_rotation_energy / result.neutral_area) * radians_to_degrees;
        result.raw_mismatch_rms_degrees = std::sqrt(
            result.raw_mismatch_energy / result.neutral_area) * radians_to_degrees;
        result.priority_rms_degrees = std::sqrt(
            result.priority_energy / result.neutral_area) * radians_to_degrees;
    }
    if (!std::isfinite(result.priority_energy) || !std::isfinite(result.priority_per_source_triangle))
        return std::unexpected(Error{"Displacement priority accumulation overflow"});
    return result;
}
} // namespace heaven
