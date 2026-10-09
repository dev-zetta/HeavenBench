/*
 * HeavenBench — Camera collision
 *
 * Stops the free-fly camera from clipping through scene geometry.
 * The camera is treated as a small sphere; each frame the desired
 * movement is resolved with a sphere-cast collide-and-slide against
 * a static collision world built from the scene's mesh instances.
 *
 * Collision uses Jolt Physics (already vendored for the bridge
 * ropes). The static world is built once at scene load: each unique
 * mesh becomes a Jolt MeshShape, and every instance becomes a static
 * body referencing that shape. Animated / floating instances have
 * their body transforms refreshed each frame so the camera still
 * collides with islands as they bob.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_CAMERA_COLLISION_H_
#define HEAVENBENCH_SRC_ENGINE_CAMERA_COLLISION_H_

#include "engine/types.h"

#include <memory>
#include <span>
#include <vector>

namespace heaven {

struct Mesh;       // engine/types.h
struct Instance;   // engine/types.h

class CameraCollision {
public:
    CameraCollision();
    ~CameraCollision();

    CameraCollision(CameraCollision&&) noexcept;
    CameraCollision& operator=(CameraCollision&&) noexcept;
    CameraCollision(const CameraCollision&) = delete;
    CameraCollision& operator=(const CameraCollision&) = delete;

    //Build the static collision world from the scene. `meshes` is
    //indexed by `Instance::mesh_index`. Replaces the previous world after a
    //scene load; preserves radius and enabled state. Returns false if no collidable geometry was found
    //(collision then silently becomes a no-op).
    bool build(std::span<const Mesh> meshes,
               std::span<const Instance> instances);

    //Refresh the transforms of bodies whose instance moved this
    //frame (floating islands, stones, animated meshes). `indices`
    //lists the instance indices that changed. Only bodies within
    //`active_radius` of `camera_pos` are actually repositioned —
    //distant islands can't collide with the camera this frame, so
    //skipping them keeps the per-frame cost proportional to what's
    //nearby rather than the whole world. Pass an empty span to skip.
    void update_moving(std::span<const Instance> instances,
                       std::span<const int> moving_indices,
                       const glm::vec3& camera_pos,
                       float active_radius = 60.0f);

    //Resolve a desired camera move. Given the current `position`
    //and the `desired` next position (after free-fly input), return
    //a collision-corrected position that slides along surfaces and
    //never penetrates geometry. When collision is disabled or the
    //world is empty this just returns `desired`.
    [[nodiscard]] glm::vec3 resolve(const glm::vec3& position,
                                    const glm::vec3& desired) const;

    //Camera collision radius (metres). Movement keeps the camera
    //centre at least this far from any surface.
    void  set_radius(float r) { radius_ = r; }
    float radius() const { return radius_; }

    bool enabled() const { return enabled_; }
    void set_enabled(bool e) { enabled_ = e; }

    size_t body_count() const;

private:
    struct State;
    std::unique_ptr<State> state_;
    float radius_  = 0.5f;
    bool  enabled_ = true;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_CAMERA_COLLISION_H_
