/*
 * HeavenBench — bounded bloom strength with the existing day/night response
 */
#ifndef HEAVENBENCH_SRC_RHI_BLOOM_STRENGTH_H_
#define HEAVENBENCH_SRC_RHI_BLOOM_STRENGTH_H_

#include "engine/types.h"

namespace heaven {

[[nodiscard]] inline float bloom_strength_for_time_of_day(
        float requested, float sun_intensity) {
    const float bright = std::isfinite(sun_intensity)
        ? std::clamp(sun_intensity / render_tuning::lighting.reference_sun_lux,
                     0.0f, 1.0f)
        : 0.0f;
    const float baseline = render_tuning::lighting.bloom_strength.x +
        bright * render_tuning::lighting.bloom_strength.y;
    const float relative = clamp_bloom_strength(requested).value /
        render_tuning::lighting.bloom_reference_strength;
    // The default request divides to exactly one, retaining the old curve.
    return std::clamp(baseline * relative, 0.0f, 1.0f);
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_BLOOM_STRENGTH_H_
