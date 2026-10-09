/*
 * HeavenBench — unit test: GPU instancing/skinning docs/feature-flag
 * consistency
 *
 * — Validates:
 *
 * The rendering-feature checklist (docs/RENDERING_FEATURES.md) is the source of
 * truth for what the renderer provides and how each capability is toggled.
 * of that doc records the two features added by these changes: GPU
 * instancing (`--no-gpu-instancing`) and GPU skinning (`--no-gpu-skinning`).
 * This deterministic test pins, for each feature, the guarantees
 * demands:
 *
 *   • EXACTLY ONE entry per feature — the controlling command-line flag string
 *     appears exactly once across the whole doc (one entry per feature).
 *   • The recorded flag matches a flag the arg parser actually accepts. The two
 *     `--no-gpu-*` flags are table-driven in engine/cli_parser.cpp's
 *     flag_table(), which IS host-linkable (cli_parser.cpp is in the
 *     heaven_benchmark_tests source list), so we assert each documented flag
 *     against heaven::flag_table() directly — the parser's own single source of
 *     truth: a FlagSpec with that exact name must exist with Arity::None (a
 *     disable toggle that consumes no value).
 *   • The default ships ON — the doc records both features as "on by default",
 *     so the default-constructed FeatureToggles field the flag controls
 *     (`gpu_instancing` / `gpu_skinning`) must read true. A drift in either the
 *     flag name, its arity, or its default fails the test.
 *
 * This mirrors unit_phase8_docs_consistency.cpp, which validates the
 * flags against flag_table() the same way.
 *
 * Flags are counted in the EXACT backtick-wrapped form the doc records
 * (`--flag`), as the sibling docs tests do, so a bare-substring match elsewhere
 * in the prose cannot inflate the count.
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
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
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

// One row of the doc/parser contract for a feature.

//   name   — human label, used only in diagnostics.
//   flag   — the controlling `--no-<feature>` flag string the doc records and
//            that flag_table() must accept as Arity::None. Counted in
//            the backtick-wrapped form so prose substrings are not counted.
//   toggle — reader for the FeatureToggles bool the flag disables; a fresh
//            FeatureToggles must read this true (the doc records both ON by
//            default).
struct GpuFeature {
    const char* name;
    const char* flag;
    bool (*toggle)(const heaven::FeatureToggles&);
};

bool unit_gpu_features_docs_consistency() {
    bool ok = true;

    // ── Read the feature checklist via the build-defined absolute path ──
    std::ifstream in(WEATHER_DOCS_PATH, std::ios::binary);
    if (!in) {
        std::printf("[unit_gpu_features_docs_consistency] FAILED: cannot open docs "
                    "file '%s' (Req 8.4 — the feature checklist must exist and be "
                    "locatable)\n",
                    WEATHER_DOCS_PATH);
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string doc = buffer.str();

    if (doc.empty()) {
        std::printf("[unit_gpu_features_docs_consistency] FAILED: docs file '%s' is "
                    "empty\n", WEATHER_DOCS_PATH);
        return false;
    }

    // The two  features, their controlling disable flags, and the
    // FeatureToggles bool each one flips off. Both ship ON, so each
    // toggle must read true on a fresh config.
    const std::array<GpuFeature, 2> features = {{
        {"GPU instancing", "--no-gpu-instancing",
         [](const heaven::FeatureToggles& t) { return t.gpu_instancing; }},
        {"GPU skinning", "--no-gpu-skinning",
         [](const heaven::FeatureToggles& t) { return t.gpu_skinning; }},
    }};

    const heaven::FeatureToggles default_toggles;

    for (const GpuFeature& f : features) {
        // ──  (exactly one entry): the controlling flag string appears
        // exactly once, counted in the backtick-wrapped form the doc records. ──
        const std::string flag_marked = std::string("`") + f.flag + "`";
        const std::size_t flag_occurrences = count_occurrences(doc, flag_marked);
        if (flag_occurrences != 1) {
            std::printf("[unit_gpu_features_docs_consistency] FAILED: controlling "
                        "flag '%s' appears %zu time(s) in %s, expected exactly 1 "
                        "(Req 8.4 — one entry per feature)\n",
                        f.flag, flag_occurrences, WEATHER_DOCS_PATH);
            ok = false;
        }

        // ──  (recorded flag matches the parser): the flag MUST exist in
        // flag_table() — the parser's own single source of truth — as a
        // value-less disable toggle (Arity::None). ──
        const heaven::FlagSpec* spec = find_flag(f.flag);
        if (spec == nullptr) {
            std::printf("[unit_gpu_features_docs_consistency] FAILED: documented "
                        "flag '%s' is not accepted by the parser (absent from "
                        "heaven::flag_table()) (Req 8.4)\n",
                        f.flag);
            ok = false;
        } else if (spec->arity != heaven::Arity::None) {
            std::printf("[unit_gpu_features_docs_consistency] FAILED: flag '%s' has "
                        "arity Value in flag_table() but must be None — it is a "
                        "disable toggle that consumes no value (Req 8.4)\n",
                        f.flag);
            ok = false;
        }

        // ── Default ON: the doc records both features "on by default", so the
        // FeatureToggles field the flag disables must read true on a fresh
        // config. This backs the doc's default annotation and proves the flag
        // controls the field it claims to. ──
        if (!f.toggle(default_toggles)) {
            std::printf("[unit_gpu_features_docs_consistency] FAILED: feature '%s' "
                        "(flag '%s') does not ship ON by default but the doc "
                        "records it on by default (Req 8.4)\n",
                        f.name, f.flag);
            ok = false;
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_gpu_features_docs_consistency{
    unit_gpu_features_docs_consistency};

} // namespace
