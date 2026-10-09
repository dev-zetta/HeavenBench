// Feature: weather-and-rendering-features,: Particle animation is frame-rate independent
/*
 * Particle animation is frame-rate independent.
 *
 *
 * The weather particle materials drive ALL motion from a single absolute
 * `time` uniform (falling streaks,  splash ripples), and the CPU
 * mirror in engine/weather_shader_ref.h reflects this exactly:
 *
 *   - streak_world_position(seed, cameraPos, extent, time, fallSpeed) places a
 *     streak on the shared drop lattice: XY is the cell's jittered column (no
 *     time dependence) and Z is the shared fall law drop_z (periodic in the
 *     cell's drop_fall_period, fall span fixed at 2*extent).
 *   - splash_age(time, spawnPhase, period) = mod(time - spawnPhase,
 *     min(period, 2.0)).
 *
 * Both are PURE functions of the absolute wall-clock time `time` — they do not
 * depend on how that time was accumulated. So the rendered particle state at a
 * given wall-clock time t is identical regardless of the per-frame cadence used
 * to reach t. That is precisely frame-rate independence: a 4000 FPS run and a
 * 30 FPS run that have both been running for the same number of real seconds
 * show the same rain.
 *
 * The test realizes "the same wall-clock time reached two ways":
 *
 *   - t_summed: a list of positive per-frame dt steps (each a plausible frame
 *     time for 30..4000 FPS) accumulated into a float frame-by-frame,
 *     exactly as the renderer accumulates getUserTime() across many frames.
 *   - t_single: the same total elapsed time computed in double precision and
 *     applied as one step. This is the identical wall-clock instant reached by
 *     a single frame instead of many.
 *
 * The ONLY thing that can differ between the two cadences is the float rounding
 * introduced by summing many small dt values (dtErr = |t_summed - t_single|),
 * so the property asserts:
 *
 *   - Streak X/Y now carry a WIND drift. The horizontal position is the cell's
 *     column (time-independent, bit-identical) PLUS a wind drift kWindDir *
 *     wind_strength(time) * fallenDist, where fallenDist = dropTopZ - drop_z is
 *     itself a pure function of the absolute time. So XY is no longer
 *     time-independent — but it is still a PURE function of the absolute time,
 *     hence frame-rate independent up to the float-accumulation error. The drift
 *     acts entirely along kWindDir, so projecting the XY offset onto kWindDir
 *     gives the scalar s = wind_strength*fallenDist; because fallenDist lives on
 *     a circle of circumference (2*extent) (it is dropTopZ - z with z periodic),
 *     s lives on a circle of circumference wind_strength*(2*extent). The two
 *     cadences' s agree within the time-accumulation error propagated through
 *     the fall (wind_strength * fallSpeed * dtErr) plus the gust's own time
 *     variation (the wind_strength change over dtErr), measured wrap-aware.
 *
 *   - Streak Z (the falling axis) agrees within a tolerance that is exactly the
 *     time-accumulation error propagated through the fall: fallSpeed * dtErr
 *     plus a small constant for in-shader float rounding, measured with the
 *     periodic (wrap-aware) circular distance over the 2*extent fall span.
 *   - The splash animation age agrees within dtErr plus a small constant, again
 *     using the wrap-aware circular distance over the (clamped) ripple period.
 *
 *
 * Because the position/age error is fully explained by (and proportional to)
 * the cadence's float-summation error — with no extra divergence — the particle
 * animation is frame-rate independent.
 *
 * Generators are finite and bounded (integer grids mapped onto closed float
 * intervals; no NaN/Inf), so the periodic comparisons are well defined. The
 * functions under test are header-only and GLM-only (no GPU/Filament/SDL), so
 * this runs on the host. This translation unit self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point; no second main() is defined.
 */
#include <cmath>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "engine/weather_shader_ref.h"

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
namespace heaven::testing {

using TestFn = std::function<bool()>;

std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

namespace wr = heaven::weather_ref;

// Map an integer grid onto a closed [lo, hi] float interval so the endpoints
// are exactly reachable while the interior is covered densely.
rc::Gen<float> ranged(float lo, float hi) {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001),
                        [lo, hi](int g) {
                            return lo + (static_cast<float>(g) / 1'000'000.0f) * (hi - lo);
                        });
}

// Wrap-aware distance between two values that live on a circle of circumference
// `mod` (both already in [0, mod)). Equals the smaller of the two arc lengths,
// so it is continuous across the wrap boundary where a raw subtraction would
// jump by ~mod. This is the correct metric for the periodic streak fall and the
// periodic splash age.
float circular_dist(float a, float b, float mod) {
    float d = std::fabs(a - b);
    if (mod > 0.0f) {
        d = wr::glsl_mod(d, mod);
        d = std::min(d, mod - d);
    }
    return d;
}

bool run_particle_framerate_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: weather-and-rendering-features, Property 4: Particle "
        "animation is frame-rate independent (Validates: Requirements 1.2, 4.2)",
        [] {
            // ── Build the same wall-clock time two ways ────────────────────
            // Per-frame dt drawn from plausible frame times for 30..4000 FPS
            // dt in [1/4000, 1/30] s. n frames spans a single big step
            // (n == 1) up to a long many-frame run.
            const int n = *rc::gen::inRange<int>(1, 601);
            const auto gen_dt = ranged(1.0f / 4000.0f, 1.0f / 30.0f);

            float t_summed = 0.0f;   // float frame-by-frame accumulation (many frames)
            double t_total = 0.0;    // double-precision running total
            for (int i = 0; i < n; ++i) {
                const float dt = *gen_dt;
                t_summed += dt;      // exactly how the renderer accumulates time
                t_total += static_cast<double>(dt);
            }
            // Same elapsed wall-clock time reached in ONE step.
            const float t_single = static_cast<float>(t_total);

            // The only divergence between the two cadences is float summation.
            const float dtErr = std::fabs(t_summed - t_single);

            // ── Streak fall (heaven_rain.mat) ────────────────────────
            const glm::vec3 seed(*ranged(0.0f, 1.0f),
                                 *ranged(0.0f, 1.0f),
                                 *ranged(0.0f, 1.0f));
            const glm::vec3 cameraPos(*ranged(-100.0f, 100.0f),
                                      *ranged(-100.0f, 100.0f),
                                      *ranged(-100.0f, 100.0f));
            const float volumeExtent = *ranged(5.0f, 50.0f);
            const float fallSpeed    = *ranged(0.5f, 8.0f);

            const glm::vec3 pSummed =
                wr::streak_world_position(seed, cameraPos, volumeExtent, t_summed, fallSpeed);
            const glm::vec3 pSingle =
                wr::streak_world_position(seed, cameraPos, volumeExtent, t_single, fallSpeed);

            // Recover the cell + per-cell fall speed (shared by the XY-wind and
            // the Z bounds below).
            const glm::vec2 cell =
                wr::streak_seed_cell(seed, cameraPos, volumeExtent);
            const float vSpeed =
                wr::drop_fall_speed(cell, volumeExtent, fallSpeed);
            const float span = 2.0f * volumeExtent;

            // UNIFICATION + WIND UPDATE: the streak XY is now the shared-lattice
            // cell's jittered column (drop_column_xy, time-independent) PLUS a
            // horizontal wind drift kWindDir * wind_strength(time) * fallenDist,
            // where fallenDist = dropTopZ - z is a pure function of the absolute
            // time. So XY is still a PURE function of the absolute time — frame-
            // rate independent up to the float-accumulation error — but no longer
            // bit-identical. Subtract the deterministic column (identical for both
            // cadences) to isolate the wind drift vector, then check:
            //   - the component PERPENDICULAR to kWindDir is ~0 (the drift is
            //     entirely along kWindDir), so it agrees to within float rounding;
            //   - the component ALONG kWindDir is s = wind_strength*fallenDist,
            //     which lives on a circle of circumference kWindStrengthMax*span
            //     (fallenDist sweeps [0, span) and jumps at the fall wrap), so it
            //     is compared wrap-aware over that circumference.
            const glm::vec2 col = wr::drop_column_xy(cell, wr::kDropSpacing);
            const glm::vec2 driftSummed(pSummed.x - col.x, pSummed.y - col.y);
            const glm::vec2 driftSingle(pSingle.x - col.x, pSingle.y - col.y);
            const glm::vec2 perp(-wr::kWindDir.y, wr::kWindDir.x);

            // Perpendicular (no-wind) component: ~0 for both, agree to rounding.
            const float perpSummed = glm::dot(driftSummed, perp);
            const float perpSingle = glm::dot(driftSingle, perp);
            RC_ASSERT(std::fabs(perpSummed - perpSingle) <= 1.0e-2f);

            // Along-wind component, wrap-aware over wind_strength*span. The wind
            // drift s = wind_strength*fallenDist is periodic on a circle of
            // circumference ≈ kWindStrengthMax*span (fallenDist sweeps [0, span)
            // and jumps at the fall recycle). Two contributions to the tolerance:
            //   - AWAY from the fall wrap, s differs by the time-accumulation
            //     error propagated through the fall (kWindStrengthMax*vSpeed) and
            //     the gust's own slew (amp*freq*span), times dtErr;
            //   - AT a fall-wrap straddle the wrap-aware distance leaves a small
            //     residual (kWindStrengthMax - wind_strength)*span <= 2*amp*span
            //     because the circle uses the MAX strength while the actual
            //     strength is slightly less.
            const float sSummed = glm::dot(driftSummed, wr::kWindDir);
            const float sSingle = glm::dot(driftSingle, wr::kWindDir);
            const float windCirc = wr::kWindStrengthMax * span;
            const float sDist = circular_dist(
                wr::glsl_mod(sSummed, windCirc),
                wr::glsl_mod(sSingle, windCirc), windCirc);
            const float windTol =
                (wr::kWindStrengthMax * vSpeed
                 + wr::kWindStrengthAmp * wr::kWindGustFreq * span) * dtErr
                + 2.0f * wr::kWindStrengthAmp * span   // fall-wrap straddle residual
                + 3.0e-2f;
            RC_ASSERT(sDist <= windTol);

            // The falling (Z) axis is drop_z, periodic with span = 2*volumeExtent
            // (the fall span is fixed at 2E; only the per-cell speed varies). Its
            // difference is the time-accumulation error propagated through the
            // fall at the CELL's actual speed drop_fall_speed (= 2E/period), plus
            // a small constant for the float rounding inside the shader's
            // mod/argument evaluation.
            const float zDist = circular_dist(pSummed.z, pSingle.z, span);
            const float zTol  = vSpeed * dtErr + 1.0e-2f;
            RC_ASSERT(zDist <= zTol);

            // ── Splash ripple animation (heaven_splash.mat) ──────────
            const float spawnPhase = *ranged(0.0f, 4.0f);
            // Cover both the unclamped and the clamped (period > 2.0 s) paths;
            // the active modulus is min(period, kSplashMaxPeriod).
            const float period = *ranged(0.1f, 3.0f);

            const float ageSummed = wr::splash_age(t_summed, spawnPhase, period);
            const float ageSingle = wr::splash_age(t_single, spawnPhase, period);

            // Age advances 1:1 with time, so its periodic difference is the
            // accumulation error itself plus a small float-rounding constant.
            const float ageMod  = std::min(period, wr::kSplashMaxPeriod);
            const float ageDist = circular_dist(ageSummed, ageSingle, ageMod);
            const float ageTol  = dtErr + 2.0e-3f;
            RC_ASSERT(ageDist <= ageTol);
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_particle_framerate{&run_particle_framerate_property};

} // namespace
