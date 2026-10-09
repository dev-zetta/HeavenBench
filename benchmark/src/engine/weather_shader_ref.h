/*
 * HeavenBench — Weather shader reference math (CPU mirror)
 *
 * Header-only, GLM-only, pure free functions that mirror the vertex/fragment
 * shader math used by the weather materials:
 *
 *   - heaven_rain.mat   (falling precipitation streaks)
 *   - heaven_splash.mat (rain-impact ripples)
 *   - heaven_lit.mat / heaven_lit_twosided.mat (the wetness shading block)
 *
 * The renderer cannot run its `.mat` shaders on the host, so this header is the
 * authoritative CPU reference the GPU shaders are written to match exactly.
 * Every function here is deterministic and side-effect free so the
 * property-based tests in `heaven_benchmark_tests` can hammer them across the
 * whole input space without a GPU, Filament, or SDL dependency.
 *
 * World convention is Z-up (the Heaven world): "up" is +Z, and the lit material
 * weights wetness by `getWorldNormal().z`.
 *
 * When the corresponding `.mat` shader block changes, change it here too (and
 * vice versa) — the property tests assert the two stay in lockstep.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_WEATHER_SHADER_REF_H_
#define HEAVENBENCH_SRC_ENGINE_WEATHER_SHADER_REF_H_

#include <glm/glm.hpp>

#include <cmath>

namespace heaven::weather_ref {

// ── Shared constants (mirror the material uniforms / literals) ──────

// Camera-centered precipitation volume half-extent in world units. The active
// volume is the axis-aligned cube [-kVolumeExtent, +kVolumeExtent]^3 around the
// camera, i.e. a 140-unit cube.

// This must be large enough that the cube's floor (cameraPos.z - kVolumeExtent)
// reaches the visible near-field ground beneath the benchmark camera, which
// flies tens of metres above the floating-island terrain. At 20 the 40-unit
// cube sat entirely above the ground (rain looked suspended and cut off) and
// read as a tiny localized blob in a world with a ~1000m view distance; 70
// envelops both the camera and the ground so streaks visibly reach the
// buildings/terrain and fill the frame. The streak count (kMaxStreaks) is
// raised in lockstep so the larger volume still reads as steady rain rather
// than sparse dashes.

// NOTE: this value is mirrored in two other places that MUST stay in sync —
// the `const float kVolumeExtent` literal in benchmark/materials/heaven_splash.mat
// and the uniform RainFeature pushes from this constant (rain_feature.cpp reads
// weather_ref::kVolumeExtent). Only the *value* may change; the symmetric
// `volumeExtent - fallen` streak expression in heaven_rain.mat must stay
// byte-for-byte identical to streak_world_position below, or the property tests
// break.
inline constexpr float kVolumeExtent = 70.0f;

// Streak length range in world units. LOOK PASS (streak realism): the
// old [0.2, 2.0] range made the longest drops read as long white lines rather
// than rain. Shortened to [0.12, 0.7] so a streak is a short dash; combined with
// the per-drop length jitter (streak_length uses hash1(seed.x)) drops differ in
// length. Appearance only — no property pins these numeric values (only that
// streak_length stays within [min,max], which still holds).
inline constexpr float kStreakLenMin = 0.12f;
inline constexpr float kStreakLenMax = 0.7f;

// Splash impact filter: a surface receives splashes only when its normal is
// within this cone half-angle of world-up.
inline constexpr float kSplashConeDegrees = 30.0f;

// Hard ceiling on a splash ripple period: every impact effect must complete and
// be removed within 2.0 seconds of spawn.
inline constexpr float kSplashMaxPeriod = 2.0f;

// ── GLSL-portable helpers ───────────────────────────────────────────

// These are defined here, immediately under the constants and ABOVE the unified
// raindrop lattice / fall-law block, because the fall-law functions (drop_z /
// drop_impact_time) call glsl_mod and the streak helpers call hash1/hash3. In
// C++ a free function must be declared before its first use, so the GLSL-portable
// primitives have to precede every function that references them.

// Exact GLSL `mod(x, y)` semantics: `x - y * floor(x / y)`. For y > 0 the result
// lies in [0, y). This is the wrap-around primitive used by both the streak fall
// and the splash lifetime, so it is reproduced here byte-for-byte rather than
// relying on std::fmod (which keeps the sign of x).
[[nodiscard]] inline float glsl_mod(float x, float y) {
    return x - y * std::floor(x / y);
}

// Scalar hash to [0, 1). Mirrors the classic `fract(sin(n) * C)` hash the rain
// material uses to derive deterministic per-streak randoms. Using fract keeps
// every derived offset bounded, which is what guarantees the streak stays inside
// the camera-centered volume for any seed.
[[nodiscard]] inline float hash1(float n) {
    return glm::fract(std::sin(n) * 43758.5453123f);
}

// Vec3 hash to [0, 1)^3. Mirrors the rain material's `hash3(seed)` used to place
// a streak's base cell inside the volume.
[[nodiscard]] inline glm::vec3 hash3(const glm::vec3& seed) {
    return {
        hash1(glm::dot(seed, glm::vec3(127.1f, 311.7f, 74.7f))),
        hash1(glm::dot(seed, glm::vec3(269.5f, 183.3f, 246.1f))),
        hash1(glm::dot(seed, glm::vec3(113.5f, 271.9f, 124.6f)))};
}

// ── Unified raindrop lattice + fall law (shared by streaks AND splashes) ──

// Both the streak shader (heaven_rain.mat) and the splash shader
// (heaven_splash.mat) derive a falling drop from the SAME world-anchored XY
// lattice and the SAME fall law, so a streak column and the splash cell it lands
// in agree byte-for-byte. This is the single source of truth for that model;
// the property test tests/prop_rain_splash_match.cpp pins the streak↔splash
// correspondence offline against these functions.

// LATTICE: a world-anchored XY grid of spacing kDropSpacing. A cell id is
// floor(absoluteWorldXY / spacing). Per cell we derive a jittered column world
// XY, a fall period, a fall phase, and a ripple duration — all pure functions of
// the integer cell id, so they are identical wherever the same cell is referenced
// (a streak placed in it, or a ground pixel whose cell it is).

// FALL LAW: a drop falls from dropTopZ (the top of the camera-centered volume,
// cameraZ + volumeExtent) downward at a per-cell constant speed and recycles with
// period T_fall. We FIX the fall span to exactly 2*volumeExtent (so every drop
// stays inside the camera-centered cube — preserving  /) and vary
// the per-cell speed instead: vSpeed = 2*volumeExtent / T_fall. The generic
// drop_z / drop_impact_time below take the (per-cell) fallSpeed as a parameter, so
// the closed-form impact time the requirement asks for is exact for any ground
// height within the volume.

// World spacing (metres) of the shared drop lattice. MUST equal the
// `kRippleSpacing` constant in heaven_splash.mat so a streak column and a splash
// cell tile on the SAME absolute world XY grid.
inline constexpr float kDropSpacing = 1.2f;

// Per-cell fall-period variation band. The base period (the time to fall the
// full 2*volumeExtent at the nominal fallSpeed) is multiplied by a per-cell value
// in [kFallPeriodVaryMin, kFallPeriodVaryMax], so drops in different cells fall at
// different speeds (the span stays 2*volumeExtent; only the rate varies).
inline constexpr float kFallPeriodVaryMin = 0.8f;
inline constexpr float kFallPeriodVaryMax = 1.4f;

// Per-cell ripple-animation duration band (seconds). The splash ring animation
// runs for this long after the drop's impact; the upper end is HARD-CLAMPED to
// kSplashMaxPeriod (2.0 s) so every impact effect completes within 2.0 s.
// LOCKSTEP: these mirror heaven_splash.mat's kRippleDurMin / kRippleDurMax
// byte-for-byte. kRippleDurationMax shortened a further 30% (1.385 -> 1.065,
// i.e. 1.385 / 1.3) to raise the minimum ripple expansion speed by another 30%
// per user request; still > kRippleDurationMin (0.9) so the mix() band stays
// valid, and still <= the 2.0 s cap, so the clamp is unaffected.
inline constexpr float kRippleDurationMin = 0.9f;
inline constexpr float kRippleDurationMax = 1.065f; // clamped to kSplashMaxPeriod

// ── 2D hashes (mirror heaven_splash.mat hash21 / hash22 byte-for-byte) ──

// These are the Dave-Hoskins style hashes the splash shader uses to derive the
// per-cell jitter / phase. They MUST match the GLSL in heaven_splash.mat exactly
// so the CPU reference and the GPU agree on every cell's column XY and phase.
[[nodiscard]] inline float hash21(const glm::vec2& p) {
    glm::vec3 p3 = glm::fract(glm::vec3(p.x, p.y, p.x) * 0.1031f);
    p3 += glm::dot(p3, glm::vec3(p3.y, p3.z, p3.x) + 33.33f);
    return glm::fract((p3.x + p3.y) * p3.z);
}

[[nodiscard]] inline glm::vec2 hash22(const glm::vec2& p) {
    glm::vec3 p3 = glm::fract(glm::vec3(p.x, p.y, p.x)
                              * glm::vec3(0.1031f, 0.1030f, 0.0973f));
    p3 += glm::dot(p3, glm::vec3(p3.y, p3.z, p3.x) + 33.33f);
    return glm::fract((glm::vec2(p3.x, p3.x) + glm::vec2(p3.y, p3.z))
                      * glm::vec2(p3.z, p3.y));
}

// Integer cell id of an absolute world XY position on the drop lattice.
[[nodiscard]] inline glm::vec2 drop_cell(const glm::vec2& worldXY, float spacing) {
    return glm::floor(worldXY / spacing);
}

// The jittered column world XY (the drop's horizontal position) for a cell.
// Mirrors heaven_splash.mat's `centreWorldXY = (cid + jitter) * kRippleSpacing`
// with `jitter = hash22(cid * 1.37 + 3.1)`.
[[nodiscard]] inline glm::vec2 drop_column_xy(const glm::vec2& cell, float spacing) {
    const glm::vec2 jitter = hash22(cell * 1.37f + 3.1f);
    return (cell + jitter) * spacing;
}

// Per-cell fall PERIOD T_fall (seconds): the time a drop takes to fall the full
// 2*volumeExtent and recycle. The base period is (2*volumeExtent)/fallSpeed (the
// time to traverse the volume at the nominal speed); a per-cell hash varies it
// within [kFallPeriodVaryMin, kFallPeriodVaryMax].
[[nodiscard]] inline float drop_fall_period(const glm::vec2& cell,
                                            float volumeExtent, float fallSpeed) {
    const float base = (2.0f * volumeExtent) / fallSpeed;
    const float h    = hash21(cell + 29.7f);
    return base * glm::mix(kFallPeriodVaryMin, kFallPeriodVaryMax, h);
}

// Per-cell constant fall SPEED (world units / second). Chosen so the fall span
// fallSpeed*T_fall equals exactly 2*volumeExtent — the drop traverses the camera
// volume top-to-bottom and recycles, so every streak stays inside the cube.
[[nodiscard]] inline float drop_fall_speed(const glm::vec2& cell,
                                           float volumeExtent, float fallSpeed) {
    const float T = drop_fall_period(cell, volumeExtent, fallSpeed);
    return (2.0f * volumeExtent) / T;
}

// Per-cell fall PHASE (seconds): a deterministic offset so different cells'
// drops are at different points of their fall at any instant.
[[nodiscard]] inline float drop_phase(const glm::vec2& cell, float fallPeriod) {
    return hash21(cell + 5.3f) * fallPeriod;
}

// Top of the camera-centered precipitation volume (absolute world Z): a drop
// begins its fall here. dropTopZ = cameraZ + volumeExtent.
[[nodiscard]] inline float drop_top_z(float cameraZ, float volumeExtent) {
    return cameraZ + volumeExtent;
}

// Per-cell ripple-animation duration (seconds), hard-clamped to kSplashMaxPeriod
// so the active ripple window is at most 2.0 s wide. The clamp is what the
// new property test proves holds for every per-cell choice.
[[nodiscard]] inline float drop_ripple_duration(const glm::vec2& cell) {
    const float h = hash21(cell + 91.7f);
    return glm::min(glm::mix(kRippleDurationMin, kRippleDurationMax, h),
                    kSplashMaxPeriod);
}

// ── The fall law (the closed forms the requirement asks for) ─────────

// A drop in a cell with phase `cellPhase` falls from `dropTopZ` at constant
// `fallSpeed`, recycling with period `T_fall`:

//     z_drop(t) = dropTopZ - fallSpeed * mod(t - cellPhase, T_fall)

// so the drop sweeps the band (dropTopZ - fallSpeed*T_fall, dropTopZ] each cycle.
[[nodiscard]] inline float drop_z(float cellPhase, float dropTopZ, float fallSpeed,
                                  float fallPeriod, float time) {
    return dropTopZ - fallSpeed * glsl_mod(time - cellPhase, fallPeriod);
}

// Most-recent IMPACT TIME: the largest t <= `time` at which z_drop crossed the
// ground height `groundZ`. The drop reaches groundZ when
// mod(t - cellPhase, T_fall) == tau, with tau = (dropTopZ - groundZ)/fallSpeed,
// i.e. at impactT = cellPhase + tau + k*T_fall. Pick the largest k with
// impactT <= time:

//     impactT = cellPhase + tau + floor((time - cellPhase - tau) / T_fall) * T_fall

// For groundZ in (dropTopZ - fallSpeed*T_fall, dropTopZ] (the ground lies within
// the drop's fall band, i.e. within the volume) tau ∈ [0, T_fall) and therefore
// z_drop(impactT) == groundZ EXACTLY (the splash spawns precisely when the drop
// reaches that ground height). The result satisfies impactT <= time and
// time - impactT ∈ [0, T_fall) (the most recent impact, no skipped cycle, no
// future spawn). Everything is a pure function of the absolute `time`, so the
// impact is frame-rate independent.
[[nodiscard]] inline float drop_impact_time(float cellPhase, float dropTopZ,
                                            float fallSpeed, float fallPeriod,
                                            float groundZ, float time) {
    const float tau  = (dropTopZ - groundZ) / fallSpeed;
    const float base = cellPhase + tau;
    const float k    = std::floor((time - base) / fallPeriod);
    return base + k * fallPeriod;
}

// ── Shared WIND (drift that grows with how far the drop has fallen) ──

// Wind is a SINGLE shared model used identically by the streak shader and the
// splash shader so a slanted drop still splashes exactly where it lands. It is a
// horizontal drift in the constant direction kWindDir whose magnitude grows
// linearly with how far the drop has fallen below the top of the volume:

//     fallenDist(t)   = dropTopZ - drop_z(...)                     / >= 0
//     windOffsetXY(t) = kWindDir * windStrength * fallenDist(t)    / metres
//     dropXY(t)       = drop_column_xy(cell) + windOffsetXY(t)

// windStrength is dimensionless (≈ tan of the slant angle): a drop that has
// fallen `d` metres has drifted kWindDir * windStrength * d metres sideways, so
// the path makes an angle atan(windStrength) with vertical. The gust
// wind_strength(time) is a slow, purely-time-driven sine so it stays frame-rate
// independent and world-stable, and never reverses (base > amp) so the rain
// always slants the same believable way. At windStrength → 0 windOffsetXY is
// exactly zero and every function below reduces to the original vertical fall,
// so the no-wind volume / match behaviour is unchanged.

// Unit horizontal wind direction (xy). A believable gust blowing mostly along
// +X with a little +Y. Mirrors the literal normalize(vec2(1.0, 0.4)) in both
// heaven_rain.mat and heaven_splash.mat.
inline constexpr glm::vec2 kWindDir{0.9284766909f, 0.3713906764f};

// Gust shape: windStrength(time) = base + amp * sin(time * freq). base > amp so
// the strength stays strictly positive (the wind never reverses). The slant
// angle of a drop's path is atan(windStrength) (drift = windStrength * distance
// fallen, vertical = distance fallen), so base = 0.18 gives a clearly-slanted
// ~10.2° from vertical. The gust AMP is kept deliberately SMALL (0.005): the
// base offset is identical for every cell at a given ground height (removed
// exactly by the splash's wind-shifted cell lookup), whereas the time-varying
// amp is what scatters per-cell ripple centres (different cells impact at
// different times), so a small amp keeps that scatter inside the splash's 5x5
// ripple scan while still making the fall genuinely time-varying (the slant
// breathes between ~9.9° and ~10.5°). freq = 0.3 gives a ~21 s gust cycle.
inline constexpr float kWindStrengthBase = 0.18f;
inline constexpr float kWindStrengthAmp  = 0.005f;
inline constexpr float kWindGustFreq     = 0.3f;

// The largest strength the gust can reach (base + amp). The tests use this to
// bound the maximum horizontal drift (maxWindStrength * fall span).
inline constexpr float kWindStrengthMax = kWindStrengthBase + kWindStrengthAmp;

// Time-varying gust strength. Pure function of the absolute `time`, so it is
// frame-rate independent (the streak XY now drifts with time, but reaching the
// same wall-clock time any way yields the same drift up to float accumulation).
[[nodiscard]] inline float wind_strength(float time) {
    return kWindStrengthBase + kWindStrengthAmp * std::sin(time * kWindGustFreq);
}

// Horizontal wind drift (metres) for a drop currently at height `z`, having
// started its fall at `dropTopZ`, for a given (already-evaluated) windStrength.
// The drift is proportional to the distance fallen below the top, so a drop at
// the top has zero drift and a drop at the ground has the full drift. Taking the
// strength as a parameter keeps this a pure geometric function the tests can
// sweep over any wind (including 0, which reduces to no drift).
[[nodiscard]] inline glm::vec2 drop_wind_offset(float dropTopZ, float z,
                                                float windStrength) {
    const float fallenDist = dropTopZ - z;   // >= 0 within the fall band
    return kWindDir * windStrength * fallenDist;
}

// A drop's horizontal world XY at height `z`: its column XY plus the wind drift
// accumulated over the distance it has fallen. This is the shared formula the
// STREAK uses to place its slanted path and the SPLASH uses (at z = groundZ) to
// place its ripple centre — so the splash blooms under the landed drop, not its
// spawn column.
[[nodiscard]] inline glm::vec2 drop_xy_at_z(const glm::vec2& cell, float dropTopZ,
                                            float z, float windStrength) {
    return drop_column_xy(cell, kDropSpacing)
         + drop_wind_offset(dropTopZ, z, windStrength);
}

// ── Rain streaks (heaven_rain.mat vertex shader) ────────────────────

// UNIFICATION: a streak is no longer an arbitrary hash3 cell with its own fall
// term — it is now a DROP ON THE SHARED LATTICE. Each streak slot is mapped to a
// world-anchored lattice cell in a window around the camera, and its world XY is
// that cell's jittered column (drop_column_xy), its Z is the shared fall law
// (drop_z). The splash shader derives the SAME cell from a ground pixel and the
// SAME fall law, so a streak column and the splash cell it lands in agree
// byte-for-byte (the offline match test prop_rain_splash_match pins this).

// Base period (seconds) of the fall for a given fall speed: the time to fall the
// full `2 * volumeExtent` at the nominal speed. The actual per-cell period varies
// around this (drop_fall_period); this nominal value is kept for reference / the
// uniform RainFeature pushes.
[[nodiscard]] inline float streak_fall_period(float volumeExtent, float fallSpeed) {
    return (2.0f * volumeExtent) / fallSpeed;
}

// The world lattice cell a streak slot occupies at the given camera position.

// Reference for the `heaven_rain.mat` vertex-shader cell selection:

//     vec3  h      = hash3(seed);
//     float seedX  = (h.x * 2.0 - 1.0) * volumeExtent; [-E, E)
//     float seedY  = (h.y * 2.0 - 1.0) * volumeExtent; [-E, E)
//     float span   = 2.0 * volumeExtent;
//     world-anchored: nearest tiled instance of this seed to the camera
//     float relX   = mod(seedX - cameraPos.x + volumeExtent, span) - volumeExtent;
//     float relY   = mod(seedY - cameraPos.y + volumeExtent, span) - volumeExtent;
//     float absX   = cameraPos.x + relX; world-static near the camera
//     float absY   = cameraPos.y + relY;
//     vec2  cell   = floor(vec2(absX, absY) / kDropSpacing);

// WORLD-ANCHORING: relX/relY are the offset to the tiled instance of this seed
// nearest the camera, so as the camera moves by Δ the offset changes by −Δ and
// the absolute position absXY (hence the lattice cell) holds still in world
// space — the camera moves THROUGH the rain — until the seed drifts a full
// window (span) away, when the mod wraps it to the opposite side (keeping the
// active set centered on the camera, density constant —). Snapping absXY to
// the kDropSpacing lattice yields the shared cell the splash also derives.
[[nodiscard]] inline glm::vec2 streak_seed_cell(const glm::vec3& seed,
                                                const glm::vec3& cameraPos,
                                                float volumeExtent) {
    const float span = 2.0f * volumeExtent;
    const glm::vec3 h = hash3(seed);
    const float seedX = (h.x * 2.0f - 1.0f) * volumeExtent;
    const float seedY = (h.y * 2.0f - 1.0f) * volumeExtent;
    const float relX = glsl_mod(seedX - cameraPos.x + volumeExtent, span) - volumeExtent;
    const float relY = glsl_mod(seedY - cameraPos.y + volumeExtent, span) - volumeExtent;
    const float absX = cameraPos.x + relX;
    const float absY = cameraPos.y + relY;
    return drop_cell(glm::vec2(absX, absY), kDropSpacing);
}

// The per-cell fall PERIOD a streak slot recycles with (varies per cell). The
// streak's Z (drop_z) is periodic in time with exactly this period.
[[nodiscard]] inline float streak_seed_fall_period(const glm::vec3& seed,
                                                   const glm::vec3& cameraPos,
                                                   float volumeExtent,
                                                   float fallSpeed) {
    const glm::vec2 cell = streak_seed_cell(seed, cameraPos, volumeExtent);
    return drop_fall_period(cell, volumeExtent, fallSpeed);
}

// World-space position of precipitation streak `seed` at `time`, driven by the
// SHARED lattice + fall law.

// Reference for the `heaven_rain.mat` vertex-shader block:

//     vec2  cell    = streak_seed_cell(seed, cameraPos, volumeExtent); above
//     vec2  col     = drop_column_xy(cell, kDropSpacing); jittered column
//     float period  = drop_fall_period(cell, volumeExtent, fallSpeed);
//     float vSpeed  = drop_fall_speed (cell, volumeExtent, fallSpeed); = 2E/period
//     float phase   = drop_phase(cell, period);
//     float dropTop = cameraPos.z + volumeExtent; drop_top_z
//     float z       = drop_z(phase, dropTop, vSpeed, period, time);
//     vec3  world   = vec3(col.x, col.y, z);

// Properties this encodes:
//   - BOUNDED, CAMERA-CENTERED. The fall span is FIXED at exactly
//     2*volumeExtent (vSpeed*period == 2E), so z = drop_z sweeps
//     (dropTop - 2E, dropTop] = (cameraZ - E, cameraZ + E] — strictly inside the
//     camera-centered cube. The XY column lies in the cell snapped from absXY
//     (within [-E, E) of the camera), so its offset from the camera is bounded by
//     E + kDropSpacing (within one lattice cell of the wrapped band).
//   - PERIODIC WRAP keeps density constant: z is periodic in time with the
//     cell's drop_fall_period, and the lattice cell wraps to the opposite side of
//     the window when the seed drifts a full span away — constant active count.
//   - FRAME-RATE INDEPENDENT: the cell + column carry NO time dependence
//     (pure functions of seed + cameraPos), and the wind drift is a pure function
//     of the absolute `time` (wind_strength) and the height z (itself a pure
//     function of time), so reaching the same wall-clock time any way yields the
//     same XY up to float accumulation; z is a pure function of the absolute
//     `time` via drop_z.
//   - WIND (slanted fall): the horizontal XY is drop_xy_at_z = column +
//     kWindDir * wind_strength(time) * (dropTopZ - z), i.e. a drift that grows
//     with the distance fallen. At wind_strength → 0 the XY is exactly the column
//     (no-wind behaviour unchanged). The SAME drop_xy_at_z at z = groundZ is the
//     splash ripple centre, so the streak and splash coincide under wind.

// NOTE (shader vs this reference): heaven_rain.mat mirrors the cell + column +
// fall math here byte-for-byte against the ABSOLUTE camera uniform
// `materialParams.cameraPos`, but does its FINAL additive placement with the
// SHADING-space camera (frameUniforms.worldFromViewMatrix[3].xyz) because
// Filament shades in camera-relative world space. The shader therefore subtracts
// camAbs and re-adds camShading; this header has a single `cameraPos` so it works
// in absolute world space directly. The cell/column/fall relationship the tests
// check is identical in both.
[[nodiscard]] inline glm::vec3 streak_world_position(const glm::vec3& seed,
                                                     const glm::vec3& cameraPos,
                                                     float volumeExtent,
                                                     float time,
                                                     float fallSpeed) {
    const glm::vec2 cell      = streak_seed_cell(seed, cameraPos, volumeExtent);
    const float fallPeriod    = drop_fall_period(cell, volumeExtent, fallSpeed);
    const float fallSpeedCell = drop_fall_speed(cell, volumeExtent, fallSpeed);
    const float cellPhase     = drop_phase(cell, fallPeriod);
    const float dropTopZ      = drop_top_z(cameraPos.z, volumeExtent);
    const float z             = drop_z(cellPhase, dropTopZ, fallSpeedCell, fallPeriod, time);
    // Slanted fall: the drop's horizontal XY is its column plus the wind drift
    // accumulated over the distance it has fallen (drop_xy_at_z). At
    // wind_strength → 0 this is exactly the column, so the no-wind behaviour is
    // unchanged. The same drop_xy_at_z(..., groundZ,...) is what the splash
    // uses for its ripple centre, so the streak and splash coincide under wind.
    const glm::vec2 xy        = drop_xy_at_z(cell, dropTopZ, z, wind_strength(time));
    return {xy.x, xy.y, z};
}

// Length (world units) of streak `seed`, in [kStreakLenMin, kStreakLenMax].

// Reference for `len = mix(streakLenMin, streakLenMax, hash1(seed.x))` in the
// rain vertex shader. Because hash1 returns [0, 1), the result is always
// within [0.2, 2.0] world units.
[[nodiscard]] inline float streak_length(const glm::vec3& seed) {
    return glm::mix(kStreakLenMin, kStreakLenMax, hash1(seed.x));
}

// ── Surface wetness (heaven_lit*.mat material() wetness block) ──────

// Per-surface wetness contribution for a given world-space normal, weighted by
// vertical alignment toward world-up.

// Reference for the lit material's wetness weighting:

//     float up  = clamp(getWorldNormal().z, 0.0, 1.0); Z-up
//     float wet = wetAmount * up;

// Upward-facing surfaces (n.z -> 1) pool water and receive the full
// contribution; downward-facing surfaces (n.z <= 0) receive zero. The
// normal's Z is read directly (the shader's `getWorldNormal()` is already unit
// length), so wetness is monotonic non-decreasing in `worldNormal.z`.
[[nodiscard]] inline float wetness_for_normal(float wetAmount, const glm::vec3& worldNormal) {
    const float up = glm::clamp(worldNormal.z, 0.0f, 1.0f);
    return wetAmount * up;
}

// ── Rain impact splashes (heaven_splash.mat) ────────────────────────

// Whether a surface with the given normal accepts rain-impact splashes.

// Reference for the splash placement test: a surface is hit only when its normal
// is within `kSplashConeDegrees` (30°) of world-up, i.e.

//     dot(normalize(N), vec3(0,0,1)) >= cos(radians(30))

// A degenerate (near-zero-length) normal cannot face up and is rejected.
[[nodiscard]] inline bool splash_accepted(const glm::vec3& normal) {
    const float len = glm::length(normal);
    if (len <= 1e-8f) {
        return false;
    }
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const float cosCone = std::cos(glm::radians(kSplashConeDegrees));
    return glm::dot(normal / len, up) >= cosCone;
}

// Animation age (seconds since the current ripple began) for a splash with the
// given spawn phase at `time`.

// Reference for the splash fragment shader's `age = mod(time - spawnPhase,
// period)`. `period` is clamped to `kSplashMaxPeriod` (2.0s) so the active
// animation window is at most 2.0s wide and the returned age is always in
// [0, period) ⊆ [0, 2.0) — every impact effect completes and is removed within
// 2.0 seconds of spawn. A non-positive period yields age 0.
[[nodiscard]] inline float splash_age(float time, float spawnPhase, float period) {
    const float p = glm::min(period, kSplashMaxPeriod);
    if (p <= 0.0f) {
        return 0.0f;
    }
    return glsl_mod(time - spawnPhase, p);
}

} // namespace heaven::weather_ref

#endif // HEAVENBENCH_SRC_ENGINE_WEATHER_SHADER_REF_H_
