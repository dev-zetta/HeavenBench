#include "engine/boiler_smoke.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool unit_boiler_smoke() {
    heaven::Mesh mesh;
    mesh.surfaces.resize(2);
    // Offset upper rim, asymmetric vertex duplication, and a lower body whose
    // bounds center differs from the opening. Header bounds are intentionally
    // wrong: geometry must determine the anchor.
    for (const glm::vec3 point : {glm::vec3{9, -2, 7}, {11, -2, 7},
                                  {11, 2, 7}, {9, 2, 7}, {9, 2, 7},
                                  {6, -3, 1}, {15, 3, 1}}) {
        heaven::Vertex vertex{};
        vertex.px = point.x;
        vertex.py = point.y;
        vertex.pz = point.z;
        mesh.surfaces[0].vertices.push_back(vertex);
    }
    mesh.surfaces[1] = mesh.surfaces[0]; // matching LOD does not bias placement
    const glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), glm::vec3(-50, 45, 8)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(2, 3, 0.5f));
    const auto actual = heaven::boiler_smoke_transform(mesh, transform);
    if (!actual || glm::length(glm::vec3((*actual)[3]) -
                               glm::vec3(-50, 65, 11.5f)) > 1e-5f ||
        glm::mat3(*actual) != glm::mat3(1.0f)) {
        std::printf("[unit_boiler_smoke] FAILED: transformed chimney rim / upright frame\n");
        return false;
    }
    if (heaven::boiler_smoke_transform(heaven::Mesh{}, transform)) return false;
    mesh.surfaces[0].vertices[0].px = std::numeric_limits<float>::quiet_NaN();
    if (heaven::boiler_smoke_transform(mesh, transform)) return false;
    mesh.surfaces[0].vertices[0].px = 9;
    auto invalid = transform;
    invalid[3].x = std::numeric_limits<float>::infinity();
    if (heaven::boiler_smoke_transform(mesh, invalid)) return false;
    return true;
}

const heaven::testing::Register reg_boiler_smoke(unit_boiler_smoke);

} // namespace
