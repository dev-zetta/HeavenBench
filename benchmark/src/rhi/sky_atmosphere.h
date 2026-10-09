/*
 * HeavenBench - shared analytic sky model
 */
#ifndef HEAVENBENCH_SRC_RHI_SKY_ATMOSPHERE_H_
#define HEAVENBENCH_SRC_RHI_SKY_ATMOSPHERE_H_

#include "rhi/rhi.h"

#include <glm/glm.hpp>

namespace heaven {

struct SkyAtmosphereParams {
    glm::vec3 depth_r{0.0f};
    glm::vec3 depth_m{0.0f};
    glm::vec3 ozone{0.0f};
    glm::vec3 multi_scattering{0.0f};
    glm::vec3 night_color{0.0f};
    glm::vec2 mie_phase_params{1.0f, 0.0f};
    float horizon_glow = 0.0f;
    float sun_radiance = 0.0f;
    float sun_disk_radiance = 0.0f;
    float sun_cos_radius = 1.0f;
    float sun_limb_darkening = 0.5f;
    float moon_intensity = 0.0f;
    float moon_cos_radius = 1.0f;
    float moon_sin_radius = 0.0f;
    float exposure = 1.0f;
    float contrast = 1.0f;
    glm::vec3 moon_dir{0.0f, 0.0f, 1.0f};
    glm::vec3 milky_way_row0{1.0f, 0.0f, 0.0f};
    glm::vec3 milky_way_row1{0.0f, 1.0f, 0.0f};
    glm::vec3 milky_way_row2{0.0f, 0.0f, 1.0f};
    glm::vec3 milky_way_control{0.0f};
};

[[nodiscard]] SkyAtmosphereParams make_sky_atmosphere_params(
    const LightingParams& lighting);

[[nodiscard]] glm::vec3 evaluate_sky_atmosphere(
    const SkyAtmosphereParams& p, glm::vec3 view_dir,
    glm::vec3 sun_dir, float cloud_density = 0.0f);

[[nodiscard]] glm::vec3 sky_moon_direction(const LightingParams& lighting);

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_SKY_ATMOSPHERE_H_
