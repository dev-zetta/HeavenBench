/*
 * HeavenBench — unit test: docs/feature-flag consistency
 *
 * — Validates:
 *
 * The rendering-feature checklist (docs/RENDERING_FEATURES.md) is the source of
 * truth for what the renderer provides and how each feature is toggled. This
 * deterministic test pins three guarantees for the two weather features added by
 * these changes (`rain` / `--no-rain` and `wet-reflections` / `--no-wet-reflections`):
 *
 *   •  — the doc records EXACTLY ONE entry per new feature: the
 *     feature's `--no-<feature>` flag string appears exactly once in the file.
 *   •  — the flag string recorded in the doc matches the flag the arg
 *     parser actually accepts. We verify the parser end by running the shared,
 *     GPU-free heaven::apply_weather_args() over {program, "--no-<feature>"} and
 *     confirming it flips that feature's toggle off (and that the toggle ships
 *     enabled by default), so a doc/parser drift in either direction fails.
 *
 * The doc is read at runtime via the WEATHER_DOCS_PATH compile-time macro, an
 * absolute path defined for this target only in benchmark/CMakeLists.txt (ctest's
 * CWD is the build dir, so a relative path would be fragile). If the file cannot
 * be opened the test FAILS with a diagnostic — a missing or moved doc is exactly
 * the kind of drift this test exists to catch.
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

// One row of the doc/parser contract: a feature name, the toggle it controls in
// AppConfig::features, and a reader for that toggle. The controlling flag is
// derived as "--no-<name>" (the `--no-<feature>` convention), which is the
// same string both the doc and the parser are expected to use.
struct FeatureFlag {
    const char* name;
    bool (*toggle)(const heaven::FeatureToggles&);
};

// Parse a single flag through the shared weather-arg parser and report the
// resulting toggle value. argv[0] is a dummy program name, as in a real launch.
bool parse_toggle(const FeatureFlag& f, const std::string& flag) {
    heaven::AppConfig config;
    const char* const argv[] = {"heaven_benchmark", flag.c_str()};
    heaven::apply_cli_arguments(config, argv);
    return f.toggle(config.features);
}

bool unit_docs_consistency() {
    bool ok = true;

    // ── Read the feature checklist via the build-defined absolute path ──
    std::ifstream in(WEATHER_DOCS_PATH, std::ios::binary);
    if (!in) {
        std::printf("[unit_docs_consistency] FAILED: cannot open docs file '%s' "
                    "(R10.1/10.2 — the feature checklist must exist and be "
                    "locatable)\n",
                    WEATHER_DOCS_PATH);
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string doc = buffer.str();

    if (doc.empty()) {
        std::printf("[unit_docs_consistency] FAILED: docs file '%s' is empty\n",
                    WEATHER_DOCS_PATH);
        return false;
    }

    // The two new weather features and the toggles they control. Defaults ship
    // enabled, so each toggle must read true on a fresh config
    // and false after its flag is parsed.
    const std::array<FeatureFlag, 2> features = {{
        {"rain", [](const heaven::FeatureToggles& t) { return t.rain; }},
        {"wet-reflections",
         [](const heaven::FeatureToggles& t) { return t.wet_reflections; }},
    }};

    for (const FeatureFlag& f : features) {
        const std::string flag = std::string("--no-") + f.name;

        // — exactly one entry per feature: the flag string appears
        // exactly once across the whole doc.
        const std::size_t occurrences = count_occurrences(doc, flag);
        if (occurrences != 1) {
            std::printf("[unit_docs_consistency] FAILED: flag '%s' appears %zu "
                        "time(s) in %s, expected exactly 1 (R10.1, R10.2)\n",
                        flag.c_str(), occurrences, WEATHER_DOCS_PATH);
            ok = false;
        }

        // — the recorded flag is one the parser accepts. The toggle ships
        // enabled, and parsing the flag must flip it off; together this proves
        // the doc's `--no-<feature>` string matches the accepted flag.
        const bool default_enabled =
            f.toggle(heaven::AppConfig{}.features);
        if (!default_enabled) {
            std::printf("[unit_docs_consistency] FAILED: feature '%s' is not "
                        "enabled by default; cannot validate its toggle flag\n",
                        f.name);
            ok = false;
        }
        const bool enabled_after_flag = parse_toggle(f, flag);
        if (enabled_after_flag) {
            std::printf("[unit_docs_consistency] FAILED: parser did not disable "
                        "feature '%s' for documented flag '%s' (R10.3 — recorded "
                        "flag must match the accepted flag)\n",
                        f.name, flag.c_str());
            ok = false;
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_docs_consistency{unit_docs_consistency};

} // namespace
