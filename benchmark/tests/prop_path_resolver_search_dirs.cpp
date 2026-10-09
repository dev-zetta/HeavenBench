// Feature: asset-path-resolution,
/*
 * HeavenBench —: The texture search-directory list is the
 * fixed ordered reference, repeatably.
 *
 *
 *
 * PathResolver::texture_search_dirs() is the single source of the ordered
 * texture auto-discovery search list. This property asserts, for any non-empty
 * Data_Root:
 *
 *   • the list has exactly seven entries (— none added or omitted);
 *   • each entry equals, element-for-element and in order, an INDEPENDENT
 *     reference sequence built by raw concatenation of the Data_Root with the
 *     legacy category fragments in the fixed order
 *     [environment, buildings, ruins, props, airship, sfx, core];
 *   • two invocations for the same Data_Root return byte-for-byte identical
 *     lists (— repeatable, identical across invocations).
 *
 * The reference sequence is built here with literal `+` concatenation and does
 * NOT share code with PathResolver — if it did, the test would degenerate into
 * comparing the resolver to itself. The Data_Root generator deliberately
 * includes trailing-`/` and non-ASCII bytes so the no-normalization guarantee
 * is exercised (the fragments are appended verbatim regardless of separators).
 *
 * This file self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the ">= 100
 * iterations" requirement (raise via RC_PARAMS=max_success=...).
 */
#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <rapidcheck.h>

#include "engine/path_resolver.h"

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

// ── Non-empty Data_Root generator ──

// Build a Data_Root from a small alphabet that mixes ordinary path characters,
// trailing-separator candidates, and non-ASCII bytes, then guarantee it is
// non-empty (PathResolver::create rejects blank roots, which is out of scope
// for this property — that is). Non-ASCII is injected as raw bytes
// so the resolver's verbatim, no-case-change concatenation is exercised.
rc::Gen<std::string> non_empty_data_root() {
    // Segment tokens: plain names, separators, dotted/relative fragments, and
    // a couple of multi-byte UTF-8 sequences (é, 雪) as raw bytes.
    static const std::vector<std::string> tokens = {
        "extracted", "heaven", "data", "assets", "root",
        "/", "//", "..", ".", "-", "_", " ",
        "\xC3\xA9",          // 'é' (U+00E9)
        "\xE9\x9B\xAA",      // '雪' (U+96EA)
    };

    return rc::gen::map(
        rc::gen::container<std::vector<int>>(
            rc::gen::inRange<int>(0, static_cast<int>(tokens.size()))),
        [](const std::vector<int>& picks) {
            std::string root;
            for (int p : picks) root += tokens[static_cast<std::size_t>(p)];
            // Guarantee non-blank: an empty pick list (or one selecting only
            // whitespace tokens, e.g. " ") yields an empty or whitespace-only
            // root, which create() legitimately rejects.
            // That branch is out of scope for this property, which exercises
            // the search-dir list of VALID resolvers; seed a default base when
            // the root has no non-whitespace character.
            if (root.find_first_not_of(" \t\n\v\f\r") == std::string::npos)
                root = "extracted/heaven";
            return root;
        });
}

// ── Independent reference: the seven category fragments, in fixed order ──

// These literals duplicate the legacy inline fragments on purpose. They are NOT
// obtained from PathResolver, so the comparison is a genuine oracle check.
const std::array<std::string, 7>& reference_fragments() {
    static const std::array<std::string, 7> frags = {
        "/heaven/textures/environment/",
        "/heaven/textures/buildings/",
        "/heaven/textures/ruins/",
        "/heaven/textures/props/",
        "/heaven/textures/airship/",
        "/heaven/textures/sfx/",
        "/core/textures/",
    };
    return frags;
}

bool run_p3_search_dirs_property() {
    return rc::check(
        "Feature: asset-path-resolution, Property 3: texture search-directory "
        "list is the fixed ordered reference, repeatably "
        "(Validates: Requirements 1.5, 3.1, 3.2)",
        [] {
            const std::string data_root = *non_empty_data_root();

            const auto resolver_result = heaven::PathResolver::create(data_root);
            // A non-blank Data_Root always constructs successfully.
            RC_ASSERT(resolver_result.has_value());
            const heaven::PathResolver& resolver = *resolver_result;

            // Build the INDEPENDENT reference list by raw concatenation of the
            // Data_Root with each fragment, in the fixed reference order.
            std::array<std::string, 7> expected;
            for (std::size_t i = 0; i < expected.size(); ++i)
                expected[i] = data_root + reference_fragments()[i];

            const std::array<std::string, 7>& dirs = resolver.texture_search_dirs();

            // Exactly seven entries (the type pins the count, asserted for
            // clarity per  — none added or omitted).
            RC_ASSERT(dirs.size() == 7u);

            // Element-for-element equality, in order.
            for (std::size_t i = 0; i < dirs.size(); ++i)
                RC_ASSERT(dirs[i] == expected[i]);

            // Repeatable: a second invocation on the same resolver yields a
            // byte-for-byte identical list in identical order.
            const std::array<std::string, 7>& dirs_again =
                resolver.texture_search_dirs();
            for (std::size_t i = 0; i < dirs_again.size(); ++i)
                RC_ASSERT(dirs_again[i] == dirs[i]);

            // And a freshly-constructed resolver over the same Data_Root yields
            // the same list (identical across invocations for the same root).
            const auto resolver2_result = heaven::PathResolver::create(data_root);
            RC_ASSERT(resolver2_result.has_value());
            const std::array<std::string, 7>& dirs2 =
                resolver2_result->texture_search_dirs();
            for (std::size_t i = 0; i < dirs2.size(); ++i)
                RC_ASSERT(dirs2[i] == expected[i]);
        });
}

const heaven::testing::Register g_register_path_resolver_search_dirs{
    &run_p3_search_dirs_property};

} // namespace
