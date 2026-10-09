/*
 * HeavenBench — Property test for the foliage shadow-caster predicate
 *
 * Feature: advanced-materials-and-grading,: Foliage shadow-caster identification is a pure xml0-derived predicate
 *
 *
 * makes the transparent-shadow feature identify foliage casters
 * EXCLUSIVELY from the binary xml0-derived surface/material data — the surface
 * name, the resolved material name, and the `parent="..."` chain — via the
 * existing foliage auto-detection heuristic (surface names containing `leaf`,
 * `leaves`, `branch`, `fir`), with NO text-format mapping file and NO other
 * input (no disk read, no GPU/Filament/SDL state). The contract lives in the
 * header-only, dependency-free predicate in engine/material_model_ref.h (the
 * single source of truth shared by the transparent-shadow caster routing and
 * this test, per the glass-detector precedent):
 *
 *   - is_foliage_shadow_caster(name)                          — name-only overload
 *   - is_foliage_shadow_caster(surface, material, parent_chain) — full overload
 *   - k_foliage_caster_markers = {"leaf","leaves","branch","fir"} — the markers
 *
 * pins the full purity / determinism contract over many inputs:
 *
 *   - Marker-driven classification: a surface name, material name, or any parent
 *     in the chain that embeds a foliage marker (leaf/leaves/branch/fir) as a
 *     substring classifies as a foliage caster; names/chains built only from
 *     marker-free tokens do not. This is the only thing that decides the result.
 *
 *   - Determinism / purity: calling the predicate twice with the same inputs
 *     yields the same answer (no hidden state, no time/RNG/disk influence), and
 *     the name-only overload equals the full overload with an empty material
 *     name and empty chain.
 *
 *   - Input independence: appending marker-free tokens to the parent chain
 *     never changes the result, and the result depends ONLY on the name +
 *     material + parent-chain bytes — nothing else feeds in.
 *
 * The generators straddle the marker set deliberately: a base token is drawn
 * from a marker-free alphabet, then with even odds a real foliage marker is
 * spliced in at a random position (so both the "is foliage" and "is not
 * foliage" branches are exercised every run), and parent chains are built the
 * same way.
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

#include "engine/material_model_ref.h"

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

namespace mm = heaven::material_model_ref;

// A marker-free alphabet: tokens built from these characters can never contain
// the substrings "leaf", "leaves", "branch", or "fir", so a name/chain
// assembled solely from them is guaranteed NOT to be a foliage caster. Every
// letter appearing in any marker (a, b, c, e, f, h, i, l, n, r, s, v) is
// deliberately excluded so no marker can form by accident.
constexpr std::string_view kSafeChars = "dgjkmopqtuwxyz_0123456789";

// Generate a token that provably contains no foliage marker.
rc::Gen<std::string> genMarkerFreeToken() {
    return rc::gen::map(
        rc::gen::container<std::string>(
            rc::gen::elementOf(kSafeChars)),
        [](std::string s) { return s; });
}

// Splice a real foliage marker into an arbitrary position of a marker-free
// base, so the result is guaranteed to contain a marker substring.
rc::Gen<std::string> genTokenWithMarker() {
    return rc::gen::apply(
        [](std::string base, std::size_t marker_idx, std::size_t pos) {
            const std::string_view marker =
                mm::k_foliage_caster_markers[marker_idx %
                    std::size(mm::k_foliage_caster_markers)];
            const std::size_t at = base.empty() ? 0 : pos % (base.size() + 1);
            base.insert(at, std::string(marker));
            return base;
        },
        genMarkerFreeToken(),
        rc::gen::arbitrary<std::size_t>(),
        rc::gen::arbitrary<std::size_t>());
}

bool run_foliage_shadow_caster_purity_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 4: foliage shadow-caster identification is a pure "
        "xml0-derived predicate (Validates: Requirements 6.4)",
        [] {
            // Decide independently whether the surface name and the material
            // name carry a foliage marker, so every case straddles both
            // branches of each overload.
            const bool surface_is_marked = *rc::gen::arbitrary<bool>();
            const std::string surface_name = surface_is_marked
                ? *genTokenWithMarker()
                : *genMarkerFreeToken();

            const bool material_is_marked = *rc::gen::arbitrary<bool>();
            const std::string material_name = material_is_marked
                ? *genTokenWithMarker()
                : *genMarkerFreeToken();

            // Build a parent chain of marker-free tokens, then optionally mark
            // exactly one of them. This keeps "which parents are foliage" under
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
            // membership of the name, material, and parent chain — the only
            // thing that decides the result.
            const bool expected =
                surface_is_marked || material_is_marked || any_parent_marked;
            const bool result = mm::is_foliage_shadow_caster(
                surface_name, material_name, parent_chain);
            RC_ASSERT(result == expected);

            // The name-only overload agrees with how we constructed the
            // surface name (it reads only that single name).
            RC_ASSERT(mm::is_foliage_shadow_caster(surface_name) ==
                      surface_is_marked);

            // ── Determinism / purity ──
            // Same inputs → same output, with no hidden state between calls.
            RC_ASSERT(mm::is_foliage_shadow_caster(
                          surface_name, material_name, parent_chain) == result);
            RC_ASSERT(mm::is_foliage_shadow_caster(surface_name) ==
                      mm::is_foliage_shadow_caster(surface_name));

            // The name-only overload equals the full overload with an empty
            // material name and empty chain: the name is the ONLY input there.
            RC_ASSERT(mm::is_foliage_shadow_caster(surface_name) ==
                      mm::is_foliage_shadow_caster(surface_name, "", {}));

            // ── Input independence ──
            // Appending marker-free tokens to the parent chain never changes
            // the answer (only the name + material + parent-chain bytes feed
            // in, and these additions carry no marker).
            std::vector<std::string> padded = parent_chain;
            const std::vector<std::string> filler =
                *rc::gen::container<std::vector<std::string>>(
                    genMarkerFreeToken());
            padded.insert(padded.end(), filler.begin(), filler.end());
            RC_ASSERT(mm::is_foliage_shadow_caster(
                          surface_name, material_name, padded) == result);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_foliage_shadow_caster_purity{
    run_foliage_shadow_caster_purity_property};

} // namespace
