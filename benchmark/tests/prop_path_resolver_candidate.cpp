/*
 * HeavenBench —: Auto-discovery candidate paths compose
 * deterministically.
 *
 * Feature: asset-path-resolution,
 *
 *
 *
 * Convention-based texture auto-discovery in the material database forms a
 * candidate path for each (search directory, base name, suffix) triple as the
 * verbatim concatenation `search_dir + base + suffix + ".dds"` — no separator
 * is inserted because the resolved search directory already ends in "/"
 * (PathResolver::texture_candidate in src/engine/path_resolver.{h,cpp}). This
 * property checks two facets across many inputs:
 *
 *   Composition: for any resolved search directory, any base name, and
 *     any suffix, texture_candidate returns exactly the concatenation of the
 *     search directory, the base name, the suffix, and the literal ".dds",
 *     matching an independent reference implementation that does the raw `+`.
 *   Determinism / repeatability: calling texture_candidate twice with
 *     identical inputs yields a byte-for-byte identical Resolved_Path.
 *
 * The search directory is drawn both from a real PathResolver's
 * texture_search_dirs() (one of the seven resolved entries — the real-usage
 * path) and from arbitrary strings, and the suffix is drawn both from
 * texture_suffixes() and from arbitrary strings, so the property exercises the
 * concrete auto-discovery inputs as well as the general string contract.
 *
 * This file self-registers with the RapidCheck harness in test_main.cpp via
 * heaven::testing::Register, exactly like the other benchmark test TUs.
 */
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <rapidcheck.h>

#include "engine/path_resolver.h"

namespace heaven::testing {

// Mirror of the registry hooks defined in test_main.cpp. registry() is defined
// there (external linkage); Register is an identical inline definition, which
// ODR permits across translation units. Each test TU appends a callable
// returning true on success.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Independent reference implementation of the candidate-composition rule. This
// performs the exact raw `+` concatenation the legacy auto-discovery loop uses
// (`dir + name + suffix + ".dds"`) and deliberately shares no code with
// PathResolver, so the property does not degenerate into comparing the resolver
// against itself.
std::string reference_candidate(const std::string& search_dir,
                                const std::string& base,
                                const std::string& suffix) {
    return search_dir + base + suffix + ".dds";
}

// Generate a Data_Root with at least one non-whitespace character so that
// PathResolver::create always succeeds (construction validation is;
// here we only need a valid resolver to source real search directories). The
// set still includes trailing-"/" and embedded "." / ".." forms so the resolved
// search directories cover realistic and pathological roots.
rc::Gen<std::string> data_root_gen() {
    return rc::gen::element<std::string>(
        std::string("extracted/heaven"),
        std::string("/abs/root"),
        std::string("data/"),
        std::string("data//"),
        std::string("./rel/.."),
        std::string("x"));
}

// Returns true when every RapidCheck case
// passes. Two checks of 100 cases each (RapidCheck's default maxSuccess) run
// the property over >= 100 iterations: one drawing the search dir / suffix from
// the real resolver enumerations (the auto-discovery usage path) and one from
// arbitrary strings (the general string contract).
bool property_4_candidate_composition() {
    // Arm A: real-usage inputs — search dir drawn from texture_search_dirs(),
    // suffix drawn from texture_suffixes(), base name arbitrary.
    bool ok = rc::check(
        "Property 4: candidate == search_dir + base + suffix + \".dds\" "
        "(resolver-sourced dir/suffix), and is repeatable",
        [] {
            const auto resolver = heaven::PathResolver::create(*data_root_gen());
            RC_ASSERT(resolver.has_value());

            const auto& dirs = resolver->texture_search_dirs();
            const auto suffixes = heaven::PathResolver::texture_suffixes();

            const std::size_t dir_idx =
                *rc::gen::inRange<std::size_t>(0, dirs.size());
            const std::size_t suf_idx =
                *rc::gen::inRange<std::size_t>(0, suffixes.size());
            const std::string base = *rc::gen::arbitrary<std::string>();

            const std::string& search_dir = dirs[dir_idx];
            const std::string suffix(suffixes[suf_idx]);

            const std::string got =
                resolver->texture_candidate(search_dir, base, suffix);

            // Composition: equals the independent raw concatenation.
            RC_ASSERT(got == reference_candidate(search_dir, base, suffix));

            // Determinism / repeatability: a second identical call
            // returns a byte-for-byte identical string.
            const std::string again =
                resolver->texture_candidate(search_dir, base, suffix);
            RC_ASSERT(again == got);
        });

    // Arm B: arbitrary search dir, base, and suffix strings — the general
    // string contract, independent of the resolver's enumerations.
    ok = rc::check(
             "Property 4: candidate == search_dir + base + suffix + \".dds\" "
             "(arbitrary inputs), and is repeatable",
             [] {
                 const auto resolver =
                     heaven::PathResolver::create(*data_root_gen());
                 RC_ASSERT(resolver.has_value());

                 const std::string search_dir = *rc::gen::arbitrary<std::string>();
                 const std::string base = *rc::gen::arbitrary<std::string>();
                 const std::string suffix = *rc::gen::arbitrary<std::string>();

                 const std::string got =
                     resolver->texture_candidate(search_dir, base, suffix);

                 RC_ASSERT(got == reference_candidate(search_dir, base, suffix));

                 const std::string again =
                     resolver->texture_candidate(search_dir, base, suffix);
                 RC_ASSERT(again == got);
             }) &&
         ok;

    return ok;
}

// Register the property at static-init time.
const heaven::testing::Register reg_property_4{&property_4_candidate_composition};

} // namespace
