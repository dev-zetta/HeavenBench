// Feature: cli-argument-parser-refactor,  &
/*
 * HeavenBench — Property tests for the CLI parser's boolean toggles and
 * backward-compatibility alias pairs.
 *
 * Feature: cli-argument-parser-refactor
 *   •: Boolean toggles set their declared target(s)
 *
 *
 *   •: Alias-pair last occurrence wins
 *
 *
 * IMPORTANT — parse_cli is still a STUB at this stage of the refactor (the
 * parse driver lands in a later task). The flag_table(), however, is the
 * single source of truth and is fully populated. These properties therefore
 * exercise the boolean-toggle and alias behaviour by invoking the `apply`
 * lambdas from heaven::flag_table() DIRECTLY — never parse_cli, which would
 * return defaults and make the assertions meaningless.
 *
 * (toggles drive their declared targets):
 *   Starting from a default-constructed BenchConfig, apply a random subset of
 *   the Arity::None toggle/alias lambdas (at most one per controlled field, so
 *   the subset is conflict-free). Each applied toggle must drive its declared
 *   target field(s) to its declared fixed value (--no-heat-haze drives BOTH
 *   heat_haze_enabled and features.heat_haze to false), and every toggle-target
 *   field NOT touched by a selected toggle must remain at its default. A
 *   handful of unrelated non-toggle fields (width, time_of_day) are also
 *   asserted unchanged to confirm toggles touch nothing else.
 *
 * (alias last-occurrence wins):
 *   For each alias pair (--no-<name> / --<name>) — the original seven plus the
 *   seven  default-on capabilities — apply a random-ordered,
 *   random-length sequence of the two lambdas; the final value of the
 *   controlled features toggle must equal the value implied by the LAST
 *   occurrence in left-to-right order.
 *
 * A completeness guard additionally asserts that every Arity::None entry in
 * flag_table() (except --help, whose apply is nullptr) is modelled here exactly
 * once, so the property cannot silently stop covering a toggle that is added to
 * the table later (— every toggle routes through the table).
 *
 * The lambdas are pure (no I/O, no GPU), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rapidcheck.h>

#include "engine/cli_parser.h"

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
using heaven::Diagnostics;
using heaven::FlagSpec;
using heaven::ParseResult;

// ── Field identity for every toggle-controlled field ────────────

// One key per boolean field any Arity::None entry can drive. get_field() reads
// the corresponding member out of a BenchConfig so the property can compare
// against an independently computed expectation without hand-writing 25 member
// accesses per assertion.
enum class FieldKey {
    Fullscreen,
    TessEnabled,
    HeatHazeEnabled,
    FeatHeatHaze,
    Grass,
    Particles,
    GodRays,
    VolFog,
    Sky,
    LensFlares,
    SoftShadows,
    ContactShadows,
    Bloom,
    AmbientOcclusion,
    Taa,
    DepthOfField,
    Clouds,
    WindDebug,
    Hdr,
    Rain,
    WetReflections,
    Benchmark,
    PointLightShadows,
    Msaa,
    Refraction,
    Reflections,
    VolumetricClouds,
    Ssgi,
    MassiveFoliage,
    GroundRelief,
    // ── background-music-playback — music enable toggle ──
    Music,
    // ──  — advanced-materials-and-grading enable toggles ──
    ClearCoat,
    Anisotropic,
    Subsurface,
    Cloth,
    TransparentShadows,
    ColoredPenumbra,
    SpecularAa,
    GpuInstancing,
    GpuSkinning,
    ColorGrading,
    PhysicalCamera,
    DynamicResolution,
    CpuStress,
};

bool get_field(const BenchConfig& c, FieldKey k) {
    switch (k) {
        case FieldKey::Fullscreen:        return c.fullscreen;
        case FieldKey::TessEnabled:       return c.tessellation.enabled;
        case FieldKey::HeatHazeEnabled:   return c.heat_haze_enabled;
        case FieldKey::FeatHeatHaze:      return c.features.heat_haze;
        case FieldKey::Grass:             return c.features.grass;
        case FieldKey::Particles:         return c.features.particles;
        case FieldKey::GodRays:           return c.features.god_rays;
        case FieldKey::VolFog:            return c.features.volumetric_fog;
        case FieldKey::Sky:               return c.features.sky;
        case FieldKey::LensFlares:        return c.features.lens_flares;
        case FieldKey::SoftShadows:       return c.features.soft_shadows;
        case FieldKey::ContactShadows:    return c.features.contact_shadows;
        case FieldKey::Bloom:             return c.features.bloom;
        case FieldKey::AmbientOcclusion:  return c.features.ambient_occlusion;
        case FieldKey::Taa:               return c.features.taa;
        case FieldKey::DepthOfField:      return c.features.depth_of_field;
        case FieldKey::Clouds:            return c.features.clouds;
        case FieldKey::WindDebug:         return c.wind_debug;
        case FieldKey::Hdr:               return c.hdr;
        case FieldKey::Rain:              return c.features.rain;
        case FieldKey::WetReflections:    return c.features.wet_reflections;
        case FieldKey::Benchmark:         return c.benchmark_mode;
        case FieldKey::PointLightShadows: return c.features.point_light_shadows;
        case FieldKey::Msaa:              return c.features.msaa;
        case FieldKey::Refraction:        return c.features.refraction;
        case FieldKey::Reflections:       return c.features.reflections;
        case FieldKey::VolumetricClouds:  return c.features.volumetric_clouds;
        case FieldKey::Ssgi:              return c.features.ssgi;
        case FieldKey::MassiveFoliage:    return c.features.massive_foliage;
        case FieldKey::GroundRelief:      return c.features.ground_relief;
        case FieldKey::Music:             return c.music.enabled;
        case FieldKey::ClearCoat:          return c.features.clear_coat;
        case FieldKey::Anisotropic:        return c.features.anisotropic;
        case FieldKey::Subsurface:         return c.features.subsurface;
        case FieldKey::Cloth:              return c.features.cloth;
        case FieldKey::TransparentShadows: return c.features.transparent_shadows;
        case FieldKey::ColoredPenumbra:    return c.features.colored_penumbra;
        case FieldKey::SpecularAa:         return c.features.specular_aa;
        case FieldKey::GpuInstancing:      return c.features.gpu_instancing;
        case FieldKey::GpuSkinning:        return c.features.gpu_skinning;
        case FieldKey::ColorGrading:       return c.features.color_grading;
        case FieldKey::PhysicalCamera:     return c.features.physical_camera;
        case FieldKey::DynamicResolution:  return c.features.dynamic_resolution;
        case FieldKey::CpuStress:          return c.cpu_stress.enabled;
    }
    // Unreachable; silences -Wreturn-type.
    return false;
}

// One declared target of a toggle: the field it drives and the fixed value it
// drives it to.
struct Target {
    FieldKey key;
    bool     value;
};

// A single Arity::None entry as modelled here: its flag name plus the declared
// target(s) its apply lambda sets. --no-heat-haze carries two targets.
struct ToggleCandidate {
    std::string         name;
    std::vector<Target> targets;
};

// A "controller" groups the candidate toggle(s) that drive one logical field.
// For a plain toggle the group has a single candidate (present or absent); for
// an alias pair the group has two mutually-exclusive candidates (the disable
// and the enable member). The property selects AT MOST ONE candidate per
// controller, which guarantees the chosen subset never sets the same field
// twice — keeping  conflict-free (alias conflicts are 's
// job).
struct Controller {
    std::vector<ToggleCandidate> candidates;
};

// Every toggle-controlled field, with the candidate(s) that drive it. This is
// the test's model of the Arity::None half of flag_table(); the completeness
// guard below cross-checks it against the live table so it cannot drift.
const std::vector<Controller>& controllers() {
    static const std::vector<Controller> c = [] {
        std::vector<Controller> v;
        auto single = [&](std::string name, FieldKey key, bool value) {
            v.push_back(Controller{{ToggleCandidate{std::move(name), {{key, value}}}}});
        };
        auto pair = [&](std::string no_name, std::string yes_name, FieldKey key) {
            v.push_back(Controller{{
                ToggleCandidate{std::move(no_name), {{key, false}}},
                ToggleCandidate{std::move(yes_name), {{key, true}}},
            }});
        };

        // Plain boolean toggles.
        single("--fullscreen", FieldKey::Fullscreen, true);
        single("--no-tessellation", FieldKey::TessEnabled, false);
        // --no-heat-haze is the one dual-target toggle.
        v.push_back(Controller{{ToggleCandidate{"--no-heat-haze",
            {{FieldKey::HeatHazeEnabled, false}, {FieldKey::FeatHeatHaze, false}}}}});
        single("--no-grass", FieldKey::Grass, false);
        single("--no-particles", FieldKey::Particles, false);
        single("--no-god-rays", FieldKey::GodRays, false);
        single("--no-volumetric-fog", FieldKey::VolFog, false);
        single("--no-sky", FieldKey::Sky, false);
        single("--no-lens-flares", FieldKey::LensFlares, false);
        single("--no-soft-shadows", FieldKey::SoftShadows, false);
        single("--no-contact-shadows", FieldKey::ContactShadows, false);
        single("--clouds", FieldKey::Clouds, true);
        single("--wind-debug", FieldKey::WindDebug, true);
        single("--hdr", FieldKey::Hdr, true);
        single("--no-rain", FieldKey::Rain, false);
        single("--no-wet-reflections", FieldKey::WetReflections, false);
        single("--benchmark", FieldKey::Benchmark, true);

        // gpu-instancing-and-skinning — both default ON; only a --no-<feature>
        // disable flag (no compat enable), so a plain single toggle each.
        single("--no-gpu-instancing", FieldKey::GpuInstancing, false);
        single("--no-gpu-skinning", FieldKey::GpuSkinning, false);

        // Backward-compatibility alias pairs.
        pair("--no-point-light-shadows", "--point-light-shadows", FieldKey::PointLightShadows);
        pair("--no-msaa", "--msaa", FieldKey::Msaa);
        pair("--no-bloom", "--bloom", FieldKey::Bloom);
        pair("--no-ambient-occlusion", "--ambient-occlusion", FieldKey::AmbientOcclusion);
        pair("--no-taa", "--taa", FieldKey::Taa);
        pair("--no-depth-of-field", "--depth-of-field", FieldKey::DepthOfField);
        pair("--no-refraction", "--refraction", FieldKey::Refraction);
        pair("--no-reflections", "--reflections", FieldKey::Reflections);
        pair("--no-volumetric-clouds", "--volumetric-clouds", FieldKey::VolumetricClouds);
        pair("--no-ssgi", "--ssgi", FieldKey::Ssgi);
        pair("--no-massive-foliage", "--massive-foliage", FieldKey::MassiveFoliage);
        pair("--no-ground-relief", "--ground-relief", FieldKey::GroundRelief);

        // background-music-playback — music defaults ON; --no-music disables,
        // --music is the explicit enable. An alias pair (last occurrence wins,

        pair("--no-music", "--music", FieldKey::Music);

        // — the seven recommended capabilities now default ON, each an
        // alias pair: --no-<feature> disables, --<feature> is a compat enable
        // The three opt-in capabilities keep their default OFF, with explicit
        // on/off controls for isolated feature captures.
        pair("--no-clear-coat", "--clear-coat", FieldKey::ClearCoat);
        pair("--no-anisotropic", "--anisotropic", FieldKey::Anisotropic);
        pair("--no-subsurface", "--subsurface", FieldKey::Subsurface);
        pair("--no-cloth", "--cloth", FieldKey::Cloth);
        pair("--no-transparent-shadows", "--transparent-shadows", FieldKey::TransparentShadows);
        pair("--no-colored-penumbra", "--colored-penumbra", FieldKey::ColoredPenumbra);
        pair("--no-specular-aa", "--specular-aa", FieldKey::SpecularAa);
        pair("--no-color-grading", "--color-grading", FieldKey::ColorGrading);
        pair("--no-physical-camera", "--physical-camera", FieldKey::PhysicalCamera);
        pair("--no-dynamic-resolution", "--dynamic-resolution", FieldKey::DynamicResolution);
        pair("--no-cpu-stress", "--cpu-stress", FieldKey::CpuStress);

        return v;
    }();
    return c;
}

// All field keys, so the property can compare every toggle-target field against
// its expected post-apply value.
const std::vector<FieldKey>& all_field_keys() {
    static const std::vector<FieldKey> keys = {
        FieldKey::Fullscreen, FieldKey::TessEnabled, FieldKey::HeatHazeEnabled,
        FieldKey::FeatHeatHaze, FieldKey::Grass, FieldKey::Particles,
        FieldKey::GodRays, FieldKey::VolFog, FieldKey::Sky, FieldKey::LensFlares,
        FieldKey::SoftShadows, FieldKey::ContactShadows, FieldKey::Clouds,
        FieldKey::Bloom, FieldKey::AmbientOcclusion, FieldKey::Taa, FieldKey::DepthOfField,
        FieldKey::WindDebug, FieldKey::Hdr, FieldKey::Rain, FieldKey::WetReflections,
        FieldKey::Benchmark, FieldKey::PointLightShadows, FieldKey::Msaa,
        FieldKey::Refraction, FieldKey::Reflections, FieldKey::VolumetricClouds,
        FieldKey::Ssgi, FieldKey::MassiveFoliage,
        FieldKey::Music,
        FieldKey::ClearCoat, FieldKey::Anisotropic, FieldKey::Subsurface,
        FieldKey::Cloth, FieldKey::TransparentShadows, FieldKey::ColoredPenumbra,
        FieldKey::SpecularAa, FieldKey::ColorGrading, FieldKey::PhysicalCamera,
        FieldKey::DynamicResolution,
        FieldKey::CpuStress,
        FieldKey::GpuInstancing, FieldKey::GpuSkinning,
    };
    return keys;
}

// Resolve a flag name to its FlagSpec in the live table (the single source of
// truth), so the test applies the SAME lambda the parser will.
const FlagSpec* find_spec(std::string_view name) {
    for (const FlagSpec& s : heaven::flag_table()) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// ── Used-asset-packer startup-action toggles ────────────────────

// --pack-move / --pack-overwrite are Arity::None entries, but unlike every
// BenchConfig toggle above their apply lambda is a deliberate no-op: the parse
// driver special-cases them by name and records packer intent on ParseResult
// (a startup ACTION), because the apply signature cannot reach ParseResult.
// They therefore cannot be exercised through the apply-lambda path
// uses — applying their lambda would (correctly) do nothing. They are modelled
// here against the PUBLIC parse_cli entry point instead (their real behaviour):
// each must drive its ParseResult bool true when present and leave it false
// when absent, touching no BenchConfig field and emitting no diagnostic. They
// are still enumerated in the coverage guard's `modelled` set below so that set
// stays a one-to-one image of the Arity::None half of flag_table()
// (— every toggle is accounted for).
struct PackToggle {
    std::string         name;   // the --pack-* flag name
    bool ParseResult::* field;  // the ParseResult bool it drives
};

const std::vector<PackToggle>& pack_toggles() {
    static const std::vector<PackToggle> p = {
        {"--pack-move",      &ParseResult::pack_move},      // used-asset-packer
        {"--pack-overwrite", &ParseResult::pack_overwrite}, // used-asset-packer
    };
    return p;
}

// Run the real parse driver over a token list (argv[0] is a dummy program
// name, as in a real launch). Used to exercise the packer toggles through the
// same public entry point the application uses.
ParseResult run_parse(const std::vector<std::string>& tokens) {
    std::vector<const char*> argv;
    argv.reserve(tokens.size() + 1);
    argv.push_back("heaven_benchmark");
    for (const std::string& t : tokens) argv.push_back(t.c_str());
    return heaven::parse_cli(argv);
}

// ── Completeness guard ──────────────────────────────────────────

// Assert the model above covers exactly the Arity::None entries of the live
// flag_table() (excluding --help, whose apply is nullptr). This makes the
// property robust to future table edits: adding a toggle without modelling it
// here fails this guard rather than silently going untested.
bool run_toggle_coverage_guard() {
    std::set<std::string_view> modelled;
    for (const Controller& ctrl : controllers()) {
        for (const ToggleCandidate& cand : ctrl.candidates) {
            modelled.insert(cand.name);
        }
    }
    // The used-asset-packer startup-action toggles are modelled by
    // run_pack_toggles_property() against parse_cli (their applys are no-ops),
    // so enumerate them here too — the guard stays a one-to-one image of every
    // Arity::None entry in the table.
    for (const PackToggle& pt : pack_toggles()) {
        modelled.insert(pt.name);
    }

    // Transient capture intent is modeled by unit_visual_capture_tour_motion:
    // all prerequisite combinations, order/repetition and config-key isolation.
    modelled.insert("--capture-tour-motion");
    modelled.insert("--capture-floating-motion");
    modelled.insert("--capture-realtime");
    // Transient viewer toggle prerequisite/config isolation lives in unit_microprofiler_cli.cpp.
    modelled.insert("--microprofile-visible");
    // Startup-only listing is checked by unit_render_test_scene_config.cpp.
    modelled.insert("--list-test-scenes");

    std::set<std::string_view> table_toggles;
    for (const FlagSpec& s : heaven::flag_table()) {
        if (s.arity != heaven::Arity::None) continue;
        if (s.name == "--help") continue; // apply is nullptr by design
        table_toggles.insert(s.name);
    }

    bool ok = true;
    for (std::string_view n : table_toggles) {
        if (!modelled.contains(n)) {
            std::printf("[prop_cli_toggles] table toggle '%.*s' is not modelled\n",
                        static_cast<int>(n.size()), n.data());
            ok = false;
        }
    }
    for (std::string_view n : modelled) {
        if (!table_toggles.contains(n)) {
            std::printf("[prop_cli_toggles] modelled toggle '%.*s' is not in the table\n",
                        static_cast<int>(n.size()), n.data());
            ok = false;
        }
    }
    return ok;
}

// ──: toggles drive their declared targets ────────────
bool run_toggle_targets_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 3: Boolean toggles set "
        "their declared target(s) (Validates: Requirements 3.1, 3.2, 3.3, 3.4, "
        "3.5, 3.6, 3.7, 3.8, 4.1, 4.2, 13.1)",
        [] {
            const std::vector<Controller>& ctrls = controllers();

            // For each controller independently pick "none" (index ==
            // candidates.size()) or exactly one candidate. This yields a random
            // subset that never drives the same field twice.
            std::vector<const ToggleCandidate*> selected;
            for (const Controller& ctrl : ctrls) {
                const std::size_t n = ctrl.candidates.size();
                const std::size_t pick = *rc::gen::inRange<std::size_t>(0, n + 1);
                if (pick < n) {
                    selected.push_back(&ctrl.candidates[pick]);
                }
            }

            // Build the expectation: every field at its default, overridden by
            // the declared target value of each selected candidate.
            const BenchConfig defaults;
            BenchConfig cfg; // default-constructed; toggles mutate from here.
            Diagnostics diag;

            std::vector<std::pair<FieldKey, bool>> expected_overrides;
            for (const ToggleCandidate* cand : selected) {
                const FlagSpec* spec = find_spec(cand->name);
                RC_ASSERT(spec != nullptr);
                RC_ASSERT(spec->arity == heaven::Arity::None);
                RC_ASSERT(static_cast<bool>(spec->apply));
                // Apply the table's own lambda (the single source of truth).
                spec->apply(cfg, {}, diag);
                for (const Target& tgt : cand->targets) {
                    expected_overrides.emplace_back(tgt.key, tgt.value);
                }
            }

            auto expected_for = [&](FieldKey key) -> bool {
                bool value = get_field(defaults, key);
                for (const auto& [k, v] : expected_overrides) {
                    if (k == key) value = v;
                }
                return value;
            };

            // Every toggle-target field matches the expectation: selected
            // toggles drove their target(s); untouched fields kept defaults.
            for (FieldKey key : all_field_keys()) {
                RC_ASSERT(get_field(cfg, key) == expected_for(key));
            }

            // Toggles touch no unrelated (non-toggle) fields.
            RC_ASSERT(cfg.width == defaults.width);
            RC_ASSERT(cfg.height == defaults.height);
            RC_ASSERT(cfg.time_of_day == defaults.time_of_day);
            RC_ASSERT(cfg.data_dir == defaults.data_dir);

            // Pure toggles never emit diagnostics.
            RC_ASSERT(diag.messages.empty());
        });
}

// ──: alias-pair last occurrence wins ─────────────────

// One alias pair: the disable member, the enable member, and the field they
// jointly control. Mirrors the seven pairs registered in the table.
struct AliasPair {
    std::string name;     // human label for the rc::check message
    std::string no_flag;  // sets the field false
    std::string yes_flag; // sets the field true
    FieldKey    key;
};

const std::vector<AliasPair>& alias_pairs() {
    static const std::vector<AliasPair> p = {
        {"point-light-shadows", "--no-point-light-shadows", "--point-light-shadows", FieldKey::PointLightShadows},
        {"msaa",                "--no-msaa",                "--msaa",                FieldKey::Msaa},
        {"bloom",               "--no-bloom",               "--bloom",               FieldKey::Bloom},
        {"ambient-occlusion",   "--no-ambient-occlusion",   "--ambient-occlusion",   FieldKey::AmbientOcclusion},
        {"taa",                 "--no-taa",                 "--taa",                 FieldKey::Taa},
        {"depth-of-field",      "--no-depth-of-field",      "--depth-of-field",      FieldKey::DepthOfField},
        {"refraction",          "--no-refraction",          "--refraction",          FieldKey::Refraction},
        {"reflections",         "--no-reflections",         "--reflections",         FieldKey::Reflections},
        {"volumetric-clouds",   "--no-volumetric-clouds",   "--volumetric-clouds",   FieldKey::VolumetricClouds},
        {"ssgi",                "--no-ssgi",                "--ssgi",                FieldKey::Ssgi},
        {"massive-foliage",     "--no-massive-foliage",     "--massive-foliage",     FieldKey::MassiveFoliage},
        {"ground-relief",       "--no-ground-relief",       "--ground-relief",       FieldKey::GroundRelief},
        // background-music-playback — music enable/disable alias pair.
        {"music",               "--no-music",               "--music",               FieldKey::Music},
        // default-on alias pairs (advanced-materials-and-grading).
        {"clear-coat",          "--no-clear-coat",          "--clear-coat",          FieldKey::ClearCoat},
        {"anisotropic",         "--no-anisotropic",         "--anisotropic",         FieldKey::Anisotropic},
        {"subsurface",          "--no-subsurface",          "--subsurface",          FieldKey::Subsurface},
        {"cloth",               "--no-cloth",               "--cloth",               FieldKey::Cloth},
        {"transparent-shadows", "--no-transparent-shadows", "--transparent-shadows", FieldKey::TransparentShadows},
        {"colored-penumbra",    "--no-colored-penumbra",    "--colored-penumbra",    FieldKey::ColoredPenumbra},
        {"specular-aa",         "--no-specular-aa",         "--specular-aa",         FieldKey::SpecularAa},
    };
    return p;
}

bool run_alias_last_wins_property() {
    bool ok = true;
    for (const AliasPair& ap : alias_pairs()) {
        const bool pass = rc::check(
            "Feature: cli-argument-parser-refactor, Property 4: Alias-pair last "
             "occurrence wins [" + ap.name + "] (Validates: Requirements 4.3)",
            [&ap] {
                const FlagSpec* no_spec  = find_spec(ap.no_flag);
                const FlagSpec* yes_spec = find_spec(ap.yes_flag);
                RC_ASSERT(no_spec != nullptr);
                RC_ASSERT(yes_spec != nullptr);

                // A non-empty random-ordered sequence: each element is true
                // (apply the enable member) or false (apply the disable member).
                const std::vector<bool> seq =
                    *rc::gen::nonEmpty(rc::gen::container<std::vector<bool>>(
                        rc::gen::arbitrary<bool>()));

                BenchConfig cfg;
                Diagnostics diag;
                for (bool enable : seq) {
                    const FlagSpec* spec = enable ? yes_spec : no_spec;
                    spec->apply(cfg, {}, diag);
                }

                // The final value equals the value implied by the last token.
                RC_ASSERT(get_field(cfg, ap.key) == seq.back());
                RC_ASSERT(diag.messages.empty());
            });
        ok = pass && ok;
    }
    return ok;
}

// ── Used-asset-packer toggles drive their ParseResult target ────

// Mirrors 's intent for the two startup-action packer toggles, which
// the apply-lambda path cannot reach. For a random subset of {--pack-move,
// --pack-overwrite}, parse_cli must set exactly the selected toggles'
// ParseResult bools true and leave the unselected ones false, while touching no
// BenchConfig field, requesting no help, leaving the value-bearing packer
// outputs (pack_requested / pack_target) untouched, and emitting no diagnostic.
bool run_pack_toggles_property() {
    return rc::check(
        "Feature: cli-argument-parser-refactor, Property 3: used-asset-packer "
        "toggles set their ParseResult target (Validates: used-asset-packer "
        "Requirements 4.2, 5.3; Requirement 13.1)",
        [] {
            // Independently include or omit each packer toggle, in table order.
            const std::vector<PackToggle>& toggles = pack_toggles();
            std::vector<std::string> tokens;
            std::vector<bool> selected(toggles.size(), false);
            for (std::size_t i = 0; i < toggles.size(); ++i) {
                if (*rc::gen::arbitrary<bool>()) {
                    selected[i] = true;
                    tokens.push_back(toggles[i].name);
                }
            }

            const ParseResult r = run_parse(tokens);

            // Each selected toggle drove its bool true; each omitted toggle kept
            // its false default.
            for (std::size_t i = 0; i < toggles.size(); ++i) {
                bool ParseResult::* const field = toggles[i].field;
                const bool got = r.*field;
                RC_ASSERT(got == selected[i]);
            }

            // The packer toggles touch nothing else: no value-bearing packer
            // output, no help, no diagnostics, and no BenchConfig drift.
            const BenchConfig defaults;
            RC_ASSERT(r.pack_requested == false);
            RC_ASSERT(!r.pack_target.has_value());
            RC_ASSERT(!r.help_requested);
            RC_ASSERT(r.diagnostics.empty());
            RC_ASSERT(r.config.fullscreen == defaults.fullscreen);
            RC_ASSERT(r.config.features.grass == defaults.features.grass);
            RC_ASSERT(r.config.width == defaults.width);
        });
}

bool run_cli_toggles_properties() {
    bool ok = true;
    ok = run_toggle_coverage_guard() && ok;
    ok = run_toggle_targets_property() && ok;
    ok = run_alias_last_wins_property() && ok;
    ok = run_pack_toggles_property() && ok;
    return ok;
}

const heaven::testing::Register g_register_cli_toggles{&run_cli_toggles_properties};

} // namespace
