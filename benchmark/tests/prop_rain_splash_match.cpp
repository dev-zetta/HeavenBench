// Feature: weather-and-rendering-features,: Streak↔splash correspondence (shared lattice + fall law)
/*
 * A falling streak and the splash it produces are driven by the
 * SAME shared raindrop lattice + fall law, so the splash blooms at the exact
 * world XY where the streak lands, at the exact moment it lands.
 * (fixed span keeps the drop in the volume), 4.2
 * (impact-driven ripple timing is frame-rate independent), 4.6 (ripple lifetime
 * <= 2.0 s).
 *
 * THE UNIFICATION THIS PINS DOWN
 * ------------------------------
 * Both the streak shader (heaven_rain.mat) and the splash shader
 * (heaven_splash.mat) derive a falling drop from the same world-anchored XY
 * lattice and the same fall law in engine/weather_shader_ref.h:
 *
 *   - A drop falls from dropTopZ = cameraZ + volumeExtent at a per-cell constant
 *     speed drop_fall_speed (= 2*volumeExtent / drop_fall_period), recycling with
 *     period drop_fall_period. The fall SPAN is FIXED at exactly 2*volumeExtent
 *     (the approved tradeoff: vary per-cell SPEED, not span), so every drop stays
 *     inside the camera-centered cube  and the closed-form impact time is
 *     exact for any ground height within the fall band.
 *   - drop_z(cellPhase, dropTopZ, fallSpeed, fallPeriod, t) is the drop's world Z
 *     at time t; drop_impact_time(...) is the most-recent time <= t at which that
 *     Z crossed the ground height groundZ.
 *
 * The splash shader spawns a ripple in a ground cell at drop_impact_time and
 * animates it over [impactT, impactT + drop_ripple_duration). This test proves,
 * offline and GPU-free, the four correspondences that make that correct:
 *
 *   1. AT IMPACT THE DROP IS AT THE GROUND. For a drop whose ground height lies
 *      within its fall band, drop_z(cellPhase, dropTopZ, fallSpeed, fallPeriod,
 *      impactT) == groundZ (within float tolerance). The splash therefore spawns
 *      exactly when the streak reaches that ground height — same place, same time.
 *
 *   2. MOST-RECENT IMPACT, NO SKIPPED / FUTURE CYCLE. impactT <= time and
 *      time - impactT ∈ [0, fallPeriod): the spawn is the latest landing at or
 *      before now, never a future spawn and never an older cycle.
 *
 *   3. RIPPLE LIFETIME <= 2.0 s. drop_ripple_duration(cell) is hard-clamped
 *      to kSplashMaxPeriod (2.0 s) for every cell, so every impact effect
 *      completes and is removed within 2.0 s.
 *
 *   4. FRAME-RATE INDEPENDENCE. Reaching the same wall-clock `time` as a
 *      sum of many per-frame dt steps vs a single step yields the same ripple age
 *      (time - impactT) within the float-accumulation tolerance — the impact /
 *      age is a pure function of the absolute time, so the splash animation does
 *      not depend on the frame cadence.
 *
 *   5. SPLASH BLOOMS UNDER THE SLANTED DROP'S LANDING POINT (wind). With wind the
 *      drop drifts horizontally as it falls, so it lands DOWNWIND of its spawn
 *      column. The splash ripple centre must therefore be the drop's XY AT THE
 *      GROUND, not its spawn column. This test asserts the shared formula
 *      drop_xy_at_z(cell, dropTopZ, groundZ, wind_strength(impactT)) (which the
 *      streak uses for its landing point) equals the splash centre formula
 *      drop_column_xy(cell) + kWindDir*wind_strength(impactT)*(dropTopZ-groundZ),
 *      for random wind — i.e. the streak and splash coincide under wind. At
 *      wind_strength → 0 both reduce to the column, so the no-wind match holds.
 *
 * Generators are finite and bounded (integer grids mapped onto closed float
 * intervals; no NaN/Inf) with fallSpeed and volumeExtent strictly positive and
 * groundZ confined to the drop's fall band, so the closed forms are well defined.
 * The functions under test are header-only and GLM-only (no GPU/Filament/SDL), so
 * this runs on the host. This translation unit self-registers with the RapidCheck
 * harness in test_main.cpp via the heaven::testing::Register pattern, so the
 * file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the entry
 * point; no second main() is defined.
 */
#include <algorithm>
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

// Wrap-aware distance between two values on a circle of circumference `mod`
// (both already in [0, mod)). Equals the smaller of the two arc lengths, so it
// stays continuous across the wrap boundary where a raw subtraction would jump
// by ~mod. This is the correct metric for the periodic ripple age.
float circular_dist(float a, float b, float mod) {
    float d = std::fabs(a - b);
    if (mod > 0.0f) {
        d = wr::glsl_mod(d, mod);
        d = std::min(d, mod - d);
    }
    return d;
}

bool run_rain_splash_match_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: weather-and-rendering-features, Property 12: Streak<->splash "
        "correspondence via the shared lattice + fall law (Validates: "
        "Requirements 1.4, 4.2, 4.6)",
        [] {
            // ── Shared lattice cell + camera-centered volume ──────────────
            // A cell id anywhere on the world lattice; the camera anywhere in a
            // wide world box; fall speed and volume extent strictly positive
            // (the preconditions of the closed forms).
            const glm::vec2 cell(*ranged(-500.0f, 500.0f),
                                 *ranged(-500.0f, 500.0f));
            const float cameraZ      = *ranged(-100.0f, 100.0f);
            const float volumeExtent = *ranged(5.0f, 70.0f);
            const float fallSpeed    = *ranged(0.5f, 30.0f);
            const float time         = *ranged(0.0f, 200.0f);

            // Per-cell fall law (mirrors the streak + splash shaders exactly).
            const float fallPeriod = wr::drop_fall_period(cell, volumeExtent, fallSpeed);
            const float fallSpeedC = wr::drop_fall_speed(cell, volumeExtent, fallSpeed);
            const float cellPhase  = wr::drop_phase(cell, fallPeriod);
            const float dropTopZ   = wr::drop_top_z(cameraZ, volumeExtent);
            const float fallSpan   = 2.0f * volumeExtent;

            // Sanity on the fixed-span tradeoff: speed * period == 2*extent, so
            // the drop sweeps exactly the camera-centered cube on its fall axis
            // (this is what keeps  /  intact with varied speed).
            RC_ASSERT(std::fabs(fallSpeedC * fallPeriod - fallSpan)
                      <= 1.0e-3f + 1.0e-4f * fallSpan);

            // groundZ strictly inside the drop's fall band
            // (dropTopZ - 2*extent, dropTopZ] = (cameraZ - extent, cameraZ + extent].
            // We keep a small margin off both ends so tau ∈ (0, fallPeriod) and the
            // closed form is unambiguous under float rounding.
            const float bandFrac = *ranged(0.02f, 0.98f);
            const float groundZ  = dropTopZ - bandFrac * fallSpan;

            // Tolerance that scales with the world magnitudes involved: the
            // drop_z reconstruction subtracts fallSpeed*tau (≈ dropTopZ - groundZ)
            // from dropTopZ, so the cancellation error grows with |dropTopZ| and
            // the fall span.
            const float magTol =
                1.0e-2f + 1.0e-4f * (std::fabs(dropTopZ) + fallSpan);

            // ── (1) At impact the drop is AT the ground ───────────────────
            const float impactT = wr::drop_impact_time(
                cellPhase, dropTopZ, fallSpeedC, fallPeriod, groundZ, time);
            const float zAtImpact = wr::drop_z(
                cellPhase, dropTopZ, fallSpeedC, fallPeriod, impactT);
            RC_ASSERT(std::fabs(zAtImpact - groundZ) <= magTol);

            // ── (2) Most-recent impact, no skipped / future cycle ─────────
            const float ageAtTime = time - impactT;
            // impactT <= time (the spawn is at or before now).
            RC_ASSERT(ageAtTime >= -1.0e-3f);
            // time - impactT ∈ [0, fallPeriod): the latest landing, not an
            // older cycle.
            RC_ASSERT(ageAtTime < fallPeriod + 1.0e-3f);

            // ── (3) Ripple lifetime <= 2.0 s  ───────────────────────
            const float rippleDur = wr::drop_ripple_duration(cell);
            RC_ASSERT(rippleDur <= wr::kSplashMaxPeriod);
            RC_ASSERT(rippleDur > 0.0f);

            // ── (4) Frame-rate independence of the impact / age ───────────
            // Reach the same wall-clock `time` two ways: a sum of many plausible
            // per-frame dt steps (30..4000 FPS) accumulated in float, vs a
            // single double-precision step. The only divergence is the float
            // summation error dtErr; the ripple age (time - impactT) is a pure
            // function of the absolute time, so it agrees within dtErr (measured
            // wrap-aware over the periodic fall).
            const int n = *rc::gen::inRange<int>(1, 401);
            const auto gen_dt = ranged(1.0f / 4000.0f, 1.0f / 30.0f);

            float  t_summed = 0.0f;
            double t_total  = 0.0;
            for (int i = 0; i < n; ++i) {
                const float dt = *gen_dt;
                t_summed += dt;
                t_total  += static_cast<double>(dt);
            }
            const float t_single = static_cast<float>(t_total);
            const float dtErr = std::fabs(t_summed - t_single);

            const float impactSummed = wr::drop_impact_time(
                cellPhase, dropTopZ, fallSpeedC, fallPeriod, groundZ, t_summed);
            const float impactSingle = wr::drop_impact_time(
                cellPhase, dropTopZ, fallSpeedC, fallPeriod, groundZ, t_single);

            // The ripple age advances 1:1 with time, so its periodic difference
            // is the accumulation error itself plus a small float-rounding
            // constant — the splash animation is frame-rate independent.
            const float ageSummed = t_summed - impactSummed;
            const float ageSingle = t_single - impactSingle;
            const float ageDist = circular_dist(
                wr::glsl_mod(ageSummed, fallPeriod),
                wr::glsl_mod(ageSingle, fallPeriod), fallPeriod);
            RC_ASSERT(ageDist <= dtErr + 2.0e-3f);

            // ── (5) Splash blooms under the slanted drop's landing point ──
            // With wind the drop drifts downwind as it falls, so it lands at
            // drop_xy_at_z(cell, dropTopZ, groundZ, wind_strength(impactT)) — its
            // column PLUS the wind drift accumulated over the full fall to the
            // ground, evaluated at the gust strength AT IMPACT. The splash shader
            // places its ripple centre with the IDENTICAL formula:
            //   centre = drop_column_xy(cell) + kWindDir*windStrength*(dropTopZ-groundZ)
            // so the two coincide by construction. We assert the shared formula
            // matches that explicit centre for a RANDOM gust strength (covering
            // any time-varying wind, including 0 where both reduce to the column).
            const float windStrengthAtImpact = wr::wind_strength(impactT);
            const glm::vec2 streakLandingXY = wr::drop_xy_at_z(
                cell, dropTopZ, groundZ, windStrengthAtImpact);
            const glm::vec2 splashCentre =
                wr::drop_column_xy(cell, wr::kDropSpacing)
                + wr::kWindDir * windStrengthAtImpact * (dropTopZ - groundZ);
            // Tolerance scales with the drift magnitude (the products grow with
            // |dropTopZ - groundZ| and the world cell magnitude).
            const float matchTol =
                1.0e-3f + 1.0e-4f * (std::fabs(dropTopZ) + fallSpan
                                     + std::fabs(cell.x) + std::fabs(cell.y));
            RC_ASSERT(std::fabs(streakLandingXY.x - splashCentre.x) <= matchTol);
            RC_ASSERT(std::fabs(streakLandingXY.y - splashCentre.y) <= matchTol);

            // And at impact the drop is at the ground height, so the streak's
            // landing point is exactly where the splash blooms AT THE MOMENT it
            // blooms — the headline streak↔splash coincidence, now under wind.
            const float zAtLanding = wr::drop_z(
                cellPhase, dropTopZ, fallSpeedC, fallPeriod, impactT);
            RC_ASSERT(std::fabs(zAtLanding - groundZ) <= magTol);
        });
}

// Register the property with the harness at static-init time.
const heaven::testing::Register reg_rain_splash_match{&run_rain_splash_match_property};

} // namespace
