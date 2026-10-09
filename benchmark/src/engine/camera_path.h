/*
 * HeavenBench — Camera path playback from.track files
 *
 * Parses Unigine Tracker xml0 binary files and interpolates
 * camera position/rotation along spline keyframes.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_CAMERA_PATH_H_
#define HEAVENBENCH_SRC_ENGINE_CAMERA_PATH_H_

#include "engine/asset_file_system.h"
#include "engine/types.h"
#include "engine/camera.h"

#include <vector>

namespace heaven {

class AssetUsageRecorder;

struct CameraKeyframe {
    float time;
    glm::vec3 position;
    glm::quat rotation; // stored as quaternion for smooth interpolation
    float fov;
    int   scene = 0;    // clip.track scene index this keyframe belongs to
    float time_of_day = 0.0f;
    bool  has_time_of_day = false;
    AuthoredDofState authored_dof;
};

struct CameraPath {
    std::vector<CameraKeyframe> keyframes;
    float duration = 0.0f; // total path time in seconds

    // Scene-cut fade-to-black, sampled from clip.track's render.fadeColor
    // parameter: pairs of (time, alpha). The original benchmark snaps the
    // camera at each scene boundary and hides the cut behind a black fade —
    // alpha ramps to 1 just before the boundary, holds across the snap, then
    // ramps back to 0. fade_alpha(t) reproduces that mask.
    std::vector<glm::vec2> fade_keys;

    //Interpolate camera state at the given time (loops: t is wrapped into
    //[0, duration) before evaluation).
    void evaluate(float t, glm::vec3& out_pos, glm::quat& out_rot, float& out_fov) const;

    //Interpolate camera state without looping. For t <= 0 returns the first
    //keyframe; for t >= duration holds the final keyframe; otherwise
    //interpolates. Shares the same interpolation core as evaluate().
    void evaluate_clamped(float t, glm::vec3& out_pos, glm::quat& out_rot, float& out_fov) const;

    //Apply the looping interpolated state to a Camera.
    void apply(float t, Camera& camera) const;

    //Apply the non-looping (clamped) interpolated state to a Camera.
    void apply_clamped(float t, Camera& camera) const;

    //Black-fade alpha in [0, 1] at the given time (0 = clear, 1 = black).
    //Returns 0 when no fade keys are present.
    [[nodiscard]] float fade_alpha(float t) const;

    //Authored time-of-day hour sampled from the active camera track's nested
    //day_cycle.track binding. Returns fallback when the path has no such data.
    [[nodiscard]] float time_of_day(float t, float fallback) const;

    //Non-looping variant of time_of_day(), matching evaluate_clamped().
    [[nodiscard]] float time_of_day_clamped(float t, float fallback) const;

    //Authored DOF state sampled from the active camera track's nested
    //dof_control2 / dof_control16 bindings.
    [[nodiscard]] AuthoredDofState depth_of_field(float t) const;

    //Non-looping variant of depth_of_field(), matching evaluate_clamped().
    [[nodiscard]] AuthoredDofState depth_of_field_clamped(float t) const;

private:
    //Shared interpolation core. Assumes a non-empty keyframe list and a time
    //value already mapped into the path's range (wrapped for looping, clamped
    //for non-looping). Both evaluate() and evaluate_clamped() delegate here so
    //their per-segment interpolation cannot drift apart.

    //Camera poses are interpolated WITHIN a scene only: when the two
    //surrounding keyframes belong to different clip.track scenes the pose
    //snaps (holds the active scene's keyframe) instead of blending across the
    //cut, matching the original benchmark's hard scene transitions.
    void evaluate_core(float t, glm::vec3& out_pos, glm::quat& out_rot, float& out_fov) const;

    [[nodiscard]] float time_of_day_core(float t, float fallback) const;
    [[nodiscard]] AuthoredDofState depth_of_field_core(float t) const;
};

//Load a camera path from a.track xml0 file.
//Returns empty path on failure.

//When `rec` is non-null (the used-asset packer), each.track file that is
//successfully opened and parsed (clip.track and every referenced camera
//track) is recorded into it. When null (every normal render run) behavior is
//byte-for-byte unchanged.
[[nodiscard]] CameraPath load_camera_path(const std::string& track_dir,
                                          AssetUsageRecorder* rec = nullptr,
                                          AssetFileSystemRef assets = std::nullopt);

//Owns the playback clock for a single, non-looping run over a CameraPath.
//Advances 1:1 with real elapsed time (no scaling) and holds the final
//keyframe once the clock reaches the path duration.
class CameraPathPlayer {
public:
    explicit CameraPathPlayer(const CameraPath* path) : path_(path) {}

    //Rewind the playback clock to the beginning of the path.
    void reset() { time_ = 0.0f; }

    //Advance the playback clock by dt seconds, 1:1 with real time.
    void advance(float dt) { time_ += dt; }

    //Drive the camera from the current clock via non-looping evaluation.
    void apply(Camera& camera) const {
        if (path_) path_->apply_clamped(time_, camera);
    }

    [[nodiscard]] float time() const { return time_; }
    [[nodiscard]] float duration() const { return path_ ? path_->duration : 0.0f; }
    [[nodiscard]] bool  at_end() const { return time_ >= duration(); }

    //Scene-cut black-fade alpha at the current clock (0 = clear, 1 = black).
    [[nodiscard]] float fade_alpha() const {
        return path_ ? path_->fade_alpha(time_) : 0.0f;
    }

    //Authored time-of-day hour at the current playback clock. Returns fallback
    //when the path has no day-cycle binding.
    [[nodiscard]] float time_of_day(float fallback) const {
        return path_ ? path_->time_of_day_clamped(time_, fallback) : fallback;
    }

    [[nodiscard]] AuthoredDofState depth_of_field() const {
        return path_ ? path_->depth_of_field_clamped(time_) : AuthoredDofState{};
    }

private:
    const CameraPath* path_ = nullptr;
    float time_ = 0.0f;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_CAMERA_PATH_H_
