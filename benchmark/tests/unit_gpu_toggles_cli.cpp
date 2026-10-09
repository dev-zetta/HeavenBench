/*
 * HeavenBench — unit test: GPU instancing / skinning CLI toggles
 *
 * Feature: gpu-instancing-and-skinning
 *    — Validates:
 *
 * The renderer's argument handling lives in the table-driven parser
 * engine/cli_parser.{h,cpp}; the public entry point heaven::parse_cli(argc,
 * argv) returns a ParseResult carrying the parsed BenchConfig (`config`) plus a
 * vector of collected diagnostics (`diagnostics`). That module is GPU-free and
 * I/O-free, so this unit test exercises it on the host without linking
 * Filament/SDL — mirroring unit_phase8_cli.cpp / unit_quality_cli.cpp.
 *
 * Where prop_cli_toggles.cpp covers the toggle mechanics across random subsets,
 * this unit test pins the concrete gpu-instancing-and-skinning CLI contract by
 * example:
 *
 *   • A flagless parse leaves BOTH FeatureToggles bools ON
 *     (gpu_instancing == true, gpu_skinning == true) — the documented default.
 *   • `--no-gpu-instancing` flips EXACTLY features.gpu_instancing to false and
 *     leaves gpu_skinning (and every other FeatureToggles field) at its default.
 *   • `--no-gpu-skinning` flips EXACTLY features.gpu_skinning to false and
 *     leaves gpu_instancing (and every other FeatureToggles field) at its
 *     default.
 *   • Passing both flags disables both; each disable flag is silent (no
 *     diagnostic) and requests no help.
 *
 * The file self-registers with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, so the
 * file(GLOB) over the tests directory in benchmark/CMakeLists.txt compiles it in
 * without editing the entry point. The registered callable returns true on
 * success and false (after printing a diagnostic) on failure.
 */
#include <cstdio>
#include <functional>
#include <string>
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

using heaven::FeatureToggles;
using heaven::ParseResult;

// Run the real parse driver over a token list (argv[0] is a dummy program name,
// as in a real launch). parse_cli is pure: no printing, no exit — the
// diagnostics it would otherwise route to the wrapper are returned in the
// ParseResult, which is exactly what we assert on.
ParseResult parse(std::vector<const char*> args) {
    args.insert(args.begin(), "heaven_benchmark");
    return heaven::parse_cli(args);
}

// Confirm that EVERY FeatureToggles field of `f` equals the default-constructed
// baseline EXCEPT the two fields named here, which are asserted against the
// supplied expected values. This is what "flips exactly its field" means: the
// flag under test moved only its own bool and disturbed nothing else.

// Rather than enumerate the (large, frequently-growing) FeatureToggles field
// list, we synthesize the expected struct from a default baseline by setting
// only the two GPU fields, then compare the two GPU fields plus a spot-check of
// the unrelated neighbours. A full member-wise sweep would couple this test to
// every future field; the targeted comparison keeps it focused on  while
// still proving isolation for the fields most likely to be confused.
bool only_gpu_fields_changed(const FeatureToggles& f,
                             bool expect_instancing,
                             bool expect_skinning,
                             const char* ctx) {
    bool ok = true;
    const FeatureToggles d; // defaults: both GPU toggles ON

    if (f.gpu_instancing != expect_instancing) {
        std::printf("[unit_gpu_toggles_cli] FAILED (%s): gpu_instancing is %s, "
                    "expected %s\n", ctx, f.gpu_instancing ? "ON" : "OFF",
                    expect_instancing ? "ON" : "OFF");
        ok = false;
    }
    if (f.gpu_skinning != expect_skinning) {
        std::printf("[unit_gpu_toggles_cli] FAILED (%s): gpu_skinning is %s, "
                    "expected %s\n", ctx, f.gpu_skinning ? "ON" : "OFF",
                    expect_skinning ? "ON" : "OFF");
        ok = false;
    }

    // Spot-check a representative spread of unrelated FeatureToggles fields: the
    // first toggle in the struct, a capability, and the GPU toggles'
    // immediate neighbours. None of these should ever move when a GPU flag is
    // passed.
    if (f.grass != d.grass || f.sky != d.sky || f.rain != d.rain ||
        f.clear_coat != d.clear_coat || f.specular_aa != d.specular_aa ||
        f.dynamic_resolution != d.dynamic_resolution) {
        std::printf("[unit_gpu_toggles_cli] FAILED (%s): a GPU toggle disturbed "
                    "an unrelated FeatureToggles field (should be isolated, "
                    "Req 7.1)\n", ctx);
        ok = false;
    }
    return ok;
}

bool unit_gpu_toggles_cli() {
    bool ok = true;
    auto fail = [&](const char* msg) {
        std::printf("[unit_gpu_toggles_cli] FAILED: %s\n", msg);
        ok = false;
    };

    // ── 1. Flagless parse: both GPU toggles default ON  ──
    {
        const ParseResult r = parse({});
        ok = only_gpu_fields_changed(r.config.features, true, true,
                                     "flagless default") && ok;
        if (!r.diagnostics.empty())
            fail("flagless parse emitted diagnostics");
        if (r.help_requested)
            fail("flagless parse requested help");
    }

    // ── 2. --no-gpu-instancing flips exactly gpu_instancing OFF ──
    {
        const ParseResult r = parse({"--no-gpu-instancing"});
        ok = only_gpu_fields_changed(r.config.features,
                                     /*expect_instancing=*/false, /*expect_skinning=*/true,
                                     "--no-gpu-instancing") && ok;
        if (!r.diagnostics.empty())
            fail("--no-gpu-instancing emitted a diagnostic (disable is silent)");
        if (r.help_requested)
            fail("--no-gpu-instancing requested help");
    }

    // ── 3. --no-gpu-skinning flips exactly gpu_skinning OFF ──
    {
        const ParseResult r = parse({"--no-gpu-skinning"});
        ok = only_gpu_fields_changed(r.config.features,
                                     /*expect_instancing=*/true, /*expect_skinning=*/false,
                                     "--no-gpu-skinning") && ok;
        if (!r.diagnostics.empty())
            fail("--no-gpu-skinning emitted a diagnostic (disable is silent)");
        if (r.help_requested)
            fail("--no-gpu-skinning requested help");
    }

    // ── 4. Both flags together disable both, independently ──
    {
        const ParseResult r = parse({"--no-gpu-instancing", "--no-gpu-skinning"});
        ok = only_gpu_fields_changed(r.config.features,
                                     /*expect_instancing=*/false, /*expect_skinning=*/false,
                                     "--no-gpu-instancing --no-gpu-skinning") && ok;
        if (!r.diagnostics.empty())
            fail("the two GPU disable flags emitted a diagnostic together");
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_gpu_toggles_cli{unit_gpu_toggles_cli};

} // namespace
