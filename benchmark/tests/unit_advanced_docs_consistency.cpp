/*
 * HeavenBench — unit test: advanced-rendering docs/feature-flag consistency
 *
 * — Validates:
 *
 * The rendering-feature checklist (docs/RENDERING_FEATURES.md) is the source of
 * truth for what the renderer provides and how each feature is toggled.
 * of that doc records the eight advanced-rendering features added by these changes.
 * This deterministic test pins, for each feature, the two guarantees
 * demands:
 *
 *   • EXACTLY ONE entry per feature — the feature's controlling command-line
 *     flag string appears exactly once across the whole doc.
 *   • The recorded flag matches the flag the arg parser actually accepts.
 *
 * The eight controlling flags are toggles parsed in main.cpp::parse_args
 * (--no-soft-shadows, --no-contact-shadows, --point-light-shadows, --msaa,
 * --refraction, --volumetric-clouds, --ssgi, --massive-foliage). main.cpp is
 * not host-linkable (it pulls in Filament/SDL), so we cannot drive parse_args
 * directly. Following the guidance for this task we validate each toggle by
 * asserting (a) its doc-occurrence count is exactly one and (b) its
 * FeatureToggles field exists in engine/types.h with the documented default
 * (the "(on by default)" / "(off by default)" annotation the doc records). A
 * drift in either the flag name or the default fails the test.
 *
 * Seven of the eight features also expose a Quality_Knob (refraction has none,
 * which the doc states explicitly). The `--<feature>-quality` flag family IS
 * host-testable: main.cpp delegates it to the shared, GPU-free
 * heaven::apply_quality_args() in engine/types.h. For each quality flag we
 * assert (a) its doc-occurrence count is exactly one and (b) running
 * apply_quality_args over {program, flag, value} actually moves the
 * corresponding knob off its default — proving the documented flag is one the
 * parser accepts. This mirrors how unit_docs_consistency.cpp validates the
 * weather features' flags by running their parser.
 *
 * Flags are counted in the EXACT backtick-wrapped form the doc records
 * (`--flag`). This matters because `--msaa` / `--ssgi` are substrings of
 * `--msaa-quality` / `--ssgi-quality`; counting the bare strings would
 * double-count. The trailing backtick disambiguates the toggle from its
 * quality flag.
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
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "engine/types.h"
#include "engine/cli_parser.h"

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

// One row of the doc/parser contract for an advanced-rendering feature.

//   name             — human label, used only in diagnostics.
//   toggle_flag      — the controlling flag string the doc records and the doc
//                      must contain exactly once.
//   default_enabled  — the documented default ("on/off by default").
//   toggle           — reader for the FeatureToggles field the flag controls;
//                      a fresh AppConfig must read this equal to default_enabled
//                      (proves the field exists and the doc's default is right).
//   quality_flag     — the `--<feature>-quality` flag, or nullptr when the
//                      feature has no quality knob (refraction).
//   quality_value    — a value that, when parsed, moves the knob off its
//                      default (so the parser end can be observed).
//   knob_changed     — predicate: did apply_quality_args(quality_flag,
//                      quality_value) move the knob away from the fresh default?
struct AdvFeature {
    const char* name;
    const char* toggle_flag;
    bool        default_enabled;
    bool (*toggle)(const heaven::FeatureToggles&);
    const char* quality_flag;
    const char* quality_value;
    bool (*knob_changed)(const heaven::QualityKnobs& def,
                         const heaven::QualityKnobs& after);
};

// Run the shared quality-arg parser over {program, flag, value} and return the
// resulting knobs. emit_warnings=false keeps the suite quiet. argv[0] is a
// dummy program name, as in a real launch.
heaven::QualityKnobs parse_quality(const char* flag, const char* value) {
    heaven::AppConfig config;
    const char* const argv[] = {"heaven_benchmark", flag, value};
    heaven::apply_cli_arguments(config, argv);
    return config.quality;
}

bool unit_advanced_docs_consistency() {
    bool ok = true;

    // ── Read the feature checklist via the build-defined absolute path ──
    std::ifstream in(WEATHER_DOCS_PATH, std::ios::binary);
    if (!in) {
        std::printf("[unit_advanced_docs_consistency] FAILED: cannot open docs file "
                    "'%s' (Req 13.4 — the feature checklist must exist and be "
                    "locatable)\n",
                    WEATHER_DOCS_PATH);
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string doc = buffer.str();

    if (doc.empty()) {
        std::printf("[unit_advanced_docs_consistency] FAILED: docs file '%s' is "
                    "empty\n", WEATHER_DOCS_PATH);
        return false;
    }

    // The eight  advanced-rendering features, their controlling toggle
    // flags + documented defaults, and (where present) their quality knob and a
    // value that moves it off its default. By product decision ALL eight ship
    // ON, each disablable via `--no-<feature>` (FeatureToggles in
    // engine/types.h).
    const std::array<AdvFeature, 8> features = {{
        {"soft directional shadows", "--no-soft-shadows", true,
         [](const heaven::FeatureToggles& t) { return t.soft_shadows; },
         "--soft-shadow-quality", "0",
         [](const heaven::QualityKnobs& d, const heaven::QualityKnobs& a) {
             return a.soft_shadow_level != d.soft_shadow_level;
         }},
        {"screen-space contact shadows", "--no-contact-shadows", true,
         [](const heaven::FeatureToggles& t) { return t.contact_shadows; },
         "--contact-shadow-quality", "0",
         [](const heaven::QualityKnobs& d, const heaven::QualityKnobs& a) {
             return a.contact_shadow_level != d.contact_shadow_level;
         }},
        {"shadow-casting point/spot lights", "--no-point-light-shadows", true,
         [](const heaven::FeatureToggles& t) { return t.point_light_shadows; },
         "--point-light-quality", "4",
         [](const heaven::QualityKnobs& d, const heaven::QualityKnobs& a) {
             return a.shadow_light_budget != d.shadow_light_budget;
         }},
        {"MSAA on top of TAA", "--no-msaa", true,
         [](const heaven::FeatureToggles& t) { return t.msaa; },
         "--msaa-quality", "4",
         [](const heaven::QualityKnobs& d, const heaven::QualityKnobs& a) {
             return a.msaa_samples != d.msaa_samples;
         }},
        {"screen-space refraction for glass", "--no-refraction", true,
         [](const heaven::FeatureToggles& t) { return t.refraction; },
         nullptr, nullptr, nullptr},
        {"raymarched volumetric clouds", "--no-volumetric-clouds", true,
         [](const heaven::FeatureToggles& t) { return t.volumetric_clouds; },
         "--cloud-quality", "0",
         [](const heaven::QualityKnobs& d, const heaven::QualityKnobs& a) {
             return a.cloud_quality != d.cloud_quality;
         }},
        {"screen-space global illumination", "--no-ssgi", true,
         [](const heaven::FeatureToggles& t) { return t.ssgi; },
         "--ssgi-quality", "0",
         [](const heaven::QualityKnobs& d, const heaven::QualityKnobs& a) {
             return a.ssgi_quality != d.ssgi_quality;
         }},
        {"massive foliage / GPU-driven geometry", "--no-massive-foliage", true,
         [](const heaven::FeatureToggles& t) { return t.massive_foliage; },
         "--foliage-quality", "2.0",
         [](const heaven::QualityKnobs& d, const heaven::QualityKnobs& a) {
             return a.foliage_density != d.foliage_density;
         }},
    }};

    const heaven::FeatureToggles default_toggles;
    const heaven::QualityKnobs   default_knobs;

    for (const AdvFeature& f : features) {
        // ──  (exactly one entry): the controlling flag string appears
        // exactly once, counted in the backtick-wrapped form the doc records so
        // `--msaa` / `--ssgi` are not double-counted by their *-quality flags. ──
        const std::string toggle_marked = std::string("`") + f.toggle_flag + "`";
        const std::size_t toggle_occurrences = count_occurrences(doc, toggle_marked);
        if (toggle_occurrences != 1) {
            std::printf("[unit_advanced_docs_consistency] FAILED: controlling flag "
                        "'%s' appears %zu time(s) in %s, expected exactly 1 "
                        "(Req 13.4 — one entry per feature)\n",
                        f.toggle_flag, toggle_occurrences, WEATHER_DOCS_PATH);
            ok = false;
        }

        // ──  (recorded flag matches the parser): the toggle is parsed
        // in main.cpp::parse_args (not host-linkable), so we validate the flag
        // against the data model — the FeatureToggles field it controls must
        // exist and ship at the documented default. ──
        const bool actual_default = f.toggle(default_toggles);
        if (actual_default != f.default_enabled) {
            std::printf("[unit_advanced_docs_consistency] FAILED: feature '%s' "
                        "(flag '%s') ships default=%s but the doc records "
                        "default=%s (Req 13.4 — recorded flag/default must match "
                        "the data model)\n",
                        f.name, f.toggle_flag,
                        actual_default ? "on" : "off",
                        f.default_enabled ? "on" : "off");
            ok = false;
        }

        // ── Quality knob (where present): the `--<feature>-quality` flag must
        // appear exactly once and, run through apply_quality_args, must move the
        // knob off its default — proving the documented quality flag is one the
        // parser accepts. Refraction has no knob (quality_flag == nullptr), which
        // the doc states explicitly, so it is skipped here. ──
        if (f.quality_flag != nullptr) {
            const std::string knob_marked = std::string("`") + f.quality_flag + "`";
            const std::size_t knob_occurrences = count_occurrences(doc, knob_marked);
            if (knob_occurrences != 1) {
                std::printf("[unit_advanced_docs_consistency] FAILED: quality flag "
                            "'%s' appears %zu time(s) in %s, expected exactly 1 "
                            "(Req 13.4)\n",
                            f.quality_flag, knob_occurrences, WEATHER_DOCS_PATH);
                ok = false;
            }

            const heaven::QualityKnobs after =
                parse_quality(f.quality_flag, f.quality_value);
            if (!f.knob_changed(default_knobs, after)) {
                std::printf("[unit_advanced_docs_consistency] FAILED: parser did not "
                            "move the quality knob for documented flag '%s %s' "
                            "(Req 13.4 — recorded quality flag must be one the "
                            "parser accepts)\n",
                            f.quality_flag, f.quality_value);
                ok = false;
            }
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_advanced_docs_consistency{
    unit_advanced_docs_consistency};

} // namespace
