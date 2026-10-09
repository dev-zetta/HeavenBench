#include "rhi/mesh_displacement.h"
#include "rhi/geometry_displacement.h"

#include <array>
#include <cstdio>
#include <functional>
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

struct TestVertex {
    float px, py, pz, u, v;
};

bool mesh_displacement_footprint_test() {
    using namespace heaven::mesh_displacement;
    const std::array<TestVertex, 10> vertices{{
        {0, 0, 0, 0, 0}, {1, 0, 0, 1, 0}, {0, 1, 0, 0, 0.5f},
        {-0.0f, 0, 0, -0.0f, 0}, {-1, 0, 0, -2, 0}, {0, -1, 0, 0, -3},
        {0, 0, 0, 10, 10}, {2, 0, 0, 10.25f, 10}, {0, 2, 0, 10, 10.125f},
        {99, 99, 99, 0, 0}}};
    const std::array<uint32_t, 12> indices{0, 1, 2, 3, 4, 5, 6, 7, 8, 1, 1, 1};
    const auto footprints = calculate_footprints<TestVertex>(vertices, indices);
    // Exact tangent-seam duplicates share a footprint even when their incident
    // triangles differ by several mip levels; a different UV chart stays separate.
    const bool matching = footprints[0] == Footprint{2, 3} &&
        footprints[3] == footprints[0] && footprints[1] == Footprint{1, 0.5f} &&
        footprints[6] == Footprint{0.25f, 0.125f} && footprints[9] == Footprint{0, 0};
    const std::array<uint32_t, 12> reordered{1, 1, 1, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    const bool stable = footprints == calculate_footprints<TestVertex>(vertices, reordered);
    const bool empty = calculate_footprints<TestVertex>({}, {}).empty();
    if (!matching || !stable || !empty) {
        std::fprintf(stderr, "[mesh_displacement] footprint/seam invariants failed\n");
        return false;
    }
    return true;
}

bool mesh_displacement_deformation_test() {
    using namespace heaven::mesh_displacement;
    const auto original = append_deformation_signature(deformation_signature_seed, 0, 1, 0.7f);
    const bool valid = original == append_deformation_signature(
        deformation_signature_seed, -0.0f, 1, 0.7f);
    const bool uv_changed = original != append_deformation_signature(
        deformation_signature_seed, 0, 1.01f, 0.7f);
    const bool magnitude_changed = original != append_deformation_signature(
        deformation_signature_seed, 0, 1, 0.8f);
    if (!valid || !uv_changed || !magnitude_changed) {
        std::fprintf(stderr, "[mesh_displacement] immutable attribute guard failed\n");
        return false;
    }
    return true;
}

heaven::testing::Register footprints{mesh_displacement_footprint_test};
heaven::testing::Register deformation{mesh_displacement_deformation_test};

bool mesh_displacement_master_gate_test() {
    heaven::GpuMaterial material;
    material.geometry_displacement = true;
    material.tess_scale = 0.4f;
    const auto on = heaven::geometry_displacement(material, true, 1.5f, 1.0f, true);
    const auto off = heaven::geometry_displacement(material, true, 1.5f, 1.0f, false);
    const auto again = heaven::geometry_displacement(material, true, 1.5f, 1.0f, true);
    if (!on.enabled || off.enabled || !again.enabled ||
            on.amplitude != off.amplitude || on.amplitude != again.amplitude ||
            off.parameters()[3] != 0.0f || on.parameters()[3] <= 0.0f) {
        std::fprintf(stderr, "[mesh_displacement] master gate failed to disable the position path while preserving controls\n");
        return false;
    }
    return true;
}

heaven::testing::Register master_gate{mesh_displacement_master_gate_test};

} // namespace
