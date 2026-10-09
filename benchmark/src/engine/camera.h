/*
 * HeavenBench — Free-fly camera
 */
#ifndef HEAVENBENCH_SRC_ENGINE_CAMERA_H_
#define HEAVENBENCH_SRC_ENGINE_CAMERA_H_

#include "engine/types.h"

#include <cstdint>

namespace heaven {

// Authored depth-of-field state evaluated from the original benchmark's nested
// dof_control2.track / dof_control16.track bindings. The full Unigine parameter
// set is retained even though Filament's built-in physical DOF currently uses
// only focus_distance and desired_aperture. Keeping the source parameters here
// gives a future custom post-process everything it needs for exact near/far
// blur parity without reparsing the assets.
struct AuthoredDofState {
    bool  enabled            = false;
    float focus_distance     = 20.0f;
    float near_focal_range   = 0.0f;
    float near_blur_range    = 0.0f;
    float near_blur_radius   = 0.0f;
    float far_focal_range    = 0.0f;
    float far_blur_range     = 0.0f;
    float far_blur_radius    = 0.0f;
    float near_blur_power    = 0.0f;
    float bokeh_far_size     = 0.0f;

    // Filament approximation of the original two-preset blend. The original
    // labels the source tracks "2" and "16"; their normalized weights are
    // mapped to an effective f-number using the harmonic mean because optical
    // aperture strength is proportional to 1/f-number.
    float desired_aperture   = 16.0f;
};

// Filament's cocScale changes DOF independently of the exposure aperture:
// camera_aperture / desired_dof_aperture. Invalid authored values disable the
// artistic scale instead of allowing infinities into the post-process.
[[nodiscard]] inline float authored_dof_coc_scale(
    float camera_aperture, float desired_dof_aperture) {
    if (!std::isfinite(camera_aperture) || camera_aperture <= 0.0f ||
        !std::isfinite(desired_dof_aperture) || desired_dof_aperture <= 0.0f) {
        return 0.0f;
    }
    return camera_aperture / desired_dof_aperture;
}

class Camera {
public:
    Camera();

    void process_mouse(float dx, float dy);
    void process_keys(const uint8_t* keys, float dt);

    [[nodiscard]] glm::vec3 front() const;
    [[nodiscard]] glm::vec3 right() const;
    [[nodiscard]] glm::mat4 view_matrix() const;

    glm::vec3 position;
    float     yaw{20.0f};
    float     pitch{-5.0f};
    float     speed{20.0f};
    float     fov{60.0f};
    AuthoredDofState authored_dof;

private:
    static constexpr float kMouseSensitivity = 0.1f;
    static constexpr float kSprintMultiplier = 3.0f;
    static constexpr float kMinPitch = -89.0f;
    static constexpr float kMaxPitch =  89.0f;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_CAMERA_H_
