/*
 * HeavenBench — unit test: weather CLI defaults and toggle flags
 *
 * — Validates:
 *
 * The renderer's argument parser (main.cpp) resolves the weather flags through
 * the shared, GPU-free helper heaven::apply_weather_args(AppConfig&, argc,
 * argv) in engine/types.h. main.cpp delegates to this exact function, so
 * exercising it here verifies the same defaults and toggle behaviour the CLI
 * produces — without linking Filament/SDL/main.cpp into the host test target.
 *
 * This pins the contract:
 *   • No --rain-intensity flag  ⇒ Rain_Intensity stays at its 0.5 medium
 *     default, so the default run starts the storm; `--rain-intensity 0`
 *     (or `--no-rain`) restores the clear-weather Shipping_Look.
 *   • No weather toggle flags   ⇒ both features.rain and
 *     features.wet_reflections remain enabled.
 *   • --no-rain                 ⇒ features.rain flips off, leaving
 *     features.wet_reflections enabled.
 *   • --no-wet-reflections      ⇒ features.wet_reflections flips off, leaving
 *     features.rain enabled.
 *   • both flags                ⇒ both toggles off.
 *   • --rain-intensity V (in range) ⇒ accepted; out-of-range clamps; junk
 *     falls back to 0.0 (the supplementary  surface that
 *     parse_rain_intensity owns and apply_weather_args routes through).
 *
 * The test registers itself with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, following
 * the same self-registration pattern as the other test translation units. The
 * registered callable returns true on success and false (after printing a
 * diagnostic) on failure.
 */
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "engine/types.h"
#include "engine/cli_parser.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// is a free function with external linkage defined there; the Register helper's
// definition is identical so it can be reused from this translation unit.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Build an AppConfig by running the shared weather-arg parser over a fixed argv
// (argv[0] is a dummy program name, as in a real launch). Warnings are silenced
// so the suite stays quiet; the values are what we assert on.
heaven::AppConfig parse(std::vector<const char*> args) {
    heaven::AppConfig config;
    args.insert(args.begin(), "heaven_benchmark");
    heaven::apply_cli_arguments(config, args);
    return config;
}

bool unit_weather_cli_defaults() {
    bool ok = true;

    // ── Defaults: no weather flags at all ──────────────────────
    // Absent --rain-intensity keeps the new 0.5 medium default (apply_weather_args
    // only writes rain_intensity when --rain-intensity is present, so a fresh
    // AppConfig keeps the WeatherConfig default); absent toggles leave both
    // features enabled.
    {
        const heaven::AppConfig c = parse({});
        if (std::fabs(c.weather.rain_intensity - 0.5f) > 1e-6f) {
            std::printf("[unit_weather_cli_defaults] FAILED: default rain_intensity "
                        "== %.9g, expected 0.5 (medium default)\n",
                        static_cast<double>(c.weather.rain_intensity));
            ok = false;
        }
        if (!c.features.rain) {
            std::printf("[unit_weather_cli_defaults] FAILED: default features.rain "
                        "== false, expected true (R7.1, R7.3)\n");
            ok = false;
        }
        if (!c.features.wet_reflections) {
            std::printf("[unit_weather_cli_defaults] FAILED: default "
                        "features.wet_reflections == false, expected true "
                        "(R7.1, R7.3, R9.2)\n");
            ok = false;
        }
    }

    // ── --no-rain flips only the rain toggle  ────────────
    {
        const heaven::AppConfig c = parse({"--no-rain"});
        if (c.features.rain) {
            std::printf("[unit_weather_cli_defaults] FAILED: --no-rain left "
                        "features.rain enabled (R7.2)\n");
            ok = false;
        }
        if (!c.features.wet_reflections) {
            std::printf("[unit_weather_cli_defaults] FAILED: --no-rain also "
                        "disabled features.wet_reflections (should be isolated, "
                        "R7.2)\n");
            ok = false;
        }
    }

    // ── --no-wet-reflections flips only the SSR toggle  ──
    {
        const heaven::AppConfig c = parse({"--no-wet-reflections"});
        if (c.features.wet_reflections) {
            std::printf("[unit_weather_cli_defaults] FAILED: --no-wet-reflections "
                        "left features.wet_reflections enabled (R7.2, R9.2)\n");
            ok = false;
        }
        if (!c.features.rain) {
            std::printf("[unit_weather_cli_defaults] FAILED: --no-wet-reflections "
                        "also disabled features.rain (should be isolated, R7.2)\n");
            ok = false;
        }
    }

    // ── Both toggle flags ⇒ both features off  ───────────
    {
        const heaven::AppConfig c = parse({"--no-rain", "--no-wet-reflections"});
        if (c.features.rain || c.features.wet_reflections) {
            std::printf("[unit_weather_cli_defaults] FAILED: both flags should "
                        "disable both toggles (rain=%d, wet_reflections=%d, R7.2)\n",
                        c.features.rain, c.features.wet_reflections);
            ok = false;
        }
    }

    // ── --rain-intensity in range is accepted; toggles untouched ──
    {
        const heaven::AppConfig c = parse({"--rain-intensity", "0.5"});
        if (std::fabs(c.weather.rain_intensity - 0.5f) > 1e-6f) {
            std::printf("[unit_weather_cli_defaults] FAILED: --rain-intensity 0.5 "
                        "⇒ %.9g, expected 0.5\n",
                        static_cast<double>(c.weather.rain_intensity));
            ok = false;
        }
        if (!c.features.rain || !c.features.wet_reflections) {
            std::printf("[unit_weather_cli_defaults] FAILED: --rain-intensity must "
                        "not alter feature toggles\n");
            ok = false;
        }
    }

    // ── Out-of-range --rain-intensity clamps to the nearest bound ──
    {
        const heaven::AppConfig hi = parse({"--rain-intensity", "5"});
        if (hi.weather.rain_intensity != 1.0f) {
            std::printf("[unit_weather_cli_defaults] FAILED: --rain-intensity 5 "
                        "⇒ %.9g, expected clamp to 1.0\n",
                        static_cast<double>(hi.weather.rain_intensity));
            ok = false;
        }
        const heaven::AppConfig lo = parse({"--rain-intensity", "-2"});
        if (lo.weather.rain_intensity != 0.0f) {
            std::printf("[unit_weather_cli_defaults] FAILED: --rain-intensity -2 "
                        "⇒ %.9g, expected clamp to 0.0\n",
                        static_cast<double>(lo.weather.rain_intensity));
            ok = false;
        }
    }

    // ── Junk --rain-intensity falls back to the default ────
    {
        const heaven::AppConfig c = parse({"--rain-intensity", "drizzle"});
        if (c.weather.rain_intensity != 0.0f) {
            std::printf("[unit_weather_cli_defaults] FAILED: junk --rain-intensity "
                        "⇒ %.9g, expected default 0.0\n",
                        static_cast<double>(c.weather.rain_intensity));
            ok = false;
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_weather_cli_defaults{unit_weather_cli_defaults};

} // namespace
