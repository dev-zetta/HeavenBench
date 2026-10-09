/*
 * HeavenBench — Filament shared utilities
 *
 * Small helpers shared between the FilamentBackend core and the
 * per-feature render modules under rhi/features/. Keep this header
 * dependency-light: only Filament math + glm, no engine state.
 */
#ifndef HEAVENBENCH_SRC_RHI_FILAMENT_UTIL_H_
#define HEAVENBENCH_SRC_RHI_FILAMENT_UTIL_H_

#include <math/mat4.h>
#include <math/vec3.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>
#include <cmath>
#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace heaven {

struct SceneLightInfo;  // rhi/render_feature.h

// Viewport aspect ratio with a divide-by-zero guard. Returns
// `width / height` (float division) when `height > 0`, and a defined
// 1.0f fallback when `height == 0` so a zero/transient viewport height
// can never produce a NaN aspect. This centralises the guard RainFeature
// already applies so every aspect consumer stays in lockstep; for any
// non-zero height the value is identical to the bare
// `static_cast<float>(w) / static_cast<float>(h)` computation.
[[nodiscard]] inline float safe_aspect(int w, int h) {
    return h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
}

// Convert a glm::mat4 to filament::math::mat4f. Both are column-major
// with column-vector convention, so a straight memcpy is correct.
[[nodiscard]] inline filament::math::mat4f glm_to_filament(const glm::mat4& m) {
    filament::math::mat4f result;
    std::memcpy(&result, glm::value_ptr(m), sizeof(float) * 16);
    return result;
}

// True when `surface_name` denotes a simplified LOD imposter (a
// `*_lod1`/`*_lod2`/… far-distance billboard) rather than the highest-
// detail `*_lod0`. We render only lod0 and skip imposters — drawing
// them on top of lod0 produces visible flat plates poking through the
// detailed geometry. Matches both the underscore-prefixed form
// (`building_windows_lod1`, `fir_trunk_lod_1`) and the bare prefix form
// the fir branch cards use (`lod_1`..`lod_4`).
[[nodiscard]] bool is_lod_imposter(std::string_view surface_name);

// Emission-light tuning derived purely from a surface name. Heaven's
// luminaires (lamp/torch/lantern/glass/window/door/mill) each get a
// characteristic warm colour, intensity (lumens) and falloff radius.
struct EmissionLightTuning {
    glm::vec3 color{1.0f, 0.78f, 0.45f};
    float     intensity = 50000.0f;
    float     falloff   = 6.0f;
};
[[nodiscard]] EmissionLightTuning classify_emission_light(std::string_view surface_name);

// Only enclosed lamp/lantern bulbs share the original night-light schedule.
// Windows, laboratory glass and open flames keep their independent behavior.
[[nodiscard]] inline bool is_night_luminaire(std::string_view surface_name) {
    const auto has = [&](std::string_view part) {
        return surface_name.find(part) != std::string_view::npos;
    };
    return has("glass") && (has("lamp") || has("lantern")) &&
           !has("torch") && !has("fire");
}

// day_cycle.track drives the night subtrack with the linear phase keys
// (0,1), (5.25,0), (18.75,0), (21,1). Smooth gain replaces the original
// per-node stepped material/light switches; this is not script playback.
[[nodiscard]] inline float night_luminaire_gain(float hour) {
    if (!std::isfinite(hour)) return 0.0f;
    hour = std::fmod(hour, 24.0f);
    if (hour < 0.0f) hour += 24.0f;
    const float phase = hour < 5.25f ? 1.0f - hour / 5.25f :
        (hour > 18.75f ? std::min((hour - 18.75f) / 2.25f, 1.0f) : 0.0f);
    return phase * phase * (3.0f - 2.0f * phase);
}

// A source's live candela controls its flare. A disabled, invalid or retired
// light never leaves a cosmetic halo; brighter sources saturate at nominal.
[[nodiscard]] inline float lens_flare_power_gain(float current, float nominal) {
    if (!std::isfinite(current) || !std::isfinite(nominal) ||
        current <= 0.0f || nominal <= 0.0f) return 0.0f;
    return std::min(current, nominal) / nominal;
}

// Optical-axis policy requested for the flare: full within five degrees,
// smoothly zero by twenty. This does not alter the source's illumination.
[[nodiscard]] inline float lens_flare_angular_gain(
        glm::vec3 camera_forward, glm::vec3 camera_to_source) {
    const float forward_squared = glm::dot(camera_forward, camera_forward);
    const float source_squared = glm::dot(camera_to_source, camera_to_source);
    if (!std::isfinite(forward_squared) || !std::isfinite(source_squared) ||
        forward_squared <= 0.0f || source_squared <= 0.0f) return 0.0f;
    const float cosine = glm::dot(camera_forward / std::sqrt(forward_squared),
                                  camera_to_source / std::sqrt(source_squared));
    if (!std::isfinite(cosine)) return 0.0f;
    constexpr float full_cosine = 0.996194698f; // cos(5 degrees)
    constexpr float zero_cosine = 0.939692621f; // cos(20 degrees)
    const float weight = std::clamp((cosine - zero_cosine) /
                                    (full_cosine - zero_cosine), 0.0f, 1.0f);
    return weight * weight * (3.0f - 2.0f * weight);
}

// The loader supplies a world-space base position and a floating-group ID.
// Offsets may arrive before the first light is uploaded, so creation and later
// updates must use the same resolver rather than waiting for a changed offset.
[[nodiscard]] inline glm::vec3 floating_light_position(
        glm::vec3 base_position, int floating_group,
        std::span<const std::pair<int, float>> offsets) {
    if (floating_group >= 0) {
        for (const auto& [group, offset] : offsets) {
            if (group == floating_group) {
                base_position.z += offset;
                break;
            }
        }
    }
    return base_position;
}

// Associate an authored flame origin with a nearby fixture without choosing an
// arbitrary origin when several sources overlap. Return mesh-local coordinates
// so the light follows the same instance transform as the fixture. The search
// allowance is one quarter of the fixture diagonal: the actual flame can sit
// just above the metal bowl, but a distant fire must not capture this fixture.
[[nodiscard]] inline std::optional<glm::vec3> luminaire_emitter_anchor(
        glm::vec3 bounds_min, glm::vec3 bounds_max,
        const glm::mat4& transform, std::span<const glm::vec3> origins) {
    const auto finite = [](glm::vec3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    if (!finite(bounds_min) || !finite(bounds_max) ||
        glm::any(glm::lessThan(bounds_max, bounds_min)) ||
        transform[0][3] != 0.0f || transform[1][3] != 0.0f ||
        transform[2][3] != 0.0f || !std::isfinite(transform[3][3]) ||
        transform[3][3] <= 0.0f) return std::nullopt;
    const glm::mat3 inverse = glm::inverse(glm::mat3(transform));
    if (!finite(inverse[0]) || !finite(inverse[1]) || !finite(inverse[2]) ||
        !finite(glm::vec3(transform[3]))) return std::nullopt;
    const float allowance_squared = glm::dot(bounds_max - bounds_min,
                                             bounds_max - bounds_min) * 0.0625f;
    if (!std::isfinite(allowance_squared) || allowance_squared <= 0.0f)
        return std::nullopt;
    std::optional<glm::vec3> result;
    for (const glm::vec3 origin : origins) {
        if (!finite(origin)) continue;
        const glm::vec3 local = inverse * (origin - glm::vec3(transform[3]));
        if (!finite(local)) continue;
        const glm::vec3 delta = local - glm::clamp(local, bounds_min, bounds_max);
        if (glm::dot(delta, delta) > allowance_squared) continue;
        if (result) return std::nullopt;
        result = local;
    }
    return result;
}

// Select which of the scene's point/spot lights should cast shadows, given a
// budget — the highest-luminous-intensity eligible lights, ties broken
// by the light's stable per-light identifier (its entity id), returned as
// ascending original indices into `lights`.

// This is the Filament-facing wrapper over the pure, host-testable algorithm in
// engine/shadow_light_select_ref.h: it reads `intensity` + `entity.getId()`
// from each SceneLightInfo and delegates the ranking/selection. The pure header
// (not this wrapper) is what the property test exercises, since this
// TU pulls in Filament types the headless test target does not link.

// Deterministic: identical `lights` input + budget always yields an identical
// selected set. A budget ≤ 0 selects none; a budget ≥ n selects all.
[[nodiscard]] std::vector<size_t> select_shadow_casting_lights(
        std::span<const SceneLightInfo> lights, int budget);

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FILAMENT_UTIL_H_
