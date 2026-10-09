/*
 * Regression coverage for the free-fly camera's screen-space mouse direction.
 */
#include "engine/camera.h"
#include "engine/camera_path.h"

#include <array>
#include <functional>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool unit_camera_controls() {
    heaven::Camera camera;

    const float initial_yaw = camera.yaw;
    camera.process_mouse(-10.0f, 0.0f);
    if (camera.yaw <= initial_yaw) return false;

    camera.yaw = initial_yaw;
    camera.process_mouse(10.0f, 0.0f);
    if (camera.yaw >= initial_yaw) return false;

    const float initial_pitch = camera.pitch;
    camera.process_mouse(0.0f, -10.0f);
    return camera.pitch > initial_pitch;
}

const heaven::testing::Register g_register_camera_controls{
    &unit_camera_controls};

bool unit_empty_camera_path() {
    const heaven::CameraPath path;
    const auto state = [](const heaven::Camera& camera) {
        const auto& dof = camera.authored_dof;
        return std::array{camera.position.x, camera.position.y, camera.position.z,
                          camera.yaw, camera.pitch, camera.fov,
                          dof.focus_distance, dof.near_focal_range, dof.near_blur_range,
                          dof.near_blur_radius, dof.far_focal_range, dof.far_blur_range,
                          dof.far_blur_radius, dof.near_blur_power, dof.bokeh_far_size,
                          dof.desired_aperture};
    };
    for (const bool clamped : {false, true}) {
        heaven::Camera camera;
        camera.position = {3.0f, -2.0f, 9.0f};
        camera.yaw = 12.0f;
        camera.pitch = -8.0f;
        camera.fov = 75.0f;
        camera.authored_dof = {true, 7.0f, 1.0f, 2.0f, 3.0f, 4.0f,
                              5.0f, 6.0f, 8.0f, 9.0f, 2.8f};
        const auto before = state(camera);
        if (clamped) path.apply_clamped(0.0f, camera);
        else path.apply(0.0f, camera);
        if (state(camera) != before || !camera.authored_dof.enabled) return false;
    }
    return true;
}

const heaven::testing::Register g_register_empty_camera_path{&unit_empty_camera_path};

} // namespace
