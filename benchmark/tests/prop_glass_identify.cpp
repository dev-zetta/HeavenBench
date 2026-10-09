/*
 * HeavenBench — Property test for glass identification purity
 *
 * Feature: advanced-rendering-features,: Glass identification depends only on xml0-derived material data
 *
 *
 * makes glass identification a PURE function of the binary
 * xml0-derived surface/material data — the material name and its `parent="..."`
 * chain — with NO text-format mapping file and NO other input (no disk read, no
 * GPU/Filament/SDL state). The contract lives in the header-only, dependency-
 * free predicate in engine/glass_material_ref.h (the single source of truth
 * shared by the screen-space-refraction feature and this test, per the
 * foliage-detector precedent):
 *
 *   - name_has_glass_marker(name)                         — substring marker test
 *   - is_glass_material(name)                             — name-only overload
 *   - is_glass_material(name, parent_chain)               — name + parent chain
 *   - k_glass_markers = {"glass"}                          — the xml0 marker
 *
 * pins the full purity / determinism contract over many inputs:
 *
 *   - Marker-driven classification: a name (or any parent in the chain) that
 *     embeds a glass marker ("glass") as a substring classifies as
 *     glass; a name/chain built only from marker-free tokens does not. This is
 *     the only thing that decides the result.
 *
 *   - Determinism / purity: calling the predicate twice with the same inputs
 *     yields the same answer (no hidden state, no time/RNG/disk influence), and
 *     the name-only overload equals the parent-chain overload with an empty
 *     chain.
 *
 *   - Input independence: appending marker-free tokens to the parent chain
 *     never changes the result, and the result depends ONLY on the name +
 *     parent-chain bytes — nothing else feeds in.
 *
 * The generators straddle the marker set deliberately: a base token is drawn
 * from a marker-free alphabet, then with even odds a real glass marker is
 * spliced in at a random position (so both the "is glass" and "is not glass"
 * branches are exercised every run), and parent chains are built the same way.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidcheck.h>

#include "engine/glass_material_ref.h"

// The registry lives in test_main.cpp (external linkage, no shared header), so
// mirror its minimal interface here to append our test. The struct definition
// is token-identical to the one in test_main.cpp, satisfying the ODR.
namespace heaven::testing {

using TestFn = std::function<bool()>;

std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// A marker-free alphabet: tokens built from these characters can never contain
// the substring "glass", so a name/chain assembled solely from
// them is guaranteed NOT to be glass. (The letters g/l/a/s are
// deliberately excluded so no marker can form by accident.)
constexpr std::string_view kSafeChars = "bcfhjkmpqrtuvxyz_0123456789";

// Generate a token that provably contains no glass marker.
rc::Gen<std::string> genMarkerFreeToken() {
    return rc::gen::map(
        rc::gen::container<std::string>(
            rc::gen::elementOf(kSafeChars)),
        [](std::string s) { return s; });
}

// Splice a real glass marker into an arbitrary position of a marker-free base,
// so the result is guaranteed to contain a marker substring.
rc::Gen<std::string> genTokenWithMarker() {
    return rc::gen::apply(
        [](std::string base, std::size_t marker_idx, std::size_t pos) {
            const std::string_view marker =
                heaven::glass_ref::k_glass_markers[marker_idx %
                    std::size(heaven::glass_ref::k_glass_markers)];
            const std::size_t at = base.empty() ? 0 : pos % (base.size() + 1);
            base.insert(at, std::string(marker));
            return base;
        },
        genMarkerFreeToken(),
        rc::gen::arbitrary<std::size_t>(),
        rc::gen::arbitrary<std::size_t>());
}

bool run_glass_identification_purity_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 7: glass identification depends only on xml0-derived "
        "material data (Validates: Requirements 5.3)",
        [] {
            // Decide independently whether the material name and each parent
            // carries a glass marker, so every case straddles both branches.
            const bool name_is_marked = *rc::gen::arbitrary<bool>();
            const std::string name = name_is_marked
                ? *genTokenWithMarker()
                : *genMarkerFreeToken();

            // Build a parent chain of marker-free tokens, then optionally mark
            // exactly one of them. This keeps "which parents are glass" under
            // precise control so the expected answer is computable.
            std::vector<std::string> parent_chain =
                *rc::gen::container<std::vector<std::string>>(
                    genMarkerFreeToken());
            const bool any_parent_marked =
                !parent_chain.empty() && *rc::gen::arbitrary<bool>();
            if (any_parent_marked) {
                const std::size_t idx =
                    *rc::gen::inRange<std::size_t>(0, parent_chain.size());
                parent_chain[idx] = *genTokenWithMarker();
            }

            // ── Marker-driven classification  ──
            // The expected result is computable purely from the marker
            // membership of the name and the parent chain — nothing else.
            const bool expected = name_is_marked || any_parent_marked;
            const bool result =
                heaven::glass_ref::is_glass_material(name, parent_chain);
            RC_ASSERT(result == expected);

            // name_has_glass_marker agrees with how we constructed the name.
            RC_ASSERT(heaven::glass_ref::name_has_glass_marker(name) ==
                      name_is_marked);

            // ── Determinism / purity ──
            // Same inputs → same output, with no hidden state between calls.
            RC_ASSERT(heaven::glass_ref::is_glass_material(name, parent_chain) ==
                      result);
            RC_ASSERT(heaven::glass_ref::name_has_glass_marker(name) ==
                      heaven::glass_ref::name_has_glass_marker(name));

            // The name-only overload equals the chain overload with an empty
            // chain: the parent chain is the ONLY additional input.
            RC_ASSERT(heaven::glass_ref::is_glass_material(name) ==
                      heaven::glass_ref::is_glass_material(name, {}));
            RC_ASSERT(heaven::glass_ref::is_glass_material(name) ==
                      name_is_marked);

            // ── Input independence ──
            // Appending marker-free tokens to the parent chain never changes
            // the answer (only the name + parent-chain bytes feed in, and these
            // additions carry no marker).
            std::vector<std::string> padded = parent_chain;
            const std::vector<std::string> filler =
                *rc::gen::container<std::vector<std::string>>(
                    genMarkerFreeToken());
            padded.insert(padded.end(), filler.begin(), filler.end());
            RC_ASSERT(heaven::glass_ref::is_glass_material(name, padded) ==
                      result);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_glass_identification_purity{
    run_glass_identification_purity_property};

} // namespace
