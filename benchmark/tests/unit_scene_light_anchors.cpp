#include "engine/shadow_light_select_ref.h"
#include "rhi/filament_util.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
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

bool require(bool condition, const char* message) {
    if (!condition) std::printf("[unit_scene_light_anchors] FAIL: %s\n", message);
    return condition;
}

bool check_scene_light_anchors() {
    using namespace heaven;
    bool ok = true;
    ok &= require(is_night_luminaire("lamp_post_lamp_post_glass") &&
                      is_night_luminaire("hanging_lantern_glass") &&
                      !is_night_luminaire("house_window_glass") &&
                      !is_night_luminaire("laboratory_glass") &&
                      !is_night_luminaire("lamp_post_metal") &&
                      !is_night_luminaire("torch_lantern_glass"),
                  "night policy captured windows, opaque fixtures or an open flame");
    ok &= require(night_luminaire_gain(0) == 1 && night_luminaire_gain(21) == 1 &&
                      night_luminaire_gain(24) == 1 && night_luminaire_gain(-3) == 1 &&
                      night_luminaire_gain(5.25f) == 0 &&
                      night_luminaire_gain(12) == 0 && night_luminaire_gain(18.75f) == 0 &&
                      night_luminaire_gain(std::numeric_limits<float>::quiet_NaN()) == 0,
                  "night operating gain violated authored phase endpoints or invalid handling");
    float last_gain = 0;
    for (int step = 0; step <= 90; ++step) {
        const float gain = night_luminaire_gain(18.75f + static_cast<float>(step) * .025f);
        ok &= require(gain >= last_gain && gain <= 1, "evening gain was discontinuous/unbounded");
        last_gain = gain;
    }
    ok &= require(lens_flare_power_gain(0, 100) == 0 &&
                      lens_flare_power_gain(50, 100) == .5f &&
                      lens_flare_power_gain(200, 100) == 1 &&
                      lens_flare_power_gain(1, 0) == 0 &&
                      lens_flare_power_gain(std::numeric_limits<float>::infinity(), 100) == 0,
                  "a disabled or invalid source retained flare power");
    const glm::vec3 forward{0, 0, -1};
    ok &= require(lens_flare_angular_gain(forward, forward * 5.0f) == 1 &&
                      lens_flare_angular_gain(forward, -forward) == 0 &&
                      lens_flare_angular_gain({}, forward) == 0 &&
                      lens_flare_angular_gain(forward, {}) == 0,
                  "optical-axis gain violated center, rear or invalid-vector controls");
    last_gain = 1;
    for (int angle = 0; angle <= 30; ++angle) {
        const float radians = glm::radians(static_cast<float>(angle));
        const float gain = lens_flare_angular_gain(forward,
            glm::vec3(std::sin(radians), 0, -std::cos(radians)));
        ok &= require(gain <= last_gain && gain >= 0,
                      "optical-axis gain increased away from the source");
        if (angle > 20) ok &= require(gain == 0, "off-axis flare did not disappear");
        last_gain = gain;
    }
    const glm::vec3 base{1, 2, 3};
    const std::array offsets{std::pair{6, 0.75f}, std::pair{2, -0.5f}};
    ok &= require(floating_light_position(base, 6, offsets) == glm::vec3(1, 2, 3.75f),
                  "an offset already present at light creation was lost");
    ok &= require(floating_light_position(base, -1, offsets) == base &&
                      floating_light_position(base, 99, offsets) == base,
                  "static or absent groups acquired another group's offset");

    // Exact source witness from torch.mesh and torch_light.node. The authored
    // flame is above the opaque bowl, not at the metal surface's AABB centre.
    const glm::vec3 minimum{-0.132973f, -0.573786f, -0.462070f};
    const glm::vec3 maximum{0.133077f, 0.014637f, 0.653104f};
    const glm::vec3 flame{-0.0058001713f, -0.48956615f, 0.83338583f};
    const glm::mat4 transform = glm::translate(glm::mat4(1), glm::vec3(4, -7, 9)) *
        glm::rotate(glm::mat4(1), 0.63f, glm::vec3(0, 0, 1)) *
        glm::scale(glm::mat4(1), glm::vec3(0.4f, 0.7f, 0.6f));
    const std::array origins{glm::vec3(transform * glm::vec4(flame, 1))};
    const auto anchor = luminaire_emitter_anchor(minimum, maximum, transform, origins);
    ok &= require(anchor && glm::length(*anchor - flame) < 2e-6f,
                  "the actual flame did not retain its source anchor under placement");
    ok &= require(anchor && glm::length(*anchor - (minimum + maximum) * 0.5f) > 0.7f,
                  "the source fell back to the metal bracket centre");
    const std::array ambiguous{origins[0], origins[0] + glm::vec3(0, 0, 0.01f)};
    const std::array distant{origins[0] + glm::vec3(20, 0, 0)};
    ok &= require(!luminaire_emitter_anchor(minimum, maximum, transform, ambiguous) &&
                      !luminaire_emitter_anchor(minimum, maximum, transform, distant),
                  "ambiguous or distant fires captured a fixture");
    auto singular = transform;
    singular[0] = singular[1];
    auto projective = transform;
    projective[0][3] = 0.1f;
    const std::array nonfinite{glm::vec3(std::numeric_limits<float>::quiet_NaN())};
    ok &= require(!luminaire_emitter_anchor(minimum, maximum, singular, origins) &&
                      !luminaire_emitter_anchor(minimum, maximum, projective, origins) &&
                      !luminaire_emitter_anchor(minimum, maximum, transform, nonfinite),
                  "invalid placement or emitter data produced a light anchor");

    using shadow_light_ref::ShadowLightCandidate;
    std::array lights{
        ShadowLightCandidate{180000, 1, false}, // synthetic enclosed lamp
        ShadowLightCandidate{8000, 2, false},   // authored lamp shadow=0
        ShadowLightCandidate{1000, 3, true},
        ShadowLightCandidate{2000, 4, true}};
    const auto original = lights;
    ok &= require(shadow_light_ref::select_shadow_casting_lights(lights, 1) ==
                      std::vector<std::size_t>{3} &&
                      shadow_light_ref::select_shadow_casting_lights(lights, 20) ==
                      std::vector<std::size_t>({2, 3}),
                  "a global shadow budget promoted an enclosed or authored-disabled source");
    for (std::size_t i = 0; i < lights.size(); ++i) {
        ok &= require(lights[i].intensity == original[i].intensity,
                      "shadow eligibility changed illumination intensity");
    }
    lights[0].eligible = true;
    ok &= require(shadow_light_ref::select_shadow_casting_lights(lights, 1) ==
                      std::vector<std::size_t>{0},
                  "an eligible bright source lost ordinary budget selection");
    for (auto& light : lights) light.eligible = false;
    ok &= require(shadow_light_ref::select_shadow_casting_lights(lights, 20).empty(),
                  "an all-ineligible light set acquired a caster");
    return ok;
}

const heaven::testing::Register registration(check_scene_light_anchors);

} // namespace
