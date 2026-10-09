/*
 * HeavenBench — Depth → world reconstruction reference (CPU mirror)
 *
 * Header-only, GLM-only, pure free functions that mirror the screen-space
 * depth→world reconstruction the splash shader (heaven_splash.mat) performs
 * against the half-res depth pre-pass. The renderer cannot run its `.mat`
 * shaders on the host, so this header is the authoritative CPU reference the
 * GPU shader is written to match, exactly like engine/weather_shader_ref.h is
 * for the streak / wetness / splash-acceptance math.
 *
 * WHY THIS EXISTS (the rotating-normal bug)
 * -----------------------------------------
 * The splash pass reconstructs each pixel's WORLD position from the depth
 * pre-pass, builds a surface normal from neighbour taps, and tests abs(n.z) for
 * Z-up ground. The reconstructed normal of FLAT GROUND was observed to change
 * colour as the camera rotated (teal looking down, magenta at level views) —
 * i.e. it was VIEW-DEPENDENT, when a true world-space flat-ground normal is
 * constant +Z at every orientation. Two prior reconstruction attempts (a CPU
 * glm::inverse(glm::perspective*view) with a `1-2*depth` GL remap, and
 * Filament's in-shader getWorldFromClipMatrix() fed the raw depth) both still
 * rotated with the camera. This header reproduces the EXACT reconstruction
 * offline so the correct math can be PROVEN with a unit test rather than via
 * unverifiable on-screen round-trips.
 *
 * THE TWO THINGS THAT MUST BE MODELLED EXACTLY
 * --------------------------------------------
 *  1. FILAMENT'S PROJECTION. The depth pre-pass (and the main view) are rendered
 *     by Filament's camera via `setProjection(60, aspect, 0.5, 2000, VERTICAL)`.
 *     Filament does NOT use GL's z∈[-1,1] NDC. `FCamera::getProjectionMatrix`
 *     post-multiplies the GL frustum by a remap whose third row is
 *     `(0, 0, -0.5, 0.5)` ("GL to inverted DX convention"), giving REVERSED-Z
 *     with z∈[0,1] (near=1, far=0) — the Vulkan convention. The window-space
 *     depth written to (and sampled from) the depth texture is therefore that
 *     reversed-z [0,1] value DIRECTLY. `filament_reversed_z_projection()` builds
 *     exactly this matrix. (This is where the old glm::perspective attempt went
 *     wrong: glm::perspective is GL z∈[-1,1] non-reversed, a different matrix.)
 *
 *  2. THE Y-FLIP BETWEEN SCREEN UV AND DEPTH-TEXTURE UV. Filament's offscreen
 *     depth render-target is stored top-down, so the splash shader samples it at
 *     `depthUV = (uv.x, 1 - uv.y)` (the same convention as godrays / volfog).
 *     The CENTRE reconstruction is consistent (sample at flipped depthUV,
 *     reconstruct at screen uv) — which is why volfog's distance was always
 *     correct. The BUG is in the NEIGHBOUR taps: the shader samples the vertical
 *     neighbour depth at `depthUV + (0, +texel.y)` but reconstructs that
 *     neighbour at screen `uv + (0, +texel.y)` — the SAME sign. Because depthUV.y
 *     runs opposite to screen uv.y, `depthUV + (0,+texel.y)` is the pixel BELOW
 *     centre while `uv + (0,+texel.y)` is the position ABOVE centre, so the
 *     neighbour's depth and its NDC position are vertically mismatched. The
 *     reconstructed neighbour then falls OFF the true ground plane by an amount
 *     that scales with the on-screen vertical depth gradient: ~0 top-down (so the
 *     normal reads ≈ +Z, teal) but large at level / grazing views (normal tilts,
 *     magenta) — precisely the reported symptom.
 *
 * THE FIX (proven by depth_reconstruct_ref_test.cpp)
 * --------------------------------------------------
 * Keep the depth sample and the reconstruction NDC position consistent: a
 * neighbour at screen offset (ox, oy) must read its depth at
 * `depthUV + (ox, -oy)` (the Y component negated, because the depth texture is
 * Y-flipped) and be reconstructed at screen `uv + (ox, oy)`. With that, every
 * tap of a planar surface reconstructs a point that lies EXACTLY on the plane,
 * so the cross product of the edge vectors is the plane normal (+Z for flat
 * ground, ⟂Z for a wall) at ANY camera orientation — view-invariant by
 * construction. The X taps were already correct (no Y flip in X).
 *
 * World convention is Z-up (the Heaven world), matching engine/camera.h
 * `view_matrix() = glm::lookAt(pos, pos + front, (0,0,1))`.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_DEPTH_RECONSTRUCT_REF_H_
#define HEAVENBENCH_SRC_ENGINE_DEPTH_RECONSTRUCT_REF_H_

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <optional>

namespace heaven::depth_ref {

// ── Filament's reversed-Z projection ────────────────────────────────

// Builds the matrix Filament's camera actually uses for a vertical-FOV
// perspective: the GL frustum `mat4::frustum(-w,w,-h,h,near,far)` (with
// w = tan(fov/2)*near*aspect, h = tan(fov/2)*near) post-multiplied by the
// "GL to inverted DX" remap whose third row is (0,0,-0.5,0.5). Collapsing the
// product for the symmetric finite-far frustum gives the closed form below
// (column-major glm; m[col][row]):

//     fx 0      0          0
//     0  fy     0          0
//     0  0   n/(f-n)   f*n/(f-n)
//     0  0     -1          0

// with fx = 1/(tan(fov/2)*aspect), fy = 1/tan(fov/2). A world/view point at the
// near plane (z_view=-n) maps to window depth 1, and the far plane (z_view=-f)
// to 0 — reversed-Z, z∈[0,1] (Vulkan), matching what the depth pre-pass writes.
[[nodiscard]] inline glm::mat4 filament_reversed_z_projection(
        float fovDegVertical, float aspect, float nearPlane, float farPlane) {
    const float tanHalf = std::tan(glm::radians(fovDegVertical) * 0.5f);
    const float fx = 1.0f / (tanHalf * aspect);
    const float fy = 1.0f / tanHalf;
    const float n  = nearPlane;
    const float f  = farPlane;

    glm::mat4 p(0.0f);          // all-zero, then set the non-zero entries
    p[0][0] = fx;
    p[1][1] = fy;
    p[2][2] = n / (f - n);      // reversed-Z, z∈[0,1]
    p[3][2] = (f * n) / (f - n);
    p[2][3] = -1.0f;            // w = -z_view
    // p[3][3] stays 0 — the reversed-Z hallmark (extra depth precision).
    return p;
}

// Engine Z-up view matrix: world → view, identical to camera.h view_matrix().
[[nodiscard]] inline glm::mat4 view_matrix_zup(const glm::vec3& eye,
                                               const glm::vec3& forward) {
    return glm::lookAt(eye, eye + forward, glm::vec3(0.0f, 0.0f, 1.0f));
}

// ── Forward projection (the depth pre-pass writes these) ────────────

// Window-space reversed-Z depth [0,1] of a world point, or nullopt if the point
// is at/behind the eye (w <= 0) or outside the [0,1] depth range. This is the
// value the depth texture stores at the point's screen pixel.
[[nodiscard]] inline std::optional<float> project_depth(
        const glm::mat4& clipFromWorld, const glm::vec3& world) {
    const glm::vec4 clip = clipFromWorld * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-6f) {
        return std::nullopt;
    }
    const float depth = clip.z / clip.w;
    if (depth <= 0.0f || depth >= 1.0f) {
        return std::nullopt;
    }
    return depth;
}

// Screen UV [0,1]^2 of a world point (NDC.xy * 0.5 + 0.5). UV.y runs bottom→top
// with NDC.y; the depth TEXTURE is the Y-flip of this (see depthUV below).
[[nodiscard]] inline std::optional<glm::vec2> project_uv(
        const glm::mat4& clipFromWorld, const glm::vec3& world) {
    const glm::vec4 clip = clipFromWorld * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-6f) {
        return std::nullopt;
    }
    const glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
    return ndc * 0.5f + 0.5f;
}

// ── Inverse reconstruction (the splash shader's worldFromUV) ────────

// Reconstruct a world position from a screen UV and its sampled reversed-Z
// depth: clip = (uv*2-1, depth, 1); world = (worldFromClip * clip) / w. This is
// byte-for-byte the splash shader's worldFromUV(). `worldFromClip` is
// inverse(clipFromWorld) = Filament's getWorldFromClipMatrix().
[[nodiscard]] inline glm::vec3 world_from_uv(
        const glm::mat4& worldFromClip, const glm::vec2& uv, float depth) {
    const glm::vec2 ndcXY = uv * 2.0f - 1.0f;
    const glm::vec4 clip(ndcXY, depth, 1.0f);
    const glm::vec4 wp = worldFromClip * clip;
    return glm::vec3(wp) / wp.w;
}

// ── Test scene: a single infinite plane filling the screen ──────────

// A plane is (unit normal, point-on-plane). The depth buffer at a screen pixel
// is the projected depth of the plane point the eye ray through that pixel hits.
struct Plane {
    glm::vec3 normal;     // unit
    glm::vec3 point;      // any point on the plane
};

// Eye ray through screen UV: reconstruct the near (depth=1) and far (depth=0)
// points and take the line between them. (Reversed-Z: near=1, far=0.)
struct Ray {
    glm::vec3 origin;     // near-plane point
    glm::vec3 dir;        // toward the far plane (not normalised)
};

[[nodiscard]] inline Ray eye_ray(const glm::mat4& worldFromClip,
                                 const glm::vec2& uv) {
    const glm::vec3 pNear = world_from_uv(worldFromClip, uv, 1.0f);
    const glm::vec3 pFar  = world_from_uv(worldFromClip, uv, 0.0f);
    return Ray{pNear, pFar - pNear};
}

// Sample the modelled depth texture at a DEPTH-TEXTURE UV `depthUV`. Converts to
// the screen UV it corresponds to (the texture is Y-flipped: screen uv =
// (depthUV.x, 1 - depthUV.y)), casts the eye ray, intersects the plane, and
// forward-projects the hit to its reversed-Z depth. Returns nullopt when the
// ray misses (parallel / behind), mirroring a pixel that sees no ground.
[[nodiscard]] inline std::optional<float> sample_plane_depth(
        const glm::mat4& clipFromWorld, const glm::mat4& worldFromClip,
        const Plane& plane, const glm::vec2& depthUV) {
    const glm::vec2 screenUV(depthUV.x, 1.0f - depthUV.y);
    const Ray ray = eye_ray(worldFromClip, screenUV);

    const float denom = glm::dot(plane.normal, ray.dir);
    if (std::abs(denom) < 1e-9f) {
        return std::nullopt;                       // ray parallel to plane
    }
    const float t = glm::dot(plane.normal, plane.point - ray.origin) / denom;
    if (t < 0.0f) {
        return std::nullopt;                       // plane behind the near point
    }
    const glm::vec3 hit = ray.origin + t * ray.dir;
    return project_depth(clipFromWorld, hit);
}

// ── Edge-aware normal reconstruction (the splash shader's 5-tap method) ──

// `flipTapY == true`  → the CORRECT mapping: the depth-texture UV offset's Y is
//                       negated relative to the screen offset, so each tap reads
//                       the depth of the SAME pixel it is reconstructed at.
// `flipTapY == false` → the BUGGY mapping currently in heaven_splash.mat: the
//                       depthUV offset uses the SAME sign as the screen offset,
//                       so the vertical neighbour's depth and NDC position are
//                       swapped top/bottom and the normal tilts with the camera.

// Returns nullopt if the centre or any required tap misses the plane.
struct NormalSample {
    glm::vec3 worldPos;   // reconstructed centre world position
    glm::vec3 normal;     // reconstructed (un-normalised) surface normal
};

[[nodiscard]] inline std::optional<NormalSample> reconstruct_normal(
        const glm::mat4& clipFromWorld, const Plane& plane,
        const glm::vec2& uv, const glm::vec2& texel, bool flipTapY) {
    const glm::mat4 worldFromClip = glm::inverse(clipFromWorld);
    const glm::vec2 depthUV(uv.x, 1.0f - uv.y);

    // A neighbour at SCREEN offset `screenOff`. Its depth is read from the
    // depth texture at depthUV + depthOff, where depthOff.y is negated in the
    // correct mapping (texture Y-flip) and kept (buggy) otherwise. It is
    // reconstructed at the screen UV uv + screenOff.
    struct PlaneTap {
        glm::vec3 world;
        float depth;
    };
    auto tap = [&](const glm::vec2& screenOff) -> std::optional<PlaneTap> {
        const glm::vec2 depthOff(screenOff.x,
                                 flipTapY ? -screenOff.y : screenOff.y);
        const std::optional<float> d =
            sample_plane_depth(clipFromWorld, worldFromClip, plane, depthUV + depthOff);
        if (!d) {
            return std::nullopt;
        }
        return PlaneTap{world_from_uv(worldFromClip, uv + screenOff, *d), *d};
    };

    // Centre: sample at the (Y-flipped) depthUV, reconstruct at screen uv —
    // exactly what the shader does for the centre, and what is already correct.
    const std::optional<float> dC =
        sample_plane_depth(clipFromWorld, worldFromClip, plane, depthUV);
    if (!dC) {
        return std::nullopt;
    }
    const glm::vec3 worldPos = world_from_uv(worldFromClip, uv, *dC);

    const std::optional<PlaneTap> wL = tap({-texel.x, 0.0f});
    const std::optional<PlaneTap> wR = tap({ texel.x, 0.0f});
    const std::optional<PlaneTap> wD = tap({0.0f, -texel.y});
    const std::optional<PlaneTap> wU = tap({0.0f,  texel.y});
    if (!wL || !wR || !wD || !wU) {
        return std::nullopt;
    }

    // Reuse the validated tap depths for the edge-aware "pick the nearer
    // neighbour" selection. On a planar surface either choice yields an
    // in-plane edge vector.
    const glm::vec3 hVec = (std::abs(wR->depth - *dC) < std::abs(wL->depth - *dC))
        ? (wR->world - worldPos) : (worldPos - wL->world);
    const glm::vec3 vVec = (std::abs(wU->depth - *dC) < std::abs(wD->depth - *dC))
        ? (wU->world - worldPos) : (worldPos - wD->world);

    return NormalSample{worldPos, glm::cross(hVec, vVec)};
}

} // namespace heaven::depth_ref

#endif // HEAVENBENCH_SRC_ENGINE_DEPTH_RECONSTRUCT_REF_H_
