/*
 * HeavenBench — Free-fly camera implementation
 */
#include "engine/camera.h"

#include <SDL2/SDL.h>

namespace heaven {

Camera::Camera()
    : position(-25.0f, -10.0f, 20.0f)
     {}

glm::vec3 Camera::front() const {
    float ry = glm::radians(yaw);
    float rp = glm::radians(pitch);
    return glm::normalize(glm::vec3(
        std::cos(rp) * std::cos(ry),
        std::cos(rp) * std::sin(ry),
        std::sin(rp)
    ));
}

glm::vec3 Camera::right() const {
    // Right-handed Z-up convention: right = world_up × front
    // (the previous order, front × world_up, gave a left-handed
    // basis, which made WASD strafe inverted and mouse-X swing
    // the world the wrong way).
    return glm::normalize(glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), front()));
}

glm::mat4 Camera::view_matrix() const {
    return glm::lookAt(position, position + front(), glm::vec3(0.0f, 0.0f, 1.0f));
}

void Camera::process_mouse(float dx, float dy) {
    yaw   -= dx * kMouseSensitivity;
    pitch -= dy * kMouseSensitivity;
    pitch  = glm::clamp(pitch, kMinPitch, kMaxPitch);
}

void Camera::process_keys(const uint8_t* keys, float dt) {
    float v = speed * dt;
    if (keys[SDL_SCANCODE_LSHIFT]) v *= kSprintMultiplier;

    if (keys[SDL_SCANCODE_W]) position += front() * v;
    if (keys[SDL_SCANCODE_S]) position -= front() * v;
    if (keys[SDL_SCANCODE_A]) position += right() * v;
    if (keys[SDL_SCANCODE_D]) position -= right() * v;
    if (keys[SDL_SCANCODE_E]) position.z += v;
    if (keys[SDL_SCANCODE_Q]) position.z -= v;
}

} // namespace heaven
