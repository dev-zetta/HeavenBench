#include "rhi/cloud_lighting.h"

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

bool unit_cloud_lighting() {
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "[unit_cloud_lighting] %s\n", message);
            ok = false;
        }
    };
    const auto noon = heaven::lighting_for_time_of_day(12.0f);
    const auto day = heaven::cloud_lighting(noon);
    check(glm::length(day.direct - glm::mix(noon.sun_color,
              glm::vec3(1.0f, 0.97f, 0.90f), 0.42f)) < 1e-6f,
          "noon direct light changed from the existing shader");
    check(glm::length(day.ambient - glm::mix(noon.sky_color,
              glm::vec3(0.88f, 0.93f, 1.0f), 0.68f)) < 1e-6f,
          "noon ambient light changed from the existing shader");
    check(glm::length(day.direction - noon.sun_dir) < 1e-6f,
          "daylight scattering stopped following the sun");
    const auto midnight = heaven::lighting_for_time_of_day(0.0f);
    const auto night = heaven::cloud_lighting(midnight);
    check(glm::length(night.direct) > 0.0f &&
              glm::length(night.direct) < glm::length(day.direct) * 0.05f &&
              glm::length(night.ambient) < glm::length(day.ambient) * 0.05f,
          "night clouds retain daylight brightness or lose all moonlight");
    check(night.direct.b > night.direct.r && night.direction.z > 0.0f,
          "night scattering does not follow cool moonlight above the horizon");
    auto unlit = noon;
    unlit.sun_intensity = 0.0f;
    const auto no_sun = heaven::cloud_lighting(unlit);
    check(glm::length(no_sun.direct) == 0.0f &&
              glm::length(no_sun.ambient) == 0.0f,
          "a dark daytime sky invents solar or lunar illumination");
    for (int step = 0; step <= 96; ++step) {
        const auto lighting = heaven::cloud_lighting(
            heaven::lighting_for_time_of_day(static_cast<float>(step) * 0.25f));
        check(std::isfinite(glm::length(lighting.direct)) &&
                  std::isfinite(glm::length(lighting.ambient)) &&
                  std::abs(glm::length(lighting.direction) - 1.0f) < 1e-5f,
              "day/night transition produces invalid scattering parameters");
    }
    return ok;
}

const heaven::testing::Register g_register_cloud_lighting{&unit_cloud_lighting};

} // namespace
