#ifndef HEAVENBENCH_SRC_RHI_CLOUD_LIGHTING_H_
#define HEAVENBENCH_SRC_RHI_CLOUD_LIGHTING_H_

#include "rhi/rhi.h"
#include "render_tuning.h"

namespace heaven {

struct CloudLighting {
    glm::vec3 direction{0.0f, 0.0f, 1.0f};
    glm::vec3 direct{0.0f};
    glm::vec3 ambient{0.0f};
};

// Cloud scattering uses relative radiance, unlike Filament's lux-based scene
// lights. Normalize the same sun/moon illumination to the existing noon sun.
// The original white mixtures are retained at noon but supply no light of
// their own after sunset. Night clouds receive the renderer's cool moon fill.
[[nodiscard]] inline CloudLighting cloud_lighting(const LightingParams& lighting) {
    const float sun_lux = std::isfinite(lighting.sun_intensity)
        ? std::max(lighting.sun_intensity, 0.0f) : 0.0f;
    const float sun = std::clamp(sun_lux / render_tuning::lighting.reference_sun_lux, 0.0f, 1.0f);
    const auto lunar = moon_lighting(lighting);
    const float moon = lunar.weight * render_tuning::lighting.moon_intensity_lux /
        render_tuning::lighting.reference_sun_lux;
    const auto tint = render_tuning::lighting.moon_color;
    const glm::vec3 moon_light = glm::vec3(tint.x, tint.y, tint.z) * moon;
    const glm::vec3 combined_direction = lighting.sun_dir * sun + lunar.direction * moon;
    const float direction_length = glm::length(combined_direction);
    return {
        direction_length > 0.0f ? combined_direction / direction_length : lunar.direction,
        glm::mix(lighting.sun_color, glm::vec3(1.0f, 0.97f, 0.90f), 0.42f) * sun + moon_light,
        glm::mix(lighting.sky_color, glm::vec3(0.88f, 0.93f, 1.0f), 0.68f) * sun + moon_light};
}

} // namespace heaven

#endif
