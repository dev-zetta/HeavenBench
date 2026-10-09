#include "engine/grass_bounds.h"
#include "rhi/filament_bounds.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool contains_windy_vertices(std::span<const heaven::GrassVertex> vertices,
                            const glm::mat4& transform) {
    const auto geometry = heaven::grass_geometry_bounds(vertices);
    const auto bounds = heaven::grass_wind_bounds(geometry, transform);
    if (!bounds) return false;
    const auto box = heaven::box_from_min_max(
        {bounds->min.x, bounds->min.y, bounds->min.z},
        {bounds->max.x, bounds->max.y, bounds->max.z});
    const auto lower = box.getMin();
    const auto upper = box.getMax();
    const glm::dmat4 world(transform);
    const glm::dmat4 inverse = glm::inverse(world);
    for (const auto& vertex : vertices) {
        const glm::dvec4 position = world *
            glm::dvec4(vertex.px, vertex.py, vertex.pz, 1.0);
        // Exercise independent sine extrema, including the worst simultaneous
        // phase, not merely a few sampled times that might miss maximum sway.
        for (double primary : {-1.0, 0.0, 1.0}) {
            for (double secondary : {-1.0, 0.0, 1.0}) {
                const double wind = primary * heaven::render_tuning::grass.wind_tuning.z +
                                    secondary * heaven::render_tuning::grass.wind_tuning.w;
                const double height = static_cast<double>(vertex.v) * vertex.v;
                const glm::dvec4 animated = position + glm::dvec4(
                    wind * height * 0.8, wind * height * 0.3,
                    -std::abs(wind) * height * 0.2, 0.0);
                const glm::dvec4 local = inverse * animated;
                for (glm::length_t axis = 0; axis < 3; ++axis) {
                    if (local[axis] < lower[axis] || local[axis] > upper[axis]) {
                        std::fprintf(stderr, "Grass wind escaped bounds at axis %d\n", axis);
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool unit_generated_foliage_bounds_cover_world_wind() {
    const std::vector<heaven::GrassRoot> roots{
        {{-17.0f, 4.0f, -2.0f}}, {{9.0f, -6.0f, 3.0f}},
        {{0.0f, 0.0f, 0.0f}}, {{23.0f, 17.0f, 8.0f}}};
    const auto grass = heaven::generate_grass_blades(roots, 0.0f);
    const auto flowers = heaven::generate_flower_cards(roots);
    if (grass.size() != roots.size() * heaven::GRASS_VERTS_PER_BLADE ||
        flowers.size() != roots.size() * heaven::FLOWER_VERTS_PER_CARD) return false;
    const std::array scales{
        glm::vec3(1.0f), glm::vec3(0.01f, 2.5f, 0.04f),
        glm::vec3(-0.1f, 0.03f, 4.0f), glm::vec3(3.0f, 0.3f, -2.0f)};
    for (const auto& scale : scales) {
        for (float angle : {0.0f, 0.35f, 1.3f, 3.0f}) {
            glm::mat4 transform = glm::translate(glm::mat4(1.0f),
                                                 glm::vec3(80.0f, -13.0f, 47.0f));
            transform = glm::rotate(transform, angle, glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)));
            transform = glm::scale(transform, scale);
            if (!contains_windy_vertices(grass, transform) ||
                !contains_windy_vertices(flowers, transform)) return false;
        }
    }
    glm::mat4 shear(1.0f);
    shear[1][0] = 2.0f;
    shear[2][1] = -0.4f;
    if (!contains_windy_vertices(grass, shear) ||
        !contains_windy_vertices(flowers, shear)) return false;

    // The replacement remains substantially tighter than the old 400m box.
    const auto bounds = heaven::grass_wind_bounds(
        heaven::grass_geometry_bounds(grass), glm::mat4(1.0f));
    return bounds && bounds->min.x > -20.0f && bounds->max.x < 26.0f &&
           bounds->min.y > -9.0f && bounds->max.y < 20.0f &&
           bounds->min.z > -5.0f && bounds->max.z < 12.0f;
}

bool unit_grass_bounds_reject_unsafe_culling() {
    std::array<heaven::GrassVertex, 1> vertices{};
    vertices[0].v = 2.0f; // Do not silently assume the generator's current 0..1 UVs.
    const auto geometry = heaven::grass_geometry_bounds(vertices);
    if (!contains_windy_vertices(vertices, glm::mat4(1.0f))) return false;
    if (heaven::grass_geometry_bounds({}).valid) return false;
    glm::mat4 singular(1.0f);
    singular[0][0] = 0.0f;
    glm::mat4 projective(1.0f);
    projective[0][3] = 0.1f;
    glm::mat4 invalid(1.0f);
    invalid[3][0] = std::numeric_limits<float>::infinity();
    if (heaven::grass_wind_bounds(geometry, singular) ||
        heaven::grass_wind_bounds(geometry, projective) ||
        heaven::grass_wind_bounds(geometry, invalid) ||
        heaven::grass_wind_bounds({}, glm::mat4(1.0f))) return false;
    vertices[0].px = std::numeric_limits<float>::quiet_NaN();
    if (heaven::grass_geometry_bounds(vertices).valid) return false;
    vertices[0].px = 0.0f;
    vertices[0].v = std::numeric_limits<float>::max();
    if (heaven::grass_geometry_bounds(vertices).valid) return false;
    vertices[0].v = 1.0f;
    vertices[0].px = std::numeric_limits<float>::max() * 0.75f;
    return !heaven::grass_wind_bounds(
        heaven::grass_geometry_bounds(vertices), glm::mat4(1.0f));
}

bool unit_grass_bounds_cache_tracks_linear_transform() {
    const std::array<heaven::GrassVertex, 1> vertices{};
    heaven::GrassBoundsCache bounds;
    bounds.geometry = heaven::grass_geometry_bounds(vertices);
    if (!bounds.update(glm::mat4(1.0f)) || !bounds.local) return false;
    const auto original = *bounds.local;
    const glm::mat4 translated = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
    if (bounds.update(translated)) return false;
    const glm::mat4 scaled = glm::scale(translated, glm::vec3(0.1f));
    if (!bounds.update(scaled) || !bounds.local ||
        bounds.local->max.x - bounds.local->min.x <= original.max.x - original.min.x) return false;
    if (bounds.update(scaled)) return false;
    glm::mat4 invalid = scaled;
    invalid[3][1] = std::numeric_limits<float>::quiet_NaN();
    if (!bounds.update(invalid) || bounds.local) return false;
    return bounds.update(scaled) && bounds.local.has_value();
}

bool unit_shared_foliage_preserves_generated_bytes() {
    const std::vector<heaven::GrassRoot> roots{
        {{-17.0f, 4.0f, -2.0f}}, {{9.0f, -6.0f, 3.0f}}, {{23.0f, 17.0f, 8.0f}}};
    const auto shared_grass = heaven::generate_grass_blades(roots, 0.0f);
    const auto shared_flowers = heaven::generate_flower_cards(roots);
    // The previous make_patch path copied root positions into every patch,
    // rebuilt GrassRoot records and generated the same stream for each island.
    // Compare that exact path's bytes with the source/category upload input.
    for (int instance = 0; instance < 3; ++instance) {
        std::vector<glm::vec3> old_positions;
        old_positions.reserve(roots.size());
        for (const auto& root : roots) old_positions.push_back(root.position);
        std::vector<heaven::GrassRoot> old_roots(old_positions.size());
        for (size_t i = 0; i < old_roots.size(); ++i) old_roots[i].position = old_positions[i];
        const auto old_grass = heaven::generate_grass_blades(old_roots, 0.0f);
        const auto old_flowers = heaven::generate_flower_cards(old_roots);
        if (!std::ranges::equal(std::as_bytes(std::span(old_grass)),
                                std::as_bytes(std::span(shared_grass))) ||
            !std::ranges::equal(std::as_bytes(std::span(old_flowers)),
                                std::as_bytes(std::span(shared_flowers)))) return false;
    }
    return true;
}

const heaven::testing::Register g_generated_foliage_bounds{
    &unit_generated_foliage_bounds_cover_world_wind};
const heaven::testing::Register g_unsafe_grass_bounds{
    &unit_grass_bounds_reject_unsafe_culling};
const heaven::testing::Register g_grass_bounds_cache{
    &unit_grass_bounds_cache_tracks_linear_transform};
const heaven::testing::Register g_shared_foliage_bytes{
    &unit_shared_foliage_preserves_generated_bytes};

} // namespace
