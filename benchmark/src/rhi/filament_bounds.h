/*
 * HeavenBench — Filament bounding-volume helpers
 */
#ifndef HEAVENBENCH_SRC_RHI_FILAMENT_BOUNDS_H_
#define HEAVENBENCH_SRC_RHI_FILAMENT_BOUNDS_H_

#include <filament/Box.h>

#include <math/vec3.h>

#include "render_tuning.h"

namespace heaven {

// Filament's Box aggregate fields are {center, halfExtent}, not {min, max}.
// Constructing it through Box::set makes min/max call sites explicit and
// prevents shifted frustum-culling bounds.
[[nodiscard]] inline filament::Box box_from_min_max(
        const filament::math::float3& min,
        const filament::math::float3& max) {
    filament::Box box;
    box.set(min, max);
    return box;
}

[[nodiscard]] inline filament::Box uncullable_box() {
    constexpr float extent =
        render_tuning::fullscreen.uncullable_bounds_extent;
    return box_from_min_max({-extent, -extent, -extent},
                            {extent, extent, extent});
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FILAMENT_BOUNDS_H_
