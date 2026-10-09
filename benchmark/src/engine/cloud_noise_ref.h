/*
 * HeavenBench — Volumetric-cloud shader reference math (CPU mirror)
 *
 * Header-only, GLM-only, pure free functions that mirror the raymarch / noise /
 * scattering math used by the volumetric-cloud material:
 *
 *   - heaven_volumetric_clouds.mat (raymarched 3D cloud layer, authored in
 *
 *
 * The renderer cannot run its `.mat` shaders on the host, so this header is the
 * authoritative CPU reference the GPU shader is written to match exactly, in
 * the same spirit as engine/weather_shader_ref.h (streak / wetness / splash
 * math) and engine/depth_reconstruct_ref.h (depth→world reconstruction). Every
 * function here is deterministic and side-effect free — no RNG state, no hidden
 * globals, same input → same output — so the property-based tests in
 * `heaven_benchmark_tests` (RapidCheck, Filament-free) can hammer them across
 * the whole input space without a GPU, Filament, or SDL dependency. That is why
 * this header is GLM-only: it must compile into the host test target.
 *
 * World convention is Z-up (the Heaven world): "up" is +Z, so the cloud layer
 * drifts horizontally in the world XY plane and casts transmittance down -Z to
 * the ground.
 *
 * ── LOCKSTEP NOTE (read before editing) ─────────────────────────────
 * The functions below are the SINGLE SOURCE OF TRUTH for the cloud math. The
 * `heaven_volumetric_clouds.mat` GPU shader  MUST mirror them
 * BYTE-FOR-BYTE: the same hash constants, the same worley/perlin construction,
 * the same Beer-Lambert extinction, the same Henyey-Greenstein phase, and the
 * same time-driven motion offset. When the cloud `.mat` shader block changes,
 * change it here too (and vice versa) — the property tests
 * (tests/prop_cloud_noise.cpp:  transmittance,  bounded
 * deterministic noise; tests/prop_cloud_noise.cpp  frame-rate-
 * independent motion) assert the two stay in lockstep. This mirrors the
 * weather_shader_ref.h ↔ heaven_rain.mat / heaven_splash.mat precedent.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_CLOUD_NOISE_REF_H_
#define HEAVENBENCH_SRC_ENGINE_CLOUD_NOISE_REF_H_

#include <glm/glm.hpp>

#include <cmath>
#include <numbers>

namespace heaven::cloud_ref {

// ── Shared constants (mirror the material uniforms / literals) ──────

// Mathematical pi, reproduced locally so the phase-function normalisation
// matches the shader's literal exactly (no dependency on <numbers>).
inline constexpr float kPi = std::numbers::pi_v<float>;

// Beer-Lambert extinction coefficient (per unit of accumulated cloud density /
// optical depth). Strictly positive so transmittance is strictly decreasing in
// density. Kept modest so that for the bounded density range the cloud raymarch
// produces, the ground transmittance stays strictly above 0 (it only approaches
// 0 asymptotically as density → ∞; see transmittance()).
inline constexpr float kCloudExtinction = 1.0f;

// Cloud-cover density threshold: ground beneath cloud density above this
// value receives measurably less direct sunlight than ground beneath clear sky
// (zero density). Mirrored in heaven_volumetric_clouds.mat.
inline constexpr float kCloudDensityThreshold = 0.5f;

// Henyey-Greenstein anisotropy for the single-scattering phase function.
// Positive g is forward scattering (clouds are strongly forward-scattering
// toward the sun). Must stay in (-1, 1) for the closed form to be finite.
inline constexpr float kCloudPhaseG = 0.6f;

// Horizontal wind that drifts the cloud layer over time. The world is Z-up, so
// the wind lies in the XY plane (no vertical drift). kCloudWindDir is a unit
// vector; kCloudWindSpeed is world units per second. The motion offset is a
// pure linear function of accumulated time (see cloud_motion_offset).
inline constexpr glm::vec3 kCloudWindDir{
    0.9578262852f, 0.2873478856f, 0.0f};
inline constexpr float kCloudWindSpeed = 0.02f;

// ── GLSL-portable hash (mirror the shader byte-for-byte) ────────────

// A classic Dave-Hoskins style vec3→vec3 hash to [0, 1)^3, used to derive both
// the worley feature points and the perlin corner gradients. Reproduced here so
// the CPU reference and the GPU shader agree on every cell's random data.
// glm::sin / glm::fract are component-wise, matching GLSL semantics.
[[nodiscard]] inline glm::vec3 hash33(const glm::vec3& p) {
    const glm::vec3 q(glm::dot(p, glm::vec3(127.1f, 311.7f, 74.7f)),
                      glm::dot(p, glm::vec3(269.5f, 183.3f, 246.1f)),
                      glm::dot(p, glm::vec3(113.5f, 271.9f, 124.6f)));
    return glm::fract(glm::sin(q) * 43758.5453123f);
}

// ── 3D worley (cellular) noise ──────────────────────────────────────

// Returns the distance to the nearest feature point on a unit lattice: each
// integer cell carries one deterministic feature point (cell + hash33(cell)),
// and the value is the minimum distance from `p` to the feature points in the
// 3×3×3 neighbourhood of cells around it. The raw minimum distance for one
// point per unit cell is bounded well below 1 in practice; the result is
// CLAMPED to [0, 1] so the documented output range is exactly [0, 1] for ANY
// input (this is what  relies on). Purely a function of `p` (the hash
// has no state), so repeated evaluation of the same `p` is bit-identical.

// DOCUMENTED RANGE: [0, 1]. DETERMINISTIC: same `p` → same value.
[[nodiscard]] inline float worley3(const glm::vec3& p) {
    const glm::vec3 pi = glm::floor(p);
    const glm::vec3 pf = p - pi;

    float minDist = 1.0f;   // also the clamp ceiling, so the result is in [0,1]
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const glm::vec3 cell(static_cast<float>(x),
                                     static_cast<float>(y),
                                     static_cast<float>(z));
                // Feature point: the cell origin plus a per-cell random offset
                // in [0,1)^3, expressed relative to `pf` (p inside its own cell).
                const glm::vec3 feature = cell + hash33(pi + cell);
                const glm::vec3 diff    = feature - pf;
                minDist = glm::min(minDist, glm::length(diff));
            }
        }
    }
    return glm::clamp(minDist, 0.0f, 1.0f);
}

// ── 3D perlin (gradient) noise ──────────────────────────────────────

// Classic improved-Perlin gradient noise: the quintic fade
// u = f³(f(6f − 15) + 10) blends the eight corner gradient dot products. Each
// corner gradient is hash33(corner)*2 − 1 ∈ [−1, 1]³ dotted with the corner→p
// vector. Raw gradient noise is centered on 0 and bounded by ~√3 in magnitude;
// it is remapped to [0, 1] via value*0.5 + 0.5 and then CLAMPED, so the
// documented output range is exactly [0, 1] for ANY input. Purely
// a function of `p`, so it is deterministic with no hidden state.

// DOCUMENTED RANGE: [0, 1]. DETERMINISTIC: same `p` → same value.
[[nodiscard]] inline float perlin3(const glm::vec3& p) {
    const glm::vec3 pi = glm::floor(p);
    const glm::vec3 pf = p - pi;

    // Quintic fade weights (component-wise).
    const glm::vec3 u = pf * pf * pf * (pf * (pf * 6.0f - 15.0f) + 10.0f);

    // Gradient contribution at a corner: a random gradient in [-1,1]^3 dotted
    // with the vector from that corner to `p`.
    auto corner = [&](float ox, float oy, float oz) -> float {
        const glm::vec3 o(ox, oy, oz);
        const glm::vec3 g = hash33(pi + o) * 2.0f - 1.0f;
        return glm::dot(g, pf - o);
    };

    const float n000 = corner(0.0f, 0.0f, 0.0f);
    const float n100 = corner(1.0f, 0.0f, 0.0f);
    const float n010 = corner(0.0f, 1.0f, 0.0f);
    const float n110 = corner(1.0f, 1.0f, 0.0f);
    const float n001 = corner(0.0f, 0.0f, 1.0f);
    const float n101 = corner(1.0f, 0.0f, 1.0f);
    const float n011 = corner(0.0f, 1.0f, 1.0f);
    const float n111 = corner(1.0f, 1.0f, 1.0f);

    // Trilinear interpolation with the faded weights.
    const float nx00 = glm::mix(n000, n100, u.x);
    const float nx10 = glm::mix(n010, n110, u.x);
    const float nx01 = glm::mix(n001, n101, u.x);
    const float nx11 = glm::mix(n011, n111, u.x);
    const float nxy0 = glm::mix(nx00, nx10, u.y);
    const float nxy1 = glm::mix(nx01, nx11, u.y);
    const float value = glm::mix(nxy0, nxy1, u.z);

    return glm::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f);
}

// ── Beer-Lambert ground transmittance ───────────────────────────────

// Fraction of direct sunlight that survives a cloud column of accumulated
// density (optical depth) `density`: T(d) = exp(-kCloudExtinction * d).

// PROPERTIES:
//   - T(0) == 1 exactly (clear sky transmits all light).
//   - T(d) ∈ (0, 1] for all finite d ≥ 0 (strictly positive; it only
//     approaches 0 asymptotically as d → ∞). Callers feed the bounded density
//     a raymarch accumulates, so it never underflows to exactly 0.
//   - T is monotonically NON-INCREASING in density: for d1 ≤ d2,
//     T(d2) ≤ T(d1), because kCloudExtinction > 0 makes exp(-k·d) decreasing.
//   - Consequently, for any density above kCloudDensityThreshold the direct
//     sunlight reaching the ground is strictly less than under clear sky.

// Negative density inputs are clamped to 0 (a column cannot have negative
// optical depth), so T is well defined and ≤ 1 across the whole real line.
[[nodiscard]] inline float transmittance(float density) {
    const float d = glm::max(density, 0.0f);
    return std::exp(-kCloudExtinction * d);
}

// ── Single-scattering phase function (Henyey-Greenstein) ────────────

// The probability density of scattering through an angle whose cosine is
// `cosTheta` for anisotropy `g` ∈ (-1, 1):

//     p(θ) = (1 - g²) / (4π · (1 + g² - 2g·cosTheta)^{3/2})

// Always strictly positive (the denominator is positive for |g| < 1), and
// integrates to 1 over the sphere. g > 0 is forward scattering (toward the
// light), matching the bright silver lining clouds show toward the sun. The
// denominator base is floored to a tiny positive value to stay finite at the
// |g| → 1 / cosTheta → 1 limit. Pure function of (cosTheta, g).
[[nodiscard]] inline float henyey_greenstein(float cosTheta, float g) {
    const float g2    = g * g;
    const float base  = glm::max(1.0f + g2 - 2.0f * g * cosTheta, 1.0e-6f);
    const float denom = 4.0f * kPi * std::pow(base, 1.5f);
    return (1.0f - g2) / denom;
}

// Convenience overload using the configured cloud anisotropy kCloudPhaseG, so
// callers (and the shader) share the single tuned g value.
[[nodiscard]] inline float cloud_phase(float cosTheta) {
    return henyey_greenstein(cosTheta, kCloudPhaseG);
}

// ── Time-driven cloud-motion offset ─────────────────────────────────

// The world-space offset (XY drift, Z-up world ⇒ no vertical motion) applied to
// the cloud noise sample positions at accumulated Frame_Time_Source `time`:

//     offset(t) = kCloudWindDir * kCloudWindSpeed * t

// FRAME-RATE INDEPENDENCE: this is a PURE linear function
// of the absolute accumulated time `time` (sourced from renderer_->getUserTime()
// in the feature). It carries no per-frame state and no dependence on frame
// count or cadence, so equal accumulated time yields an identical offset
// regardless of how that time was reached — a 4000 FPS run and a 30 FPS run that
// have been running the same number of real seconds sample the clouds at the
// same place (up to the float accumulation error of summing many small dt, which
// is what the property test bounds). The shader advances motion the same way
// from FrameContext::time.
[[nodiscard]] inline glm::vec3 cloud_motion_offset(float time) {
    return kCloudWindDir * (kCloudWindSpeed * time);
}

} // namespace heaven::cloud_ref

#endif // HEAVENBENCH_SRC_ENGINE_CLOUD_NOISE_REF_H_
