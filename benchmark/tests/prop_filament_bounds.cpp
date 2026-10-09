/*
 * HeavenBench — regression property for Filament Box conversion.
 *
 * filament::Box is an aggregate of {center, halfExtent}; it is not a
 * {minimum, maximum} pair. This test exercises the renderer's real conversion
 * helper and protects culling code from reintroducing the shifted-bounds bug.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <vector>

#include <rapidcheck.h>

#include "rhi/filament_bounds.h"

namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

bool bounds_preserved(const filament::math::float3& min,
                      const filament::math::float3& extent) {
    const auto max = min + extent;
    const filament::Box box = heaven::box_from_min_max(min, max);
    const auto actual_min = box.getMin();
    const auto actual_max = box.getMax();

    // There are up to three rounded additions/subtractions: generating max,
    // forming center/halfExtent, and reconstructing a corner. Halving is exact
    // for this finite, normal-valued generator. The standard gamma(3) bound
    // scales with the operands, because center - halfExtent can cancel to a
    // tiny corner even when both intermediate values are large.
    constexpr double unit_roundoff = std::numeric_limits<float>::epsilon() * 0.5;
    constexpr double gamma3 = 3.0 * unit_roundoff / (1.0 - 3.0 * unit_roundoff);
    for (size_t axis = 0; axis < 3; ++axis) {
        const double lo = min[axis];
        const double hi = max[axis];
        const double width = extent[axis];
        const double tolerance = gamma3 * std::max(1.0, std::abs(lo) + std::abs(hi) + width);
        const auto close = [tolerance](double actual, double expected) {
            return std::abs(actual - expected) <= tolerance;
        };
        if (!close(actual_min[axis], lo) ||
            !close(actual_max[axis], hi) ||
            !close(box.center[axis], (lo + hi) * 0.5) ||
            !close(box.halfExtent[axis], width * 0.5)) {
            return false;
        }
    }
    return true;
}

bool run_filament_box_cancellation_regression() {
    // Shrunk counterexample from RapidCheck seed 1876408331214698159.
    return bounds_preserved({-134.0f * 0.01f, -1000.0f, -1000.0f},
                            {51469.0f * 0.01f, 0.0f, 0.0f});
}

bool run_filament_box_min_max_property() {
    return rc::check(
        "filament Box(min,max) conversion preserves extrema",
        [] {
            const auto min = filament::math::float3{
                static_cast<float>(*rc::gen::inRange(-100000, 100000)) * 0.01f,
                static_cast<float>(*rc::gen::inRange(-100000, 100000)) * 0.01f,
                static_cast<float>(*rc::gen::inRange(-100000, 100000)) * 0.01f};
            const auto extent = filament::math::float3{
                static_cast<float>(*rc::gen::inRange(0, 100000)) * 0.01f,
                static_cast<float>(*rc::gen::inRange(0, 100000)) * 0.01f,
                static_cast<float>(*rc::gen::inRange(0, 100000)) * 0.01f};
            RC_ASSERT(bounds_preserved(min, extent));
        });
}

const heaven::testing::Register g_register_filament_bounds{
    &run_filament_box_min_max_property};
const heaven::testing::Register g_register_filament_bounds_cancellation{
    &run_filament_box_cancellation_regression};

} // namespace
