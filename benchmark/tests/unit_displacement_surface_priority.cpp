#include "engine/displacement_surface_priority.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
namespace {
bool check(bool ok, const char* message) {
    if (!ok) std::fprintf(stderr, "[displacement_surface_priority] %s\n", message);
    return ok;
}
bool close(double a, double b, double tolerance = 1e-12) {
    return std::abs(a - b) <= tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}
bool priority_contract() {
    std::vector<BoundedDisplacementVertex> v(3);
    v[0].neutral_position = {0, 0, 0};
    v[1].neutral_position = {1, 0, 0};
    v[2].neutral_position = {0, 1, 0};
    for (auto& vertex : v) vertex.displacement_direction = {0, 0, 1};
    std::vector<std::array<std::uint32_t, 3>> triangles{{0, 1, 2}};
    BoundedDisplacementInput input{v, triangles, 1, .7};
    const auto constant = analyze_displacement_surface_priority(input);
    if (!check(bool(constant) && constant->priority_energy == 0.0 &&
        close(constant->neutral_area, .5) && close(constant->displaced_area, .5) &&
        close(constant->minimum_neutral_triangle_angle_degrees, 45),
        "constant height translated a plane or changed its severity")) return false;
    v[1].height += .1f;
    const auto tilted = analyze_displacement_surface_priority(input);
    const double theta = std::atan(.7 * (double(v[1].height) - double(v[0].height)));
    if (!check(bool(tilted) && close(tilted->height_rotation_energy, .5 * theta * theta) &&
        close(tilted->increased_raw_mismatch_energy, .5 * theta * theta) &&
        close(tilted->priority_energy, theta * theta) &&
        tilted->height_rotation_histogram.triangles[3] == 1 &&
        tilted->raw_mismatch_histogram.triangles[3] == 1 &&
        tilted->reversed_displaced_triangles == 0,
        "analytic tilted-plane score/histogram disagrees")) return false;
    const auto repeated = analyze_displacement_surface_priority(input);
    if (!check(bool(repeated) && repeated->priority_energy == tilted->priority_energy &&
        repeated->minimum_displaced_triangle_angle_degrees ==
            tilted->minimum_displaced_triangle_angle_degrees,
        "score is nondeterministic")) return false;
    input.reference_amplitude = 0;
    const auto disabled = analyze_displacement_surface_priority(input);
    if (!check(bool(disabled) && disabled->priority_energy == 0,
        "zero-amplitude source retained displacement severity")) return false;
    input.reference_amplitude = 1.4;
    input.uniform_scale = 2;
    const auto scaled = analyze_displacement_surface_priority(input);
    if (!check(bool(scaled) && close(scaled->priority_energy, 4 * tilted->priority_energy) &&
        close(scaled->priority_rms_degrees, tilted->priority_rms_degrees),
        "uniform metric scaling changed angular severity")) return false;
    input.reference_amplitude = .7;
    input.uniform_scale = 1;
    for (auto& vertex : v) vertex.neutral_position += glm::dvec3(1000, -2000, 3000);
    const auto translated = analyze_displacement_surface_priority(input);
    if (!check(bool(translated) && close(translated->priority_energy, tilted->priority_energy, 1e-10),
        "translation changed geometry priority")) return false;
    triangles.push_back(triangles[0]);
    input.triangles = triangles;
    const auto doubled = analyze_displacement_surface_priority(input);
    if (!check(bool(doubled) && close(doubled->priority_energy, 2 * translated->priority_energy) &&
        close(doubled->priority_per_source_triangle, translated->priority_per_source_triangle),
        "source-work normalization changed for duplicated domains")) return false;
    triangles.back() = {0, 0, 1};
    const auto degenerate = analyze_displacement_surface_priority(input);
    if (!check(bool(degenerate) && degenerate->measured_triangles == 1 &&
        degenerate->degenerate_neutral_triangles == 1 &&
        degenerate->degenerate_displaced_triangles == 1,
        "degenerate geometry was silently measured")) return false;
    triangles.back() = {0, 1, 3};
    if (!check(!analyze_displacement_surface_priority(input), "bad index accepted")) return false;
    triangles.pop_back();
    input.triangles = triangles;
    v[1].height = std::numeric_limits<float>::quiet_NaN();
    return check(!analyze_displacement_surface_priority(input), "nonfinite native height accepted");
}
} // namespace
const Register displacement_surface_priority{priority_contract};
} // namespace heaven::testing
