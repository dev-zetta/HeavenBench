/*
 * HeavenBench — SSGI depth → position/normal reconstruction (CPU mirror)
 *
 * Header-only, GLM-only, pure free functions that mirror the screen-space
 * depth→position and depth→normal reconstruction the SSGI shader
 * (heaven_ssgi.mat) performs against the half-res linear scene-depth texture.
 * The renderer cannot run its `.mat` shaders on the host, so this header is the
 * authoritative CPU reference the GPU shader is written to match, exactly like
 * engine/weather_shader_ref.h is for the weather math and
 * engine/depth_reconstruct_ref.h is for the splash pass.
 *
 * WHY THIS EXISTS
 * ---------------
 * Screen-space global illumination reconstructs each pixel's VIEW-SPACE position
 * and surface normal from the depth buffer, then gathers one-bounce indirect
 * light from the scene-colour buffer within a bounded screen-space radius. The
 * reconstruction math (un-projecting a screen [uv, distance] back to a
 * view-space position, and deriving a normal from neighbouring taps) is the part
 * most prone to silent, hard-to-see-on-screen error — a sign flip or a wrong
 * scale only subtly tints the gather. Mirroring it here lets the round-trip be
 * PROVEN with a property test  rather than judged by eye.
 *
 * THE DEPTH SOURCE MODELLED (scene-depth-pipeline)
 * ------------------------------------------------
 * The depth pre-pass now renders LINEAR scene depth into an F COLOUR target
 * (heaven_depth proxy render): each pixel stores the eye-space FORWARD distance
 * in metres (-viewZ) to the nearest opaque surface, NOT a reversed-Z window
 * depth. The view-space reconstruction is therefore a plain view-ray
 * un-projection scaled by that linear distance — no projection matrix, no
 * reversed-Z, no GL remap. The per-axis view-ray slope `projScale =
 * (tan(fovY/2)*aspect, tan(fovY/2))` (the inverse of the perspective focal
 * terms fx, fy) is the only camera parameter the reconstruction needs.
 *
 * THE RECONSTRUCTION ROUND-TRIP (proven by tests/prop_ssgi_reconstruct.cpp)
 * ------------------------------------------------------------------------
 * For any view-space point p in front of the camera and inside the frustum,
 * projecting it to its screen [uv, distance] and then reconstructing from that
 * [uv, distance] must return p within float tolerance:
 *
 *     project_view_linear(projScale, p)            -> { uv, dist }
 *     view_position_from_dist(projScale, uv, dist) == p   (within tolerance)
 *
 * This is byte-for-byte the SSGI shader's `viewPosFromDist(uv, dist)`.
 *
 * NORMAL RECONSTRUCTION
 * ---------------------
 * The shader reconstructs the surface normal from screen-space derivatives of
 * the reconstructed view-space position: `n = normalize(cross(dpdx, dpdy))`,
 * oriented toward the camera. `reconstruct_view_normal()` mirrors this and
 * ALWAYS returns a unit-length vector (a degenerate/flat tap set falls back to
 * the view-space camera axis +Z, which is itself unit length), so the normal a
 * lighting gather divides by is never zero-length.
 *
 * LOCKSTEP
 * --------
 * When the heaven_ssgi.mat reconstruction block changes, change it here too (and
 * vice versa) — the property tests assert the two stay in lockstep.
 *
 * World convention is Z-up (the Heaven world), matching engine/camera.h and
 * engine/depth_reconstruct_ref.h.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_SSGI_RECONSTRUCT_REF_H_
#define HEAVENBENCH_SRC_ENGINE_SSGI_RECONSTRUCT_REF_H_

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <optional>

namespace heaven::ssgi_ref {

// Screen-space sample produced by the linear depth pre-pass for a view-space
// point: the screen UV [0,1]^2 (NDC.xy * 0.5 + 0.5) and the eye-space FORWARD
// distance in metres (-viewZ) the depth texture stores at that pixel.
struct UvDist {
    glm::vec2 uv;
    float dist;
};

// ── Per-axis view-ray slope ─────────────────────────────────────────

// projScale = (tan(fovY/2)*aspect, tan(fovY/2)) — the view-ray slope at the
// image edges along X and Y, i.e. the inverse of the perspective focal terms
// fx = 1/(tan(fovY/2)*aspect), fy = 1/tan(fovY/2). This is the only camera
// parameter the linear-depth reconstruction needs.
[[nodiscard]] inline glm::vec2 view_ray_scale(float fovDegVertical, float aspect) {
    const float tanHalf = std::tan(glm::radians(fovDegVertical) * 0.5f);
    return {tanHalf * aspect, tanHalf};
}

// ── Forward projection (what the linear depth pre-pass writes) ──────

// Project a VIEW-SPACE point to its screen [uv, distance]. In view space the
// camera sits at the origin looking down -Z, so the eye-forward distance is
// dist = -viewPos.z and the NDC is viewPos.xy / (projScale * dist). Returns
// nullopt when the point is at/behind the eye (dist <= 0) or projects outside
// the [-1,1] NDC range, mirroring a pixel the depth pre-pass would not write
// for this point.
[[nodiscard]] inline std::optional<UvDist> project_view_linear(
        const glm::vec2& projScale, const glm::vec3& viewPos) {
    const float dist = -viewPos.z;
    if (dist <= 1e-6f) {
        return std::nullopt;
    }
    const glm::vec2 ndc(viewPos.x / (projScale.x * dist),
                        viewPos.y / (projScale.y * dist));
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) {
        return std::nullopt;
    }
    return UvDist{ndc * 0.5f + 0.5f, dist};
}

// ── Inverse reconstruction (the SSGI shader's viewPosFromDist) ──────

// Reconstruct a VIEW-SPACE position from a screen UV and its sampled LINEAR
// eye-forward distance (metres): ndc = uv*2-1; viewRay = (ndc.x*projScale.x,
// ndc.y*projScale.y, -1); viewPos = viewRay * dist. This is byte-for-byte the
// SSGI shader's view-position reconstruction. Composing it with
// project_view_linear round-trips to the original view-space point within float
// tolerance.
[[nodiscard]] inline glm::vec3 view_position_from_dist(
        const glm::vec2& projScale, const glm::vec2& uv, float dist) {
    const glm::vec2 ndcXY = uv * 2.0f - 1.0f;
    const glm::vec3 viewRay(ndcXY.x * projScale.x, ndcXY.y * projScale.y, -1.0f);
    return viewRay * dist;
}

// ── Normal reconstruction (screen-space derivative cross product) ───

// Reconstruct a unit-length surface normal from the centre view-space position
// and its horizontal (right) and vertical (up) reconstructed neighbour taps,
// exactly like the SSGI shader's `normalize(cross(dpdx, dpdy))`. The normal is
// oriented to face the camera (the view-space origin): a point in front of the
// camera has dot(n, -center) > 0 for a camera-facing normal, so the result is
// flipped when that dot is negative.

// ALWAYS returns a unit-length vector: a degenerate tap set (collinear / coincident
// neighbours, zero-length cross product) cannot define a plane, so the function
// falls back to the view-space camera axis +Z, which is itself unit length. This
// guarantees the normal a lighting gather divides/normalises by is never zero.
[[nodiscard]] inline glm::vec3 reconstruct_view_normal(
        const glm::vec3& center, const glm::vec3& right, const glm::vec3& up) {
    const glm::vec3 dpdx = right - center;
    const glm::vec3 dpdy = up - center;
    const glm::vec3 cross = glm::cross(dpdx, dpdy);
    const float len = glm::length(cross);
    if (len <= 1e-12f) {
        return {0.0f, 0.0f, 1.0f};   // degenerate: unit fallback
    }
    glm::vec3 n = cross / len;
    // Orient toward the camera at the view-space origin (skip if center is at the
    // eye, where the view direction is undefined).
    if (glm::dot(n, -center) < 0.0f) {
        n = -n;
    }
    return n;
}

} // namespace heaven::ssgi_ref

#endif // HEAVENBENCH_SRC_ENGINE_SSGI_RECONSTRUCT_REF_H_
