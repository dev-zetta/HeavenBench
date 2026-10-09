/*
 * HeavenBench — unit test: advanced-rendering quality CLI parsing
 *
 * — Validates:
 *
 * The renderer's argument parser (main.cpp) resolves the `--<feature>-quality`
 * flag family through the shared, GPU-free helper
 * heaven::apply_quality_args(AppConfig&, argc, argv, emit_warnings) in
 * engine/types.h. main.cpp delegates to this exact function, so exercising it
 * here verifies the same clamp/reject and defaulting behaviour the CLI produces
 * — without linking Filament/SDL/main.cpp into the host test target. This
 * mirrors unit_weather_cli_defaults.cpp.
 *
 * Scope note: apply_quality_args owns ONLY the `--*-quality` value
 * family. The on/off toggle flags (--no-soft-shadows, --no-contact-shadows,
 * --point-light-shadows, --msaa, --refraction, --volumetric-clouds, --ssgi,
 * --massive-foliage) are parsed in main.cpp::parse_args, NOT here, and so are
 * not exercised by this test. We assert the FeatureToggles defaults and that
 * apply_quality_args leaves them untouched (it only writes config.quality).
 *
 * This pins the contract:
 *   • No quality flags at all ⇒ every QualityKnobs field keeps its default and
 *     every FeatureToggle keeps its default (absent-flag defaults).
 *   • --soft-shadow-quality / --contact-shadow-quality / --cloud-quality /
 *     --ssgi-quality L ⇒ QualityLevel set; out-of-range L clamps to {0,1,2}
 *     (clamp-and-report).
 *   • --point-light-quality N ⇒ shadow_light_budget clamped to
 *     [0, shadow_light_budget_max].
 *   • --foliage-quality F ⇒ foliage_density clamped to
 *     [1.0, foliage_density_max].
 *   • --msaa-quality N ⇒ reject-and-retain semantics: values in {1,2,4} are
 *     accepted, anything else is rejected and the previous count is retained
 *     (distinct from the clamping knobs).
 *
 * The test registers itself with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, following
 * the same self-registration pattern as the other test translation units. The
 * registered callable returns true on success and false (after printing a
 * diagnostic) on failure. apply_quality_args is invoked with emit_warnings=false
 * so the suite stays quiet.
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

// Build an AppConfig by running the shared quality-arg parser over a fixed argv
// (argv[0] is a dummy program name, as in a real launch). Warnings are silenced
// so the suite stays quiet; the values are what we assert on.
heaven::AppConfig parse(std::vector<const char*> args) {
    heaven::AppConfig config;
    args.insert(args.begin(), "heaven_benchmark");
    heaven::apply_cli_arguments(config, args);
    return config;
}

bool unit_quality_cli() {
    bool ok = true;

    // The defaults we expect on a fresh AppConfig — these are the single source
    // of truth in QualityKnobs / FeatureToggles, captured once so the
    // absent-flag assertions below compare against the real defaults.
    const heaven::QualityKnobs def_knobs;
    const heaven::FeatureToggles def_toggles;

    // Helper to flag a failure with a message.
    auto fail = [&](const char* msg) {
        std::printf("[unit_quality_cli] FAILED: %s\n", msg);
        ok = false;
    };

    // ── Absent flags leave every quality knob at its default  ──
    {
        const heaven::AppConfig c = parse({});
        const heaven::QualityKnobs& q = c.quality;
        if (q.soft_shadow_level != def_knobs.soft_shadow_level)
            fail("absent --soft-shadow-quality changed soft_shadow_level");
        if (q.contact_shadow_level != def_knobs.contact_shadow_level)
            fail("absent --contact-shadow-quality changed contact_shadow_level");
        if (q.cloud_quality != def_knobs.cloud_quality)
            fail("absent --cloud-quality changed cloud_quality");
        if (q.ssgi_quality != def_knobs.ssgi_quality)
            fail("absent --ssgi-quality changed ssgi_quality");
        if (q.shadow_light_budget != def_knobs.shadow_light_budget)
            fail("absent --point-light-quality changed shadow_light_budget");
        if (q.msaa_samples != def_knobs.msaa_samples)
            fail("absent --msaa-quality changed msaa_samples");
        if (std::fabs(q.foliage_density - def_knobs.foliage_density) > 1e-6f)
            fail("absent --foliage-quality changed foliage_density");

        // apply_quality_args only writes config.quality, so every feature
        // toggle must keep its default (toggles are parsed in parse_args).
        const heaven::FeatureToggles& f = c.features;
        if (f.soft_shadows != def_toggles.soft_shadows ||
            f.contact_shadows != def_toggles.contact_shadows ||
            f.point_light_shadows != def_toggles.point_light_shadows ||
            f.msaa != def_toggles.msaa ||
            f.refraction != def_toggles.refraction ||
            f.volumetric_clouds != def_toggles.volumetric_clouds ||
            f.ssgi != def_toggles.ssgi ||
            f.massive_foliage != def_toggles.massive_foliage ||
            f.ground_relief != def_toggles.ground_relief)
            fail("apply_quality_args altered a FeatureToggle (should only touch config.quality)");
    }

    // ── Ordered-level knobs: in-range accepted, out-of-range clamped ──
    // Each of the four level knobs shares clamp_quality_level: valid {0,1,2}
    // map straight through; <0 clamps to Low(0), >2 clamps to High(2).
    {
        // In-range: 0 → Low, 2 → High for each knob.
        const heaven::AppConfig lo = parse({"--soft-shadow-quality", "0",
                                            "--contact-shadow-quality", "0",
                                            "--cloud-quality", "0",
                                            "--ssgi-quality", "0"});
        if (lo.quality.soft_shadow_level != heaven::QualityLevel::Low)
            fail("--soft-shadow-quality 0 should map to Low");
        if (lo.quality.contact_shadow_level != heaven::QualityLevel::Low)
            fail("--contact-shadow-quality 0 should map to Low");
        if (lo.quality.cloud_quality != heaven::QualityLevel::Low)
            fail("--cloud-quality 0 should map to Low");
        if (lo.quality.ssgi_quality != heaven::QualityLevel::Low)
            fail("--ssgi-quality 0 should map to Low");

        const heaven::AppConfig hi = parse({"--soft-shadow-quality", "2",
                                            "--contact-shadow-quality", "2",
                                            "--cloud-quality", "2",
                                            "--ssgi-quality", "2"});
        if (hi.quality.soft_shadow_level != heaven::QualityLevel::High)
            fail("--soft-shadow-quality 2 should map to High");
        if (hi.quality.contact_shadow_level != heaven::QualityLevel::High)
            fail("--contact-shadow-quality 2 should map to High");
        if (hi.quality.cloud_quality != heaven::QualityLevel::High)
            fail("--cloud-quality 2 should map to High");
        if (hi.quality.ssgi_quality != heaven::QualityLevel::High)
            fail("--ssgi-quality 2 should map to High");
    }
    {
        // Out-of-range clamps to the nearest valid level for every knob.
        const heaven::AppConfig over = parse({"--soft-shadow-quality", "9",
                                              "--contact-shadow-quality", "9",
                                              "--cloud-quality", "9",
                                              "--ssgi-quality", "9"});
        if (over.quality.soft_shadow_level != heaven::QualityLevel::High ||
            over.quality.contact_shadow_level != heaven::QualityLevel::High ||
            over.quality.cloud_quality != heaven::QualityLevel::High ||
            over.quality.ssgi_quality != heaven::QualityLevel::High)
            fail("level > 2 should clamp to High for all level knobs");

        const heaven::AppConfig under = parse({"--soft-shadow-quality", "-5",
                                               "--contact-shadow-quality", "-5",
                                               "--cloud-quality", "-5",
                                               "--ssgi-quality", "-5"});
        if (under.quality.soft_shadow_level != heaven::QualityLevel::Low ||
            under.quality.contact_shadow_level != heaven::QualityLevel::Low ||
            under.quality.cloud_quality != heaven::QualityLevel::Low ||
            under.quality.ssgi_quality != heaven::QualityLevel::Low)
            fail("level < 0 should clamp to Low for all level knobs");
    }
    {
        // Each level knob is isolated: setting one leaves the others at default.
        const heaven::AppConfig c = parse({"--soft-shadow-quality", "0"});
        if (c.quality.soft_shadow_level != heaven::QualityLevel::Low)
            fail("isolated --soft-shadow-quality 0 should set Low");
        if (c.quality.contact_shadow_level != def_knobs.contact_shadow_level ||
            c.quality.cloud_quality != def_knobs.cloud_quality ||
            c.quality.ssgi_quality != def_knobs.ssgi_quality)
            fail("--soft-shadow-quality must not disturb the other level knobs");
    }

    // ── --point-light-quality: int budget clamp to [0, max]  ──
    {
        const heaven::AppConfig in = parse({"--point-light-quality", "4"});
        if (in.quality.shadow_light_budget != 4)
            fail("--point-light-quality 4 should be accepted as-is");

        const heaven::AppConfig hi = parse({"--point-light-quality", "9999"});
        if (hi.quality.shadow_light_budget != def_knobs.shadow_light_budget_max)
            fail("--point-light-quality above max should clamp to shadow_light_budget_max");

        const heaven::AppConfig lo = parse({"--point-light-quality", "-3"});
        if (lo.quality.shadow_light_budget != 0)
            fail("--point-light-quality below 0 should clamp to 0");
    }

    // ── --foliage-quality: float clamp to [1.0, max]  ──────
    {
        const heaven::AppConfig in = parse({"--foliage-quality", "1.5"});
        if (std::fabs(in.quality.foliage_density - 1.5f) > 1e-6f)
            fail("--foliage-quality 1.5 should be accepted as-is");

        // Below the 1.0 floor clamps up to 1.0.
        const heaven::AppConfig lo = parse({"--foliage-quality", "0.2"});
        if (std::fabs(lo.quality.foliage_density - 1.0f) > 1e-6f)
            fail("--foliage-quality below 1.0 should clamp to 1.0");

        // Above the configured max clamps down to foliage_density_max.
        const heaven::AppConfig hi = parse({"--foliage-quality", "1000000"});
        const float expected_hi = std::max(1.0f, def_knobs.foliage_density_max);
        if (std::fabs(hi.quality.foliage_density - expected_hi) > 1e-4f)
            fail("--foliage-quality above max should clamp to foliage_density_max");
    }

    // ── --msaa-quality: reject-and-retain for values outside {1,2,4} ──
    {
        // Accepted values become the new sample count.
        for (int n : {1, 2, 4}) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", n);
            const heaven::AppConfig c = parse({"--msaa-quality", buf});
            if (c.quality.msaa_samples != n) {
                std::printf("[unit_quality_cli] FAILED: --msaa-quality %d should be "
                            "accepted, got %d\n", n, c.quality.msaa_samples);
                ok = false;
            }
        }

        // Out-of-set values are REJECTED (not clamped): the previously
        // configured count (the default) is retained.
        for (const char* bad : {"3", "8", "0", "-1"}) {
            const heaven::AppConfig c = parse({"--msaa-quality", bad});
            if (c.quality.msaa_samples != def_knobs.msaa_samples) {
                std::printf("[unit_quality_cli] FAILED: --msaa-quality '%s' should be "
                            "rejected and retain %d, got %d\n",
                            bad, def_knobs.msaa_samples, c.quality.msaa_samples);
                ok = false;
            }
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_quality_cli{unit_quality_cli};

} // namespace
