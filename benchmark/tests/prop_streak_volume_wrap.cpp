// Feature: weather-and-rendering-features,: Streaks stay in a camera-centered bounded volume via periodic wrap
/*
 * Streaks stay in a camera-centered bounded volume via periodic wrap.
 *
 *
 * UNIFICATION UPDATE: the falling-rain material no longer positions a streak as
 * an arbitrary hash3 cell with its own fall term. A streak is now a DROP ON THE
 * SHARED LATTICE: its world XY is a lattice cell's jittered column
 * (drop_column_xy) and its Z is the shared fall law (drop_z), with the fall span
 * FIXED at exactly 2*volumeExtent and only the per-cell SPEED varying. The CPU
 * mirror in engine/weather_shader_ref.h::streak_world_position encodes exactly
 * that, so this property is re-expressed for the new law (NOT deleted):
 *
 *     cell    = streak_seed_cell(seed, cameraPos, E); world-anchored
 *     col     = drop_column_xy(cell, kDropSpacing); jittered XY
 *     period  = drop_fall_period(cell, E, fallSpeed); per-cell, varies
 *     vSpeed  = drop_fall_speed (cell, E, fallSpeed); = 2E / period
 *     phase   = drop_phase(cell, period);
 *     z       = drop_z(phase, cameraZ + E, vSpeed, period, time);
 *     world   = vec3(col.x, col.y, z);
 *
 * This property asserts the two clauses the reference is the source of truth for:
 *
 *    Bounded camera-centered volume — the clause the implementation tradeoff was
 *     approved to preserve. Because the fall span is fixed at 2E (vSpeed*period
 *     == 2E), z = drop_z sweeps (cameraZ - E, cameraZ + E] EXACTLY, so the Z
 *     offset from the camera is always within [-E, E] — the streak never leaves
 *     the camera-centered cube on the falling axis no matter the per-cell speed.
 *     WIND is horizontal only, so the Z bound is UNCHANGED. The XY column is the
 *     lattice cell snapped from an absolute position within [camXY - E, camXY + E);
 *     snapping + sub-cell jitter can push the column out by at most one lattice
 *     spacing, AND the wind now drifts the drop horizontally by up to
 *     kWindStrengthMax * (fall span 2E) along kWindDir, so the XY offset is within
 *     [-(E + kDropSpacing + maxWindDrift), E + kDropSpacing + maxWindDrift] with
 *     maxWindDrift = kWindStrengthMax * 2E. Still a bounded horizontal window,
 *     just slightly larger to admit the slant.
 *
 *    Periodic wrap keeps density constant. drop_z is exactly periodic in
 *     time with the cell's drop_fall_period (the `mod(time - phase, period)`
 *     wrap): a streak crossing the lower boundary re-enters at the top, so the Z
 *     at `time` equals the Z at `time + period`. The XY is the column plus the
 *     wind drift kWindDir * wind_strength(time) * fallenDist; the column and
 *     fallenDist repeat after one period, so X and Y repeat up to the wind
 *     drift's TIME variation over one period (wind_strength is a slow function of
 *     absolute time, so it differs by the gust change across the period) — a
 *     bounded difference, not bit-identical, since wind is genuinely time-driven.
 *
 * Generators are finite and bounded (integer grids mapped onto closed float
 * intervals; no NaN/Inf) with fallSpeed and volumeExtent strictly positive, so
 * the volume bounds and the periodic comparison are well defined. The function
 * under test is header-only and GLM-only (no GPU/Filament/SDL), so this runs on
 * the host. This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "engine/weather_shader_ref.h"

// The registry lives in test_main.cpp (external linkage, no shared header), so
// mirror its minimal interface here to append our test. The struct definition
// is token-identical to the one in test_main.cpp, satisfying the ODR.
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

// Wrap-aware distance between two values on a circle of circumference `mod`.
// Equals the smaller of the two arc lengths, so it stays continuous across the
// wrap boundary where a raw subtraction would jump by ~mod. This is the correct
// metric for the periodic streak fall.
float circular_dist(float a, float b, float mod) {
    float d = std::fabs(a - b);
    if (mod > 0.0f) {
        d = wr::glsl_mod(d, mod);
        d = std::min(d, mod - d);
    }
    return d;
}

bool run_streak_volume_wrap_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: weather-and-rendering-features, Property 3: Streaks stay in a "
        "camera-centered bounded volume via periodic wrap (Validates: "
        "Requirements 1.4, 1.5)",
        [] {
            // Per-streak seed in the normalized [0,1]^3 space the rain material
            // uses; camera anywhere in a wide world box; volume extent and fall
            // speed strictly positive (the preconditions of).
            const glm::vec3 seed(*ranged(0.0f, 1.0f),
                                 *ranged(0.0f, 1.0f),
                                 *ranged(0.0f, 1.0f));
            const glm::vec3 cameraPos(*ranged(-100.0f, 100.0f),
                                      *ranged(-100.0f, 100.0f),
                                      *ranged(-100.0f, 100.0f));
            const float volumeExtent = *ranged(5.0f, 50.0f);
            const float fallSpeed    = *ranged(0.5f, 8.0f);
            const float time         = *ranged(0.0f, 100.0f);

            const glm::vec3 pos =
                wr::streak_world_position(seed, cameraPos, volumeExtent, time, fallSpeed);

            // ──  Bounded, camera-centered volume ─────────────────────
            // The falling Z axis is the clause the implementation tradeoff preserves:
            // with the fall span fixed at 2E and only the per-cell speed varying,
            // drop_z sweeps (cameraZ - E, cameraZ + E], so the Z offset is within
            // [-E, E]. Wind is horizontal, so the Z bound is unchanged. The XY
            // column is the lattice cell snapped from an absolute position in
            // [camXY - E, camXY + E); the floor() snap plus the sub-cell jitter
            // can push the column out by at most one lattice spacing, AND the
            // wind drifts the drop horizontally by up to kWindStrengthMax times
            // the full fall span (2E) along kWindDir. So the XY offset bound is
            // E + kDropSpacing + maxWindDrift. Recovering the offset from
            // (pos - cameraPos) reintroduces a small rounding when the camera
            // coordinate is large; absorb it with a tolerance scaling with extent.
            const glm::vec3 offset = pos - cameraPos;
            const float boundTol = 1.0e-3f + 1.0e-4f * volumeExtent;
            const float maxWindDrift = wr::kWindStrengthMax * (2.0f * volumeExtent);
            const float xyBound  = volumeExtent + wr::kDropSpacing + maxWindDrift;

            RC_ASSERT(offset.x >= -xyBound - boundTol);
            RC_ASSERT(offset.x <= xyBound + boundTol);
            RC_ASSERT(offset.y >= -xyBound - boundTol);
            RC_ASSERT(offset.y <= xyBound + boundTol);
            // The falling axis stays strictly inside the camera-centered cube.
            RC_ASSERT(offset.z >= -volumeExtent - boundTol);
            RC_ASSERT(offset.z <= volumeExtent + boundTol);

            // ──  Periodic wrap keeps the streak in the volume ────────
            // After one cell fall period the streak has fallen a full 2E and
            // re-entered at the top, so the column + fall repeat. The period is
            // the per-cell drop_fall_period (the fall law varies speed per cell),
            // recovered from the cell this seed occupies.
            const glm::vec2 cell =
                wr::streak_seed_cell(seed, cameraPos, volumeExtent);
            const float period =
                wr::drop_fall_period(cell, volumeExtent, fallSpeed);
            const glm::vec3 posNext = wr::streak_world_position(
                seed, cameraPos, volumeExtent, time + period, fallSpeed);

            // The XY is the (time-independent) column PLUS the wind drift, which
            // is a slow function of absolute time. Over one period the column +
            // fallen distance repeat, so X and Y repeat up to the wind drift's
            // change across the period: |wind_strength(time+period) -
            // wind_strength(time)| * fallenDist, bounded by the full gust swing
            // (2*amp) times the fall span (2E). Wind is genuinely time-driven, so
            // this is a bounded difference, not bit-identical.
            const float windXYTol =
                2.0f * wr::kWindStrengthAmp * (2.0f * volumeExtent) + 1.0e-2f;
            RC_ASSERT(std::fabs(posNext.x - pos.x) <= windXYTol);
            RC_ASSERT(std::fabs(posNext.y - pos.y) <= windXYTol);

            // The falling Z axis is periodic over span = 2*extent; its only
            // divergence across one period is the float rounding of the wrap's
            // mod/argument evaluation, measured with the wrap-aware distance.
            const float span = 2.0f * volumeExtent;
            const float zDist = circular_dist(pos.z, posNext.z, span);
            RC_ASSERT(zDist <= 5.0e-2f);

            // The wrapped position must itself still satisfy the volume bound.
            const glm::vec3 offsetNext = posNext - cameraPos;
            RC_ASSERT(offsetNext.z >= -volumeExtent - boundTol);
            RC_ASSERT(offsetNext.z <= volumeExtent + boundTol);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_streak_volume_wrap{
    &run_streak_volume_wrap_property};

} // namespace
