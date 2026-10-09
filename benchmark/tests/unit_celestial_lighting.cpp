#include "rhi/cloud_lighting.h"

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

bool unit_celestial_lighting() {
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) std::fprintf(stderr, "[unit_celestial_lighting] %s\n", message);
        ok &= condition;
    };
    const auto noon = heaven::lighting_for_time_of_day(12.0f);
    const auto midnight = heaven::lighting_for_time_of_day(0.0f);
    check(noon.sun_dir.z > 0.999f && midnight.sun_dir.z < -0.999f,
          "Sun does not cross the complete day/night orbit");
    check(noon.sun_intensity == 110000.0f && midnight.sun_intensity == 0.0f,
          "day/night solar illuminance changed");
    for (int step = 0; step < 96; ++step) {
        const float hour = static_cast<float>(step) * 0.25f;
        const auto light = heaven::lighting_for_time_of_day(hour);
        const auto moon = heaven::moon_lighting(light);
        check(std::abs(glm::length(moon.direction) - 1.0f) < 1e-6f &&
                  glm::dot(moon.direction, light.sun_dir) < -0.99999f,
              "full Moon and Sun are not opposite unit directions");
        check(moon.weight >= 0.0f && moon.weight <= 1.0f &&
                  (moon.direction.z > 0.0f || moon.weight == 0.0f),
              "Moon illuminates from below the horizon");
        if (hour >= 7.0f && hour <= 17.0f) {
            check(moon.weight == 0.0f, "daytime gained lunar illumination");
        }
        if (hour >= 20.0f || hour <= 4.0f) {
            check(light.sun_dir.z < -0.49f && moon.weight == 1.0f,
                  "night Sun was pinned to twilight or Moon disappeared");
        }
        auto dark = light;
        dark.sun_intensity = 0.0f;
        check(heaven::moon_lighting(dark).weight == moon.weight &&
                  heaven::moon_lighting(dark).direction == moon.direction,
              "weather-dependent solar lux moves or creates the Moon");
    }
    const auto night_clouds = heaven::cloud_lighting(midnight);
    const auto moon = heaven::moon_lighting(midnight);
    check(glm::length(night_clouds.direction - moon.direction) < 1e-6f,
          "cloud scattering disagrees with lunar illumination");
    auto invalid = midnight;
    invalid.sun_dir = glm::vec3(0.0f);
    check(heaven::moon_lighting(invalid).weight == 0.0f,
          "zero celestial direction creates invalid illumination");
    invalid.sun_dir.x = std::numeric_limits<float>::infinity();
    check(heaven::moon_lighting(invalid).weight == 0.0f,
          "nonfinite celestial direction creates illumination");
    return ok;
}

const heaven::testing::Register g_register_celestial_lighting{&unit_celestial_lighting};

} // namespace
