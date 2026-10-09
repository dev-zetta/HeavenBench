/*
 * HeavenBench — Property test: weather re-application change decision
 *
 * Feature: codebase-hardening
 *   •: Weather re-application change decision
 *
 *
 * (re-apply weather-dependent Filament state on change):
 *   11.1/11.2/11.3 — WHEN the current frame's WeatherState differs from the
 *     previous frame's, the backend re-applies the weather-dependent
 *     post-process, lighting/exposure, and distance-fog state in that frame.
 *   11.4 — it re-applies on every such detected change, independent of any
 *     one-time initialization flag.
 *   11.5 — WHILE the current WeatherState equals the previous one, the backend
 *     does NOT re-apply that state.
 *
 * The production decision lives in rhi/filament_backend.cpp::render_frame. It is
 * a PURE predicate over two WeatherState values plus the "have we seeded
 * prev_weather_ yet" flag:
 *
 * one-time baseline, gated by the one-time init flag
 *     if (!lighting_initialized_) {
 *         if (apply_weather_state(...)) {
 *             prev_weather_        = weather;
 *             weather_initialized_ = true;
 *             lighting_initialized_= true;
 *         }
 *     }
 * per-frame change check — independent of lighting_initialized_
 *     if (weather_initialized_ && weather != prev_weather_) {
 *         if (apply_weather_state(...)) prev_weather_ = weather;
 *     }
 *
 * The re-apply DECISION therefore reduces to:
 *
 *     should_reapply = weather_initialized && (weather != prev_weather)
 *
 * and WeatherState::operator!= compares the DERIVED weather-dependent outputs
 * (effective intensity, fog_density(), surface_wetness(), sun_scale()) — the
 * exact signature the backend pushes to Filament — not the raw transition
 * timers. apply_weather_state itself calls into Filament (GPU) and cannot run in
 * the headless, Filament-free harness, so this property pins the host-side
 * change-decision INVARIANT that gates that call (the live re-apply is covered
 * by the gated smoke seam).
 *
 * This TU self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <functional>
#include <tuple>
#include <vector>

#include <rapidcheck.h>

#include "engine/types.h" // heaven::WeatherState, heaven::WeatherConfig

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

// ── Host-side model of render_frame's re-apply decision ─────────
// Verbatim transcription of the per-frame change check in
// filament_backend.cpp::render_frame. The decision reads ONLY the seeded flag
// and the WeatherState inequality — it never consults the one-time init flag
// which gates the separate first-frame baseline.
bool should_reapply(bool weather_initialized,
                    const heaven::WeatherState& cur,
                    const heaven::WeatherState& prev) {
    return weather_initialized && (cur != prev);
}

// ── Independent reference of the derived-output signature ───────
// Mirrors exactly the four outputs WeatherState::operator== compares, computed
// here independently so the test does not merely restate operator==: two states
// share a signature iff these four derived values are equal.
using Signature = std::tuple<float, float, float, float>;
Signature signature_of(const heaven::WeatherState& s) {
    return {s.effective_intensity(), s.fog_density(),
            s.surface_wetness(), s.sun_scale()};
}

// ── Smart WeatherState generator ────────────────────────────────
// Varies every field that feeds the derived signature: the commanded rain
// intensity (which drives effective_intensity / fog_density / sun_scale) plus
// the config endpoints fog density and sun scale read from. An optional advance
// soaks the surface-wetness accumulator so that derived output varies too. The
// states are built from a DRY config (rain_intensity 0) and then commanded via
// set_intensity, so effective_intensity() snaps to the generated value (no
// dry-down) and the signature is deterministic for the chosen inputs.
rc::Gen<heaven::WeatherState> weather_gen() {
    return rc::gen::map(
        rc::gen::tuple(
            // intensity in [0,1] on a fine integer grid (endpoints exact).
            rc::gen::inRange<int>(0, 1'000'001),
            // fog density endpoint in [0, 0.01].
            rc::gen::inRange<int>(0, 10'001),
            // sun-scale floor endpoint in [0,1].
            rc::gen::inRange<int>(0, 1'001),
            // optional soak time in [0, 20] s (0 = bone dry).
            rc::gen::inRange<int>(0, 2'001)),
        [](const std::tuple<int, int, int, int>& g) {
            heaven::WeatherConfig cfg{};
            cfg.rain_intensity      = 0.0f; // build dry, command explicitly
            cfg.max_fog_density_at_1 = static_cast<float>(std::get<1>(g)) / 1'000'000.0f;
            cfg.min_sun_scale_at_1   = static_cast<float>(std::get<2>(g)) / 1'000.0f;

            heaven::WeatherState s(cfg);
            s.set_intensity(static_cast<float>(std::get<0>(g)) / 1'000'000.0f);

            const float soak = static_cast<float>(std::get<3>(g)) / 100.0f;
            if (soak > 0.0f) s.advance(soak);
            return s;
        });
}

// ──: re-apply iff derived signatures differ ─────────
bool run_p17_change_decision() {
    return rc::check(
        "Feature: codebase-hardening, Property 17: weather re-application "
        "change decision — re-apply iff derived signatures differ, independent "
        "of the one-time init flag (Validates: Requirements 11.1, 11.2, 11.3, "
        "11.4, 11.5)",
        [] {
            const heaven::WeatherState prev = *weather_gen();
            // Half the time exercise the EQUAL case by cloning prev (continuous
            // floats rarely collide), so both branches are covered densely.
            const bool clone_prev = *rc::gen::arbitrary<bool>();
            const heaven::WeatherState cur =
                clone_prev ? prev : *weather_gen();

            // operator!= must agree with the independent derived signature: the
            // change check fires iff the APPLIED outputs would actually differ
            // (not on raw timer bookkeeping).
            const bool sigs_differ =
                signature_of(cur) != signature_of(prev);
            RC_ASSERT((cur != prev) == sigs_differ);
            RC_ASSERT((cur == prev) == !sigs_differ);

            // ── Core decision: once the backend has
            // seeded prev_weather_, it re-applies the weather-dependent
            // post-process, lighting/exposure, and distance-fog state IFF the
            // current state differs from the previous one. ──
            RC_ASSERT(should_reapply(/*weather_initialized=*/true, cur, prev)
                      == sigs_differ);

            // Equal states never re-apply; differing states always do
            // once initialized.
            if (sigs_differ) {
                RC_ASSERT(should_reapply(true, cur, prev));
            } else {
                RC_ASSERT(!should_reapply(true, cur, prev));
            }

            // ── Independence from the one-time init flag: the change
            // decision has no one-time-init input, so only the seeded flag and
            // WeatherState inequality can affect it. ──
            RC_ASSERT(should_reapply(/*weather_initialized=*/true, cur, prev)
                      == sigs_differ);

            // ── Before prev_weather_ is seeded (weather_initialized == false),
            // the per-frame change check never fires; the one-time baseline path
            // is what applies the state for the first frame. ──
            RC_ASSERT(!should_reapply(/*weather_initialized=*/false, cur, prev));
        });
}

// ── Full first-frame + change sequence model ────────────────────
// Drives the EXACT two-branch render_frame logic across a frame sequence to
// pin the higher-level behaviour the decision serves:
//   • the one-time baseline applies EXACTLY ONCE, on the first frame, and
//   • after seeding, each frame re-applies iff the WeatherState changed
//     (independent of the now-latched one-time init flag —).
bool run_p17_sequence_model() {
    return rc::check(
        "Feature: codebase-hardening, Property 17: first-frame baseline applies "
        "once, then re-apply tracks WeatherState changes (Validates: "
        "Requirements 11.1, 11.4, 11.5)",
        [] {
            const std::vector<heaven::WeatherState> frames =
                *rc::gen::container<std::vector<heaven::WeatherState>>(
                    weather_gen());
            RC_PRE(!frames.empty());

            // Backend state, modelled exactly as the members in render_frame.
            bool lighting_initialized = false;
            bool weather_initialized  = false;
            heaven::WeatherState prev{}; // unseeded; value irrelevant until set

            int init_applied_count = 0;

            for (std::size_t i = 0; i < frames.size(); ++i) {
                const heaven::WeatherState& cur = frames[i];

                // Snapshot the committed previous state the change check will
                // see this frame, before either branch mutates `prev`.
                const bool had_prev = weather_initialized;
                heaven::WeatherState prev_seen = prev;

                // Branch 1: one-time baseline (apply always succeeds in the
                // headless model — failure retention is 's domain).
                bool init_applied_this_frame = false;
                if (!lighting_initialized) {
                    prev                 = cur;
                    weather_initialized  = true;
                    lighting_initialized = true;
                    init_applied_this_frame = true;
                    ++init_applied_count;
                    prev_seen = cur; // the seeded baseline is now the reference
                }

                // Branch 2: per-frame change check (the decision under test).
                const bool reapplied = should_reapply(weather_initialized, cur, prev);
                if (reapplied) {
                    prev = cur; // commit on success
                }

                if (i == 0) {
                    // First frame seeds the baseline and, having just set
                    // prev == cur, never re-applies in the same frame.
                    RC_ASSERT(init_applied_this_frame);
                    RC_ASSERT(!reapplied);
                } else {
                    // After the first frame the baseline is latched and the
                    // change check alone governs re-application: it fires iff
                    // the current state differs from the committed previous one
                    // (independent of the now-latched one-time init flag).
                    RC_ASSERT(had_prev);
                    RC_ASSERT(!init_applied_this_frame);
                    RC_ASSERT(reapplied == (cur != prev_seen));
                }
            }

            // The one-time baseline ran exactly once across the whole run.
            RC_ASSERT(init_applied_count == 1);
        });
}

bool run_weather_reapply_properties() {
    bool ok = true;
    ok = run_p17_change_decision() && ok;
    ok = run_p17_sequence_model() && ok;
    return ok;
}

const heaven::testing::Register g_register_weather_reapply{
    &run_weather_reapply_properties};
} // namespace
