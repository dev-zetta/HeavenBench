// Feature: codebase-hardening,
/*
 * HeavenBench — Property test for the aspect / division NaN guard.
 *
 * Feature: codebase-hardening
 *   •: Aspect / division NaN guard
 *
 *
 * This exercises the REAL exported helper `heaven::safe_aspect(int w, int h)`
 * from rhi/filament_util.h  — a pure inline function defined as
 * `h > 0 ? (float)w / (float)h: 1.0f`. Every aspect consumer (camera,
 * godrays, volfog, ssgi, clouds, hdr_output, depth_prepass) routes through it
 * so pinning the helper's invariants covers  for
 * the whole subsystem set.
 *
 * Properties asserted for any width >= 0 and any height >= 0:
 *   • the result is always finite (never NaN/Inf) —;
 *   • at height == 0 the result is exactly the 1.0f fallback — Requirements
 *     17.1, 17.2, 18.2 (the same guard RainFeature applies);
 *   • at height > 0 the result equals `static_cast<float>(w) /
 *     static_cast<float>(h)` — behavioural parity,.
 *
 * It also models the main-loop skinned-animation `fmod` guard
 * locally — `num_frames > 0 ? fmod(x, num_frames)
 * 0.0f` — and asserts the guarded result is finite for any num_frames >= 0
 * (the zero case skips the divide-by-zero `fmod` and yields 0). The guard is
 * modelled here rather than called because it lives inline in main.cpp (which
 * the headless test target does not link); the real division it protects is
 * `safe_aspect`, exercised directly above.
 *
 * safe_aspect is pure (no I/O, no GPU), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the ">= 100
 * iterations" requirement (raise via RC_PARAMS=max_success=...).
 */
#include <cmath>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "rhi/filament_util.h"

// Mirror the minimal registry surface defined in test_main.cpp. `registry()`
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

// Model of the main-loop skinned-animation phase guard (main.cpp):
// when the animation has frames, advance the phase with fmod; with zero frames
// skip the divide-by-zero fmod entirely and use frame 0. Kept local
// because main.cpp is not part of the headless test link line.
float guarded_anim_phase(float x, int num_frames) {
    return num_frames > 0
               ? std::fmod(x, static_cast<float>(num_frames))
               : 0.0f;
}

bool run_p22_aspect_division_guard_property() {
    return rc::check(
        "Feature: codebase-hardening, Property 22: aspect / division NaN guard "
        "(Validates: Requirements 17.1, 17.2, 17.3, 18.1, 18.2, 18.3)",
        [] {
            // Fuzz width over [0, large] and height over [0, large]. The upper
            // bound is well inside int range so the float division itself is
            // exercised across the whole meaningful viewport space, including
            // the height == 0 boundary.
            const int w = *rc::gen::inRange(0, 100000);
            const int h = *rc::gen::inRange(0, 100000);

            const float aspect = heaven::safe_aspect(w, h);

            // the guard never produces NaN/Inf, for any input.
            RC_ASSERT(std::isfinite(aspect));

            if (h == 0) {
                // defined 1.0f fallback at zero height.
                RC_ASSERT(aspect == 1.0f);
            } else {
                // behavioural parity with the bare float division.
                RC_ASSERT(aspect ==
                          static_cast<float>(w) / static_cast<float>(h));
            }

            // the modelled skinned-animation phase guard is finite
            // for any frame count >= 0; the zero case skips fmod and yields 0.
            const float x =
                static_cast<float>(*rc::gen::inRange(0, 1000000)) * 0.01f;
            const int num_frames = *rc::gen::inRange(0, 4096);
            const float phase = guarded_anim_phase(x, num_frames);
            RC_ASSERT(std::isfinite(phase));
            if (num_frames == 0) {
                RC_ASSERT(phase == 0.0f);
            }
        });
}

const heaven::testing::Register g_register_safe_aspect{
    &run_p22_aspect_division_guard_property};

} // namespace
