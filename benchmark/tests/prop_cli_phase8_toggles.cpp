// Feature: advanced-materials-and-grading,: CLI flag parsing sets exactly the intended toggle
/*
 * HeavenBench — Property test for the CLI flag → toggle mapping.
 *
 * Feature: advanced-materials-and-grading
 *   •: CLI flag parsing sets exactly the intended toggle
 *
 *
 * (advanced-materials-and-grading) adds ten off-by-default enable
 * flags — one bare `--<feature>` per capability  — plus two
 * `--<feature>-quality` knob flags. The parse driver
 * heaven::parse_cli is fully implemented, so this property exercises the PUBLIC
 * entry point directly: it builds a real argv, calls parse_cli, and asserts on
 * the resulting BenchConfig.
 *
 * (enable flag sets exactly its toggle):
 *   For any single  enable flag, parsing it alone drives exactly that
 *   one FeatureToggles bool true and leaves EVERY other  toggle at its
 *   off default — so a flag can never silently flip a sibling capability
 * No diagnostic is emitted on a bare enable flag.
 *
 *   The two `--<feature>-quality` knob flags  set exactly their own
 *   knob through the shared pure clamp helpers and touch no  toggle:
 *     • --specular-aa-quality <L>      → quality.specular_aa_level via
 *                                        clamp_quality_level (atoi + clamp)
 *     • --dynamic-resolution-quality <S> → quality.dynres_min_scale via
 *                                        clamp_dynres_min_scale (atof + clamp)
 *
 * A completeness guard asserts every modelled  enable flag resolves to a
 * real Arity::None entry in the live flag_table(), so the model cannot drift
 * from the table the parser actually uses.
 *
 * This file is the OWNER of the value-flag CLI coverage referenced by
 * the cli-argument-parser-refactor suite (prop_cli_value_flags.cpp's
 * phase8_value_flags() set): the two quality knobs are covered here; the
 * tone-mapper / grading / physical-camera value flags are covered by the
 * CLI-defaults unit test.
 *
 * parse_cli is pure (no I/O, no GPU), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidcheck.h>

#include "engine/cli_parser.h"
#include "engine/types.h"

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

using heaven::BenchConfig;
using heaven::FeatureToggles;
using heaven::FlagSpec;
using heaven::ParseResult;
using heaven::QualityKnobs;

// ──  toggle-flag model ───────────────────────────────────

// One Group-A..D capability per row: its bare `--<feature>` enable flag, its
// `--no-<feature>` disable flag (empty for the three opt-in capabilities), the
// FeatureToggles bool it drives, and its default. The seven recommended
// capabilities default ON (with a --no- disable + compat enable alias); the
// three opt-in ones default OFF (bare enable only). The completeness guard
// cross-checks every name against the live table so the model cannot drift

struct ToggleFlag {
    std::string         enable;       // bare --<feature>
    std::string         disable;      // --no-<feature> or "" (opt-in capabilities)
    bool                default_on;
    bool FeatureToggles::* field;
};

const std::vector<ToggleFlag>& toggle_flags() {
    static const std::vector<ToggleFlag> v = {
        {"--clear-coat",          "--no-clear-coat",          true,  &FeatureToggles::clear_coat},
        {"--anisotropic",         "--no-anisotropic",         true,  &FeatureToggles::anisotropic},
        {"--subsurface",          "--no-subsurface",          true,  &FeatureToggles::subsurface},
        {"--cloth",               "--no-cloth",               true,  &FeatureToggles::cloth},
        {"--transparent-shadows", "--no-transparent-shadows", true,  &FeatureToggles::transparent_shadows},
        {"--colored-penumbra",    "--no-colored-penumbra",    true,  &FeatureToggles::colored_penumbra},
        {"--specular-aa",         "--no-specular-aa",         true,  &FeatureToggles::specular_aa},
        {"--color-grading",       "",                         false, &FeatureToggles::color_grading},
        {"--physical-camera",     "",                         false, &FeatureToggles::physical_camera},
        {"--dynamic-resolution",  "",                         false, &FeatureToggles::dynamic_resolution},
    };
    return v;
}

// Resolve a flag name to its FlagSpec in the live table (the single source of
// truth), so the test can confirm it routes through the same table the parser
// uses.
const FlagSpec* find_spec(std::string_view name) {
    for (const FlagSpec& s : heaven::flag_table()) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// Run the real parse driver over a token list (argv[0] is a dummy program name,
// as in a real launch).
ParseResult run_parse(const std::vector<std::string>& tokens) {
    std::vector<const char*> argv;
    argv.reserve(tokens.size() + 1);
    argv.push_back("heaven_benchmark");
    for (const std::string& t : tokens) argv.push_back(t.c_str());
    return heaven::parse_cli(argv);
}

// ── Completeness guard ──────────────────────────────────────────

// Every modelled  enable flag (and, for the seven default-on
// capabilities, its --no- disable flag) must resolve to a real Arity::None
// entry in the live flag_table(). Adding a toggle without modelling it
// here, or renaming one in the table, fails this guard rather than silently
// going untested.
bool run_phase8_coverage_guard() {
    bool ok = true;
    auto check_none = [&](const std::string& name) {
        const FlagSpec* spec = find_spec(name);
        if (spec == nullptr) {
            std::printf("[prop_cli_phase8_toggles] flag '%s' is not in the table\n",
                        name.c_str());
            ok = false;
            return;
        }
        if (spec->arity != heaven::Arity::None) {
            std::printf("[prop_cli_phase8_toggles] flag '%s' is not Arity::None\n",
                        name.c_str());
            ok = false;
        }
    };
    for (const ToggleFlag& tf : toggle_flags()) {
        check_none(tf.enable);
        if (!tf.disable.empty()) check_none(tf.disable);
    }
    // The two knob flags must be Arity::Value (they consume a level/scale token).
    for (std::string_view knob : {"--specular-aa-quality", "--dynamic-resolution-quality"}) {
        const FlagSpec* spec = find_spec(knob);
        if (spec == nullptr || spec->arity != heaven::Arity::Value) {
            std::printf("[prop_cli_phase8_toggles] knob flag '%.*s' missing or not Arity::Value\n",
                        static_cast<int>(knob.size()), knob.data());
            ok = false;
        }
    }
    return ok;
}

// Read a toggle off a config by its member pointer.
bool toggle(const BenchConfig& c, bool FeatureToggles::* field) {
    return c.features.*field;
}

// ── a: a single flag flips exactly its toggle (default-on aware) ─

// New default-on semantics: a `--no-<feature>` turns exactly its
// toggle OFF; a bare `--<feature>` leaves it ON (compat enable for the seven,
// the real enable for the three opt-in capabilities); every OTHER
// toggle keeps its NEW default (seven on, three off). Bare flags and disable
// flags emit no diagnostic.
bool run_enable_flag_isolation_property() {
    return rc::check(
        "Feature: advanced-materials-and-grading, Property 11: a Phase-8 enable "
        "or disable flag sets exactly its toggle, leaving every other Phase-8 "
        "toggle at its (new) default (Validates: Requirements 13.1, 13.6)",
        [] {
            const std::vector<ToggleFlag>& flags = toggle_flags();
            const std::size_t idx = *rc::gen::inRange<std::size_t>(0, flags.size());
            const ToggleFlag& chosen = flags[idx];
            const BenchConfig defaults;

            // Randomly exercise the bare enable, or (when present) the --no-
            // disable. The disable is only available for the seven default-on
            // capabilities.
            const bool use_disable =
                !chosen.disable.empty() && *rc::gen::arbitrary<bool>();
            const std::string flag = use_disable ? chosen.disable : chosen.enable;
            const bool expected_chosen = !use_disable; // enable → true, disable → false

            const ParseResult r = run_parse({flag});

            // The chosen toggle takes the flag's value; every other
            // toggle stays at its NEW default.
            for (const ToggleFlag& tf : flags) {
                if (tf.field == chosen.field) {
                    RC_ASSERT(toggle(r.config, tf.field) == expected_chosen);
                } else {
                    RC_ASSERT(toggle(r.config, tf.field) ==
                              toggle(defaults, tf.field));
                }
            }

            // A bare enable / --no- disable flag is a pure toggle: no
            // diagnostic, no help.
            RC_ASSERT(r.diagnostics.empty());
            RC_ASSERT(!r.help_requested);

            // It touches neither  knob (those are separate flags).
            RC_ASSERT(r.config.quality.specular_aa_level ==
                      defaults.quality.specular_aa_level);
            RC_ASSERT(r.config.quality.dynres_min_scale ==
                      defaults.quality.dynres_min_scale);
        });
}

// ── b: --specular-aa-quality sets exactly its knob ───
bool run_specular_aa_quality_property() {
    return rc::check(
        "Feature: advanced-materials-and-grading, Property 11: "
        "--specular-aa-quality sets quality.specular_aa_level via "
        "clamp_quality_level and touches no Phase-8 toggle (Validates: "
        "Requirements 13.6)",
        [] {
            // Span valid {0,1,2} levels and out-of-range values that exercise
            // the clamp path; the SAME helper computes the expectation.
            const int raw = *rc::gen::inRange(-4, 7);
            const std::string tok = std::to_string(raw);

            const ParseResult r = run_parse({"--specular-aa-quality", tok});

            // The knob equals the shared clamp helper's output.
            RC_ASSERT(r.config.quality.specular_aa_level ==
                      heaven::clamp_quality_level(raw).value);

            // It drives no  enable toggle.
            const BenchConfig defaults;
            for (const ToggleFlag& tf : toggle_flags()) {
                RC_ASSERT(toggle(r.config, tf.field) == toggle(defaults, tf.field));
            }
            // It leaves the sibling knob at its default.
            RC_ASSERT(r.config.quality.dynres_min_scale ==
                      defaults.quality.dynres_min_scale);

            // A diagnostic is emitted exactly when the value was clamped.
            const bool clamped = heaven::clamp_quality_level(raw).clamped;
            RC_ASSERT(r.diagnostics.size() == (clamped ? 1u : 0u));
        });
}

// ── c: --dynamic-resolution-quality sets exactly its knob ─
bool run_dynres_quality_property() {
    return rc::check(
        "Feature: advanced-materials-and-grading, Property 11: "
        "--dynamic-resolution-quality sets quality.dynres_min_scale via "
        "clamp_dynres_min_scale and touches no Phase-8 toggle (Validates: "
        "Requirements 13.6)",
        [] {
            // A fixed-point grid spanning the valid (0,1] band and out-of-range
            // values; the parser uses atof, so any of these round-trips.
            const int g = *rc::gen::inRange(-200, 301);
            const float raw = static_cast<float>(g) / 100.0f; // [-2.0, 3.0]
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(raw));
            const std::string tok(buf);

            const ParseResult r = run_parse({"--dynamic-resolution-quality", tok});

            // The knob equals the shared clamp helper's output. The
            // bounds come from the default knobs the parser clamps against.
            const QualityKnobs def_knobs;
            const heaven::FloatKnobClamp expect =
                heaven::clamp_dynres_min_scale(raw, def_knobs);
            RC_ASSERT(r.config.quality.dynres_min_scale == expect.value);

            // It drives no  enable toggle.
            const BenchConfig defaults;
            for (const ToggleFlag& tf : toggle_flags()) {
                RC_ASSERT(toggle(r.config, tf.field) == toggle(defaults, tf.field));
            }
            // It leaves the sibling knob at its default.
            RC_ASSERT(r.config.quality.specular_aa_level ==
                      defaults.quality.specular_aa_level);

            // A diagnostic is emitted exactly when the value was clamped.
            RC_ASSERT(r.diagnostics.size() == (expect.clamped ? 1u : 0u));
        });
}

bool run_cli_phase8_toggles_properties() {
    bool ok = true;
    ok = run_phase8_coverage_guard() && ok;
    ok = run_enable_flag_isolation_property() && ok;
    ok = run_specular_aa_quality_property() && ok;
    ok = run_dynres_quality_property() && ok;
    return ok;
}

const heaven::testing::Register g_register_cli_phase8_toggles{
    &run_cli_phase8_toggles_properties};

} // namespace
