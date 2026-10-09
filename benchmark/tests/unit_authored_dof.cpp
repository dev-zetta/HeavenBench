/*
 * HeavenBench — authored benchmark DOF interpolation examples
 */

#include "engine/camera_path.h"

#include <cmath>
#include <cstdio>
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

bool approximately(float a, float b) {
    return std::abs(a - b) <= 1e-5f;
}

bool unit_authored_dof() {
    bool ok = true;

    // Filament's documented artistic scale: the exposure aperture is divided
    // by the desired DOF aperture exactly once.
    if (!approximately(heaven::authored_dof_coc_scale(16.0f, 2.0f), 8.0f) ||
        !approximately(heaven::authored_dof_coc_scale(16.0f, 16.0f), 1.0f) ||
        heaven::authored_dof_coc_scale(16.0f, 0.0f) != 0.0f) {
        std::printf("[unit_authored_dof] FAILED: CoC scale mapping\n");
        ok = false;
    }

    heaven::CameraKeyframe a;
    a.time = 0.0f;
    a.scene = 0;
    a.authored_dof.enabled = true;
    a.authored_dof.focus_distance = 2.0f;
    a.authored_dof.desired_aperture = 2.0f;
    a.authored_dof.near_blur_radius = 20.0f;

    heaven::CameraKeyframe b = a;
    b.time = 1.0f;
    b.authored_dof.focus_distance = 16.0f;
    b.authored_dof.desired_aperture = 16.0f;
    b.authored_dof.near_blur_radius = 5.0f;

    heaven::CameraPath path;
    path.keyframes = {a, b};
    path.duration = 1.0f;

    const heaven::AuthoredDofState middle =
        path.depth_of_field_clamped(0.5f);
    if (!middle.enabled ||
        !approximately(middle.focus_distance, 9.0f) ||
        !approximately(middle.desired_aperture, 9.0f) ||
        !approximately(middle.near_blur_radius, 12.5f)) {
        std::printf("[unit_authored_dof] FAILED: same-scene interpolation\n");
        ok = false;
    }

    heaven::Camera camera;
    path.apply_clamped(0.5f, camera);
    if (!approximately(camera.authored_dof.focus_distance, 9.0f)) {
        std::printf("[unit_authored_dof] FAILED: camera state propagation\n");
        ok = false;
    }

    // A scene cut holds the active side instead of creating a focus pull
    // between unrelated shots, matching pose and time-of-day evaluation.
    path.keyframes[1].scene = 1;
    const heaven::AuthoredDofState before_cut =
        path.depth_of_field_clamped(0.999f);
    if (!approximately(before_cut.focus_distance, 2.0f)) {
        std::printf("[unit_authored_dof] FAILED: scene-cut hold\n");
        ok = false;
    }

    if (ok) {
        std::printf("[unit_authored_dof] OK\n");
    }
    return ok;
}

const heaven::testing::Register register_authored_dof{unit_authored_dof};

} // namespace
