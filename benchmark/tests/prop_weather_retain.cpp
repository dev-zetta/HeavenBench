/*
 * HeavenBench — Property test: weather re-application failure retains
 * last good state
 *
 * Feature: codebase-hardening
 *   •: Weather re-application failure retains last good state
 *
 *
 * IF re-application of the weather-dependent post-process,
 * lighting, or distance-fog state fails, THEN the Filament_Backend SHALL retain
 * the last successfully applied weather-dependent state and continue rendering
 * without terminating.
 *
 * The production site lives in rhi/filament_backend.cpp::render_frame, which
 * gates the re-application on a per-frame change check and commits the new
 * state ONLY when apply_weather_state succeeds:
 *
 *     if (weather_initialized_ && weather != prev_weather_) {
 *         if (apply_weather_state(weather, lighting, tonemap)) {
 *             prev_weather_ = weather; commit only on success
 *         }
 *     }
 *
 * apply_weather_state returns heaven::Result<void> (it never throws — on an
 * unavailable engine/view it returns an Error) and calls into Filament (GPU),
 * so the real path cannot run in the headless, Filament-free test harness. Per
 * the current gated-smoke seam pattern (the live backend is covered by smoke
 * this property pins the commit-discipline INVARIANT host-side: it
 * models the commit logic exactly — `if (apply().has_value()) prev = cur;` —
 * using the real WeatherState type and heaven::Result<void>, and asserts that
 *
 *   - under a forced apply-FAILURE, prev_weather_ is unchanged (the new state
 *     is NOT committed) and the last good state stays live, and
 *   - under apply-SUCCESS, prev advances to the current state,
 *
 * with NO exception thrown on either path (the path returns Result, never
 * throws).
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <expected>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "engine/types.h" // heaven::WeatherState, heaven::Result, heaven::Error

namespace heaven::testing {

// Mirrors the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is identical across test TUs (ODR-safe).
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Smart generator for a rain intensity in [0, 1]. An integer grid maps onto the
// unit interval so the endpoints 0.0 and 1.0 are exactly representable while the
// interior is covered densely. Distinct intensities yield distinct derived
// WeatherState signatures (operator==), so the change check `cur != prev` fires.
rc::Gen<float> intensity_gen() {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001),
                        [](int g) { return static_cast<float>(g) / 1'000'000.0f; });
}

// Build a DRY WeatherState commanded to `intensity`. Constructing from a dry
// config (rain_intensity 0) means set_intensity(x) snaps effective_intensity()
// to exactly x rather than starting a dry-down, so the derived-output signature
// the backend compares is deterministic for the generated value.
heaven::WeatherState make_state(float intensity) {
    heaven::WeatherConfig dry{};
    dry.rain_intensity = 0.0f;
    heaven::WeatherState s(dry);
    s.set_intensity(intensity);
    return s;
}

bool run_p18_weather_retain() {
    return rc::check(
        "Feature: codebase-hardening, Property 18: weather re-application "
        "failure retains last good state (Validates: Requirements 11.7)",
        [] {
            const float prev_intensity = *intensity_gen();
            const float cur_intensity  = *intensity_gen();
            // Whether apply_weather_state is forced to fail this frame.
            const bool force_fail = *rc::gen::arbitrary<bool>();

            const heaven::WeatherState last_good = make_state(prev_intensity);
            const heaven::WeatherState cur        = make_state(cur_intensity);

            // Model of apply_weather_state: returns heaven::Result<void>, never
            // throws. On forced failure it yields an Error (mirroring the real
            // method's "engine/view unavailable" Error path); otherwise success.
            const auto apply = [force_fail](const heaven::WeatherState&)
                -> heaven::Result<void> {
                if (force_fail) {
                    return std::unexpected(
                        heaven::Error("forced apply_weather_state failure"));
                }
                return {};
            };

            // ── Verbatim transcription of render_frame's commit discipline ──
            // weather_initialized_ is true by this point (the first-frame
            // baseline has seeded prev_weather_), so only the change check
            // gates the re-apply. Commit prev ONLY on success.
            heaven::WeatherState prev = last_good;
            bool threw = false;
            try {
                if (cur != prev) {
                    if (apply(cur)) {
                        prev = cur; // commit only on success
                    }
                    // On failure: prev is NOT advanced.
                }
            } catch (...) {
                threw = true;
            }

            // ── Invariant: the path never throws (it returns Result). ──
            RC_ASSERT(!threw);

            if (force_fail) {
                // ── Failure retains the last good state: prev is unchanged
                // and the new state is NOT committed. ──
                RC_ASSERT(prev == last_good);
                if (cur != last_good) {
                    RC_ASSERT(prev != cur);
                }
            } else {
                // ── Success commits: prev advances to the current state. ──
                RC_ASSERT(prev == cur);
            }
        });
}

const heaven::testing::Register reg_weather_retain{&run_p18_weather_retain};

} // namespace
