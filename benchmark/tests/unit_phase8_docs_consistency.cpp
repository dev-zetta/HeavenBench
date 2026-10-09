/*
 * HeavenBench — unit test:  docs/feature-flag consistency
 *
 * — Validates:
 *
 * The rendering-feature checklist (docs/RENDERING_FEATURES.md) is the source of
 * truth for what the renderer provides and how each capability is toggled.
 * of that doc records the eleven advanced-materials / shadow /
 * color-grading capabilities added by these changes. This deterministic test pins,
 * for each capability, the two guarantees  demands:
 *
 *   • EXACTLY ONE entry per capability — the controlling command-line flag
 *     string appears exactly once across the whole doc.
 *   • The recorded flag matches a flag the arg parser actually accepts.
 *
 * This is STRONGER than the advanced-rendering docs precedent
 * (unit_advanced_docs_consistency.cpp). That test could not link
 * main.cpp::parse_args, so it validated documented flags only against the
 * FeatureToggles data model. The enable flags are table-driven in
 * engine/cli_parser.cpp's flag_table(), which IS host-linkable (cli_parser.cpp
 * is in the heaven_benchmark_tests source list). So here we assert each
 * documented flag against heaven::flag_table() directly — the parser's own
 * single source of truth: a FlagSpec with that exact name must exist, with the
 * expected Arity (None for the bare enables and the --no-<feature> disables,
 * Value for --tone-mapper and the two quality knobs). For the seven
 * recommended capabilities we ALSO assert their `--no-<feature>` disable flag
 * exists in flag_table() as Arity::None, and that the default-constructed
 * FeatureToggles field reads ON; for the three opt-in capabilities the field
 * must read OFF — backing the doc's per-capability default annotation
 *
 *
 * Flags are counted in the EXACT backtick-wrapped form the doc records
 * (`--flag`). The trailing backtick is load-bearing: `--specular-aa` is a
 * substring of `--specular-aa-quality`, and `--dynamic-resolution` is a
 * substring of `--dynamic-resolution-quality`; counting the bare strings would
 * double-count. The trailing backtick disambiguates each toggle from its
 * quality flag (and `--cloth` from `--cloth-rate`, etc.).
 *
 * The doc is read at runtime via the WEATHER_DOCS_PATH compile-time macro, an
 * absolute path defined for the heaven_benchmark_tests target in
 * benchmark/CMakeLists.txt (it points to docs/RENDERING_FEATURES.md; ctest's
 * CWD is the build dir, so a relative path would be fragile). If the file
 * cannot be opened the test FAILS with a diagnostic — a missing or moved doc is
 * exactly the kind of drift this test exists to catch.
 *
 * Self-registers with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time, like the other test translation units.
 */
#include <array>
#include <cstdio>
#include <functional>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "engine/cli_parser.h"
#include "engine/types.h"

#ifndef WEATHER_DOCS_PATH
#error "WEATHER_DOCS_PATH must be defined by the build system (see benchmark/CMakeLists.txt)"
#endif

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Count non-overlapping occurrences of `needle` within `haystack`.
std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// Look a flag up in the parser's single source of truth, flag_table(). Returns
// nullptr when no FlagSpec carries that exact name.
const heaven::FlagSpec* find_flag(std::string_view name) {
    for (const heaven::FlagSpec& spec : heaven::flag_table()) {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

// One row of the doc/parser contract for a capability.

//   name           — human label, used only in diagnostics.
//   flag           — the controlling flag string the doc records and that
//                    flag_table() must accept. Counted in the
//                    backtick-wrapped form so substrings are not double-counted.
//   disable_flag   — the `--no-<feature>` disable flag for the seven default-on
//                    capabilities (which must ALSO exist in flag_table() as
//                    Arity::None), or nullptr for the three opt-in capabilities
//                    and the tone-mapper value flag.
//   arity          — the arity flag_table() must record: None for the bare
//                    `--<feature>` enables / `--no-<feature>` disables, Value
//                    for the --tone-mapper value flag (and the quality knobs).
//   default_on     — the expected default of the FeatureToggles bool: ON for
//                    the seven recommended capabilities, OFF for the three
//                    opt-in ones. Ignored when toggle == nullptr (tone-mapper).
//   toggle         — reader for the FeatureToggles bool; a fresh FeatureToggles
//                    must read this == default_on. nullptr for --tone-mapper.
//   quality_flag   — the `--<feature>-quality` knob, or nullptr when the
//                    capability has no quality knob.
struct Phase8Cap {
    const char*  name;
    const char*  flag;
    const char*  disable_flag;
    heaven::Arity arity;
    bool         default_on;
    bool (*toggle)(const heaven::FeatureToggles&);
    const char*  quality_flag;
};

const char* arity_name(heaven::Arity a) {
    return a == heaven::Arity::Value ? "Value" : "None";
}

bool unit_phase8_docs_consistency() {
    bool ok = true;

    // ── Read the feature checklist via the build-defined absolute path ──
    std::ifstream in(WEATHER_DOCS_PATH, std::ios::binary);
    if (!in) {
        std::printf("[unit_phase8_docs_consistency] FAILED: cannot open docs file "
                    "'%s' (Req 16.6 — the feature checklist must exist and be "
                    "locatable)\n",
                    WEATHER_DOCS_PATH);
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string doc = buffer.str();

    if (doc.empty()) {
        std::printf("[unit_phase8_docs_consistency] FAILED: docs file '%s' is "
                    "empty\n", WEATHER_DOCS_PATH);
        return false;
    }

    // The eleven  capabilities, their controlling flags, the arity the
    // parser must record, the FeatureToggles bool each enable flips and its
    // expected default, and (where present) the quality knob. By product
    // decision the seven recommended capabilities default ON, each exposing a
    // `--no-<feature>` disable flag plus a bare `--<feature>` compat enable
    // alias; the three opt-in capabilities default OFF as bare
    // `--<feature>` enables. --tone-mapper is the one value flag with no bool
    // toggle (an enum select; ACES by default).
    const std::array<Phase8Cap, 11> caps = {{
        // ── Group A — material models (no quality knob), default ON ──
        {"clear coat", "--clear-coat", "--no-clear-coat", heaven::Arity::None, true,
         [](const heaven::FeatureToggles& t) { return t.clear_coat; }, nullptr},
        {"anisotropic", "--anisotropic", "--no-anisotropic", heaven::Arity::None, true,
         [](const heaven::FeatureToggles& t) { return t.anisotropic; }, nullptr},
        {"subsurface / translucency", "--subsurface", "--no-subsurface", heaven::Arity::None, true,
         [](const heaven::FeatureToggles& t) { return t.subsurface; }, nullptr},
        {"cloth / sheen", "--cloth", "--no-cloth", heaven::Arity::None, true,
         [](const heaven::FeatureToggles& t) { return t.cloth; }, nullptr},

        // ── Group B — shadows & specular AA, default ON ──
        {"transparent foliage shadows", "--transparent-shadows", "--no-transparent-shadows",
         heaven::Arity::None, true,
         [](const heaven::FeatureToggles& t) { return t.transparent_shadows; }, nullptr},
        {"colored penumbra", "--colored-penumbra", "--no-colored-penumbra", heaven::Arity::None, true,
         [](const heaven::FeatureToggles& t) { return t.colored_penumbra; }, nullptr},
        {"specular AA", "--specular-aa", "--no-specular-aa", heaven::Arity::None, true,
         [](const heaven::FeatureToggles& t) { return t.specular_aa; },
         "--specular-aa-quality"},

        // ── Group C — tone-mapping, grading & camera ──
        {"tone-mapper selection", "--tone-mapper", nullptr, heaven::Arity::Value, false,
         nullptr, nullptr},
        {"full color grading", "--color-grading", nullptr, heaven::Arity::None, false,
         [](const heaven::FeatureToggles& t) { return t.color_grading; }, nullptr},
        {"physical camera", "--physical-camera", nullptr, heaven::Arity::None, false,
         [](const heaven::FeatureToggles& t) { return t.physical_camera; }, nullptr},

        // ── Group D — dynamic resolution, default OFF ──
        {"dynamic resolution + FSR", "--dynamic-resolution", nullptr, heaven::Arity::None, false,
         [](const heaven::FeatureToggles& t) { return t.dynamic_resolution; },
         "--dynamic-resolution-quality"},
    }};

    const heaven::FeatureToggles default_toggles;

    for (const Phase8Cap& c : caps) {
        // ──  (exactly one entry): the controlling flag string appears
        // exactly once, counted in the backtick-wrapped form the doc records so
        // `--specular-aa` / `--dynamic-resolution` are not double-counted by
        // their *-quality knobs (the trailing backtick disambiguates). ──
        const std::string flag_marked = std::string("`") + c.flag + "`";
        const std::size_t flag_occurrences = count_occurrences(doc, flag_marked);
        if (flag_occurrences != 1) {
            std::printf("[unit_phase8_docs_consistency] FAILED: controlling flag "
                        "'%s' appears %zu time(s) in %s, expected exactly 1 "
                        "(Req 16.6 — one entry per capability)\n",
                        c.flag, flag_occurrences, WEATHER_DOCS_PATH);
            ok = false;
        }

        // ──  (recorded flag matches the parser): the flag MUST exist in
        // flag_table() — the parser's own single source of truth — with the
        // documented arity. ──
        const heaven::FlagSpec* spec = find_flag(c.flag);
        if (spec == nullptr) {
            std::printf("[unit_phase8_docs_consistency] FAILED: documented flag "
                        "'%s' is not accepted by the parser (absent from "
                        "heaven::flag_table()) (Req 16.6)\n",
                        c.flag);
            ok = false;
        } else if (spec->arity != c.arity) {
            std::printf("[unit_phase8_docs_consistency] FAILED: flag '%s' has "
                        "arity %s in flag_table() but the doc/contract expects "
                        "%s (Req 16.6)\n",
                        c.flag, arity_name(spec->arity), arity_name(c.arity));
            ok = false;
        }

        // ──: the --no-<feature> disable flag (the seven default-on
        // capabilities) must ALSO exist in flag_table() as Arity::None — the
        // disable half of the alias pair. nullptr for the opt-in capabilities
        // and tone-mapper. ──
        if (c.disable_flag != nullptr) {
            const heaven::FlagSpec* no_spec = find_flag(c.disable_flag);
            if (no_spec == nullptr) {
                std::printf("[unit_phase8_docs_consistency] FAILED: disable flag "
                            "'%s' is not accepted by the parser (absent from "
                            "heaven::flag_table()) (Req 13.1)\n",
                            c.disable_flag);
                ok = false;
            } else if (no_spec->arity != heaven::Arity::None) {
                std::printf("[unit_phase8_docs_consistency] FAILED: disable flag "
                            "'%s' has arity %s in flag_table() but must be None "
                            "(Req 13.1)\n",
                            c.disable_flag, arity_name(no_spec->arity));
                ok = false;
            }
        }

        // ──  backing the doc's default annotation: each enable's
        // FeatureToggles bool must default to the documented value (ON for the
        // seven recommended capabilities, OFF for the three opt-in ones).
        // --tone-mapper has no toggle bool (an enum value flag), so it is
        // skipped here. ──
        if (c.toggle != nullptr) {
            if (c.toggle(default_toggles) != c.default_on) {
                std::printf("[unit_phase8_docs_consistency] FAILED: capability "
                            "'%s' (flag '%s') ships default %s but the doc "
                            "records it default %s (Req 13.1/14.1)\n",
                            c.name, c.flag,
                            c.toggle(default_toggles) ? "ON" : "OFF",
                            c.default_on ? "ON" : "OFF");
                ok = false;
            }
        }

        // ── Quality knob (where present): the `--<feature>-quality` flag must
        // appear exactly once and exist in flag_table() as Arity::Value —
        // proving the documented quality flag is one the parser accepts as a
        // value flag. Capabilities without a knob (quality_flag == nullptr) are
        // skipped. ──
        if (c.quality_flag != nullptr) {
            const std::string knob_marked = std::string("`") + c.quality_flag + "`";
            const std::size_t knob_occurrences = count_occurrences(doc, knob_marked);
            if (knob_occurrences != 1) {
                std::printf("[unit_phase8_docs_consistency] FAILED: quality flag "
                            "'%s' appears %zu time(s) in %s, expected exactly 1 "
                            "(Req 16.6)\n",
                            c.quality_flag, knob_occurrences, WEATHER_DOCS_PATH);
                ok = false;
            }

            const heaven::FlagSpec* knob = find_flag(c.quality_flag);
            if (knob == nullptr) {
                std::printf("[unit_phase8_docs_consistency] FAILED: documented "
                            "quality flag '%s' is not accepted by the parser "
                            "(absent from heaven::flag_table()) (Req 16.6)\n",
                            c.quality_flag);
                ok = false;
            } else if (knob->arity != heaven::Arity::Value) {
                std::printf("[unit_phase8_docs_consistency] FAILED: quality flag "
                            "'%s' has arity %s in flag_table() but must be Value "
                            "(Req 16.6)\n",
                            c.quality_flag, arity_name(knob->arity));
                ok = false;
            }
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_phase8_docs_consistency{
    unit_phase8_docs_consistency};

} // namespace
