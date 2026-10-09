/*
 * HeavenBench — unit test:  CLI defaults and value parsing
 *
 * — Validates:
 *
 * The renderer's argument handling was refactored into the table-driven parser
 * in engine/cli_parser.{h,cpp}; the public entry point heaven::parse_cli(argc,
 * argv) returns a ParseResult carrying the parsed BenchConfig (`config`) plus a
 * vector of collected diagnostics (`diagnostics`). That module is GPU-free and
 * I/O-free, so this unit test exercises it on the host without linking
 * Filament/SDL — mirroring unit_quality_cli.cpp / unit_weather_cli_defaults.cpp.
 *
 * Where the prop_cli_* property tests cover the toggle/value mechanics across
 * random inputs, this unit test pins the concrete  contract by example:
 *
 *   • A flagless parse leaves the seven recommended  FeatureToggles
 *     bools ON (clear_coat, anisotropic, subsurface, cloth,
 *     transparent_shadows, colored_penumbra, specular_aa) and the three opt-in
 *     bools OFF (color_grading, physical_camera, dynamic_resolution),
 *     tone_mapper == ACES, and the grading / physical-camera configs at their
 *     neutral Shipping_Look defaults.
 *   • Each --no-<feature> disable flag (the seven default-on capabilities)
 *     flips EXACTLY its FeatureToggles field false; each bare --<feature>
 *     enable flag sets its field true (a compat no-op for the seven, the real
 *     enable for the three opt-in ones), disturbing no other.
 *   • The `--<feature>-quality` knob flags (--specular-aa-quality,
 *     --dynamic-resolution-quality) parse an in-range value cleanly and clamp
 *     an out-of-range value WITH a diagnostic, under the documented naming
 *     convention.
 *   • --tone-mapper with a known name sets the enum; an unknown name retains
 *     ACES and emits a diagnostic (value-flag parsing;
 *     unknown-name parity to the ACES default).
 *   • A grading value flag (--grading-contrast) out of range clamps and warns.
 *
 * The file self-registers with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, so the
 * file(GLOB) over the tests directory in benchmark/CMakeLists.txt compiles it in
 * without editing the entry point. The registered callable returns true on
 * success and false (after printing a diagnostic) on failure.
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/cli_parser.h"
#include "engine/types.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// is a free function with external linkage defined there; the Register helper's
// definition is token-identical across test TUs (ODR-safe), so it can be reused
// from this translation unit.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using heaven::ParseResult;

// Run the real parse driver over a token list (argv[0] is a dummy program name,
// as in a real launch). parse_cli is pure: no printing, no exit — the
// diagnostics it would otherwise route to the wrapper are returned in the
// ParseResult, which is exactly what we assert on.
ParseResult parse(std::vector<const char*> args) {
    args.insert(args.begin(), "heaven_benchmark");
    return heaven::parse_cli(args);
}

// True iff some diagnostic line contains `needle` (so the assertions stay
// robust to incidental wording while still pinning the flag that warned).
bool has_diag_containing(const ParseResult& r, std::string_view needle) {
    return std::ranges::any_of(r.diagnostics, [&](const std::string& message) {
        return message.contains(needle);
    });
}

// One  capability toggle: its bare enable flag, its --no- disable flag
// (nullptr for the three opt-in capabilities), the FeatureToggles bool it
// drives, and its default value.
struct ToggleFlag {
    const char*                 enable_flag;   // bare --<feature>
    const char*                 disable_flag;  // --no-<feature> or nullptr
    bool                        default_on;
    bool heaven::FeatureToggles::* field;
};

// The ten  capabilities (tone-mapper is a value flag, excluded). The
// first seven default ON with a --no- disable flag plus a bare --<feature>
// compat enable alias; the last three default OFF as bare --<feature> enables.
const std::vector<ToggleFlag>& toggle_flags() {
    static const std::vector<ToggleFlag> f = {
        {"--clear-coat",          "--no-clear-coat",          true,  &heaven::FeatureToggles::clear_coat},
        {"--anisotropic",         "--no-anisotropic",         true,  &heaven::FeatureToggles::anisotropic},
        {"--subsurface",          "--no-subsurface",          true,  &heaven::FeatureToggles::subsurface},
        {"--cloth",               "--no-cloth",               true,  &heaven::FeatureToggles::cloth},
        {"--transparent-shadows", "--no-transparent-shadows", true,  &heaven::FeatureToggles::transparent_shadows},
        {"--colored-penumbra",    "--no-colored-penumbra",    true,  &heaven::FeatureToggles::colored_penumbra},
        {"--specular-aa",         "--no-specular-aa",         true,  &heaven::FeatureToggles::specular_aa},
        {"--color-grading",       nullptr,                    false, &heaven::FeatureToggles::color_grading},
        {"--physical-camera",     nullptr,                    false, &heaven::FeatureToggles::physical_camera},
        {"--dynamic-resolution",  nullptr,                    false, &heaven::FeatureToggles::dynamic_resolution},
    };
    return f;
}

bool unit_phase8_cli() {
    bool ok = true;
    auto fail = [&](const char* msg) {
        std::printf("[unit_phase8_cli] FAILED: %s\n", msg);
        ok = false;
    };

    // ── 1. Flagless parse: seven default ON, three default OFF  ──
    {
        const ParseResult r = parse({});
        const heaven::FeatureToggles& f = r.config.features;

        for (const ToggleFlag& tf : toggle_flags()) {
            if (r.config.features.*(tf.field) != tf.default_on) {
                std::printf("[unit_phase8_cli] FAILED: default toggle for %s is "
                            "%s, expected %s (Req 13.1/14.1)\n", tf.enable_flag,
                            (r.config.features.*(tf.field)) ? "ON" : "OFF",
                            tf.default_on ? "ON" : "OFF");
                ok = false;
            }
        }
        // Belt-and-suspenders: colored_penumbra now defaults ON (recommended set).
        if (!f.colored_penumbra)
            fail("default features.colored_penumbra is OFF, expected ON (Req 13.1)");
        // The three opt-in capabilities stay OFF.
        if (f.color_grading || f.physical_camera || f.dynamic_resolution)
            fail("an opt-in Phase-8 capability defaults ON, expected OFF (Req 14.1)");

        // Tone-mapper defaults to ACES.
        if (r.config.tone_mapper != heaven::ToneMapperSelection::ACES)
            fail("default tone_mapper is not ACES (Req 14.1)");

        // Grading config at its neutral defaults.
        const heaven::ColorGradingConfig g;
        if (r.config.grading.temperature != g.temperature ||
            r.config.grading.tint != g.tint ||
            r.config.grading.contrast != g.contrast ||
            r.config.grading.saturation != g.saturation ||
            r.config.grading.night_adaptation != g.night_adaptation ||
            r.config.grading.cdl_slope != g.cdl_slope ||
            r.config.grading.cdl_offset != g.cdl_offset ||
            r.config.grading.cdl_power != g.cdl_power)
            fail("flagless grading config is not at neutral defaults (Req 14.1)");

        // Physical-camera config at its neutral Shipping_Look defaults.
        const heaven::PhysicalCameraConfig cam;
        if (r.config.physical_camera.aperture != cam.aperture ||
            r.config.physical_camera.shutter_speed != cam.shutter_speed ||
            r.config.physical_camera.sensitivity != cam.sensitivity)
            fail("flagless physical_camera config is not at neutral defaults (Req 14.1)");

        // Dynamic-resolution knob at its default min scale.
        const heaven::QualityKnobs q;
        if (std::fabs(r.config.quality.dynres_min_scale - q.dynres_min_scale) > 1e-6f)
            fail("flagless dynres_min_scale is not at its default (Req 14.1)");
        if (r.config.quality.specular_aa_level != q.specular_aa_level)
            fail("flagless specular_aa_level is not at its default (Req 14.1)");

        // A flagless parse is clean: no diagnostics, no help.
        if (!r.diagnostics.empty())
            fail("flagless parse emitted diagnostics");
        if (r.help_requested)
            fail("flagless parse requested help");
    }

    // ── 2. Each --no-<feature> flips its field OFF; bare --<feature> sets ON ──
    for (const ToggleFlag& tf : toggle_flags()) {
        // 2a. The disable flag (seven default-on capabilities) flips its field
        //     OFF and leaves every other  toggle at its default.
        if (tf.disable_flag != nullptr) {
            const ParseResult r = parse({tf.disable_flag});
            if (r.config.features.*(tf.field)) {
                std::printf("[unit_phase8_cli] FAILED: %s did not turn its toggle "
                            "OFF (Req 13.1)\n", tf.disable_flag);
                ok = false;
            }
            for (const ToggleFlag& other : toggle_flags()) {
                if (other.field == tf.field) continue;
                if (r.config.features.*(other.field) != other.default_on) {
                    std::printf("[unit_phase8_cli] FAILED: %s disturbed another "
                                "Phase-8 toggle (should be isolated, Req 13.1)\n",
                                tf.disable_flag);
                    ok = false;
                }
            }
            if (!r.diagnostics.empty()) {
                std::printf("[unit_phase8_cli] FAILED: %s emitted a diagnostic "
                            "(disable toggles are silent)\n", tf.disable_flag);
                ok = false;
            }
        }

        // 2b. The bare enable flag sets its field ON (a compat no-op for the
        //     seven default-on capabilities; the real enable for the three
        //     opt-in ones) and disturbs no other  toggle.
        {
            const ParseResult r = parse({tf.enable_flag});
            if (!(r.config.features.*(tf.field))) {
                std::printf("[unit_phase8_cli] FAILED: %s did not set its toggle "
                            "ON (Req 13.1)\n", tf.enable_flag);
                ok = false;
            }
            for (const ToggleFlag& other : toggle_flags()) {
                if (other.field == tf.field) continue;
                if (r.config.features.*(other.field) != other.default_on) {
                    std::printf("[unit_phase8_cli] FAILED: %s disturbed another "
                                "Phase-8 toggle (should be isolated, Req 13.1)\n",
                                tf.enable_flag);
                    ok = false;
                }
            }
            if (!r.diagnostics.empty()) {
                std::printf("[unit_phase8_cli] FAILED: %s emitted a diagnostic "
                            "(enable toggles are silent)\n", tf.enable_flag);
                ok = false;
            }
        }
    }

    // ── 3a. --specular-aa-quality parses in range, clamps out of range ──
    {
        // In-range 0 → Low, 2 → High, no diagnostic.
        const ParseResult lo = parse({"--specular-aa-quality", "0"});
        if (lo.config.quality.specular_aa_level != heaven::QualityLevel::Low)
            fail("--specular-aa-quality 0 should map to Low");
        if (!lo.diagnostics.empty())
            fail("--specular-aa-quality 0 should not warn");

        const ParseResult hi = parse({"--specular-aa-quality", "2"});
        if (hi.config.quality.specular_aa_level != heaven::QualityLevel::High)
            fail("--specular-aa-quality 2 should map to High");
        if (!hi.diagnostics.empty())
            fail("--specular-aa-quality 2 should not warn");

        // Out-of-range 9 clamps to High WITH a diagnostic naming the flag.
        const ParseResult over = parse({"--specular-aa-quality", "9"});
        if (over.config.quality.specular_aa_level != heaven::QualityLevel::High)
            fail("--specular-aa-quality 9 should clamp to High");
        if (!has_diag_containing(over, "--specular-aa-quality"))
            fail("--specular-aa-quality 9 should emit a clamp diagnostic");

        // Below-range -1 clamps to Low WITH a diagnostic.
        const ParseResult under = parse({"--specular-aa-quality", "-1"});
        if (under.config.quality.specular_aa_level != heaven::QualityLevel::Low)
            fail("--specular-aa-quality -1 should clamp to Low");
        if (!has_diag_containing(under, "--specular-aa-quality"))
            fail("--specular-aa-quality -1 should emit a clamp diagnostic");
    }

    // ── 3b. --dynamic-resolution-quality parses in range, clamps out of range ──
    {
        // In-range value is accepted verbatim with no diagnostic.
        const ParseResult in = parse({"--dynamic-resolution-quality", "0.5"});
        if (std::fabs(in.config.quality.dynres_min_scale - 0.5f) > 1e-6f)
            fail("--dynamic-resolution-quality 0.5 should be accepted as-is");
        if (!in.diagnostics.empty())
            fail("--dynamic-resolution-quality 0.5 should not warn");

        // Above the upper bound clamps down WITH a diagnostic.
        const ParseResult hi = parse({"--dynamic-resolution-quality", "5"});
        if (std::fabs(hi.config.quality.dynres_min_scale - heaven::QualityKnobs::k_dynres_min_scale_hi) > 1e-6f)
            fail("--dynamic-resolution-quality above max should clamp to the upper bound");
        if (!has_diag_containing(hi, "--dynamic-resolution-quality"))
            fail("--dynamic-resolution-quality above max should emit a clamp diagnostic");

        // Below the lower bound clamps up WITH a diagnostic.
        const ParseResult lo = parse({"--dynamic-resolution-quality", "0.01"});
        if (std::fabs(lo.config.quality.dynres_min_scale - heaven::QualityKnobs::k_dynres_min_scale_lo) > 1e-6f)
            fail("--dynamic-resolution-quality below min should clamp to the lower bound");
        if (!has_diag_containing(lo, "--dynamic-resolution-quality"))
            fail("--dynamic-resolution-quality below min should emit a clamp diagnostic");
    }

    // ── 4. --tone-mapper: known sets enum; unknown retains ACES + warns ──
    {
        // A known name (case-insensitive, dash/space/underscore-insensitive)
        // selects the operator, no diagnostic.
        const ParseResult agx = parse({"--tone-mapper", "agx"});
        if (agx.config.tone_mapper != heaven::ToneMapperSelection::AgX)
            fail("--tone-mapper agx should select AgX");
        if (!agx.diagnostics.empty())
            fail("--tone-mapper agx should not warn");

        const ParseResult gt7 = parse({"--tone-mapper", "gt7"});
        if (gt7.config.tone_mapper != heaven::ToneMapperSelection::GT7)
            fail("--tone-mapper gt7 should select GT7");

        // An unknown name retains the ACES default and emits a diagnostic
        // identifying the rejected selection (parity to ACES).
        const ParseResult bad = parse({"--tone-mapper", "filmic-nope"});
        if (bad.config.tone_mapper != heaven::ToneMapperSelection::ACES)
            fail("--tone-mapper with an unknown name should retain ACES");
        if (!has_diag_containing(bad, "--tone-mapper"))
            fail("--tone-mapper with an unknown name should emit a reject diagnostic");
    }

    // ── 5. A grading value flag out of range clamps and warns ──
    {
        // In-range contrast is accepted with no diagnostic.
        const ParseResult in = parse({"--grading-contrast", "1.5"});
        if (std::fabs(in.config.grading.contrast - 1.5f) > 1e-6f)
            fail("--grading-contrast 1.5 should be accepted as-is");
        if (!in.diagnostics.empty())
            fail("--grading-contrast 1.5 should not warn");

        // Above the [0,2] bound clamps to 2.0 WITH a diagnostic.
        const ParseResult hi = parse({"--grading-contrast", "9"});
        if (std::fabs(hi.config.grading.contrast - heaven::ColorGradingConfig::k_contrast_hi) > 1e-6f)
            fail("--grading-contrast above max should clamp to k_contrast_hi");
        if (!has_diag_containing(hi, "--grading-contrast"))
            fail("--grading-contrast above max should emit a clamp diagnostic");

        // Below the bound clamps to 0.0 WITH a diagnostic.
        const ParseResult lo = parse({"--grading-contrast", "-3"});
        if (std::fabs(lo.config.grading.contrast - heaven::ColorGradingConfig::k_contrast_lo) > 1e-6f)
            fail("--grading-contrast below min should clamp to k_contrast_lo");
        if (!has_diag_containing(lo, "--grading-contrast"))
            fail("--grading-contrast below min should emit a clamp diagnostic");
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_phase8_cli{unit_phase8_cli};

} // namespace
