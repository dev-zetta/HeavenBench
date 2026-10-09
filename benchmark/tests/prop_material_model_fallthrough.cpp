/*
 * HeavenBench — Property test for Shipping_Look fall-through
 *
 * Feature: advanced-materials-and-grading,: Disabled / unmatched surfaces fall through to the Shipping_Look model
 *
 *
 * The Material_Model_Selector (engine/material_model_ref.h) is the single,
 * data-driven source of truth that assigns each Heaven surface exactly one
 * Filament shading model from the xml0-derived surface name, material name, and
 * parent chain (AGENTS.md; the glass/foliage-detector precedent).
 * Group A's parity-when-disabled contract  and
 * the selector's own fall-through guarantees  reduce to a
 * single property over the pure mirror:
 *
 *   The selected model equals the Shipping_Look model the surface resolves to
 *   today (Standard_Lit or Unlit) in EITHER of these situations:
 *
 *     (a) every advanced model is disabled (EnabledModels all false) — no matter
 *         what markers the surface's tokens carry
 * with all features off the output is the Shipping_Look); OR
 *
 *     (b) the surface's tokens match no advanced marker — no matter which
 *         advanced models are enabled (an unmatched surface keeps the
 *         Standard_Lit / Unlit model it resolves to today).
 *
 * The generators straddle both situations every run. A `shipping_look_model` is
 * drawn from {Standard_Lit, Unlit} (the only models a surface resolves to
 * today). A scenario bool then picks branch (a) or (b):
 *
 *   - Branch (a): EnabledModels is all-false and the surface name / material
 *     name / parent chain are built from a marker-free alphabet but with real
 *     advanced markers spliced in at random, proving the all-disabled gate wins
 *     even when every marker table would otherwise match.
 *
 *   - Branch (b): EnabledModels is arbitrary (any combination on) and the
 *     surface name / material name / parent chain are built SOLELY from a
 *     marker-free alphabet, proving an unmatched surface falls through whatever
 *     is enabled.
 *
 * The marker-free alphabet "jkwxy0123456789" is chosen so that NONE of its
 * characters appears in ANY advanced marker (cloth/subsurface/clear-coat/
 * anisotropic), so a string assembled from it can never contain a marker
 * substring — branch (b)'s "no match" precondition holds by construction.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <array>
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

// A marker-free alphabet: NONE of these characters appears in any advanced
// marker table (cloth: flag/banner/fabric/cloth; subsurface: leaf/leaves/
// branch/fir/fabric/grass; clear-coat: dragon/lacquer/varnish/glaze/
// airship_metal; anisotropic: gear/engine/propeller/cannon/brushed). Every
// marker uses only letters from {a,b,c,d,e,f,g,h,i,l,m,n,o,p,q,r,s,t,u,v,z} and
// '_', so a string assembled solely from j/k/w/x/y/digits can never contain a
// marker as a substring.
constexpr std::string_view kSafeChars = "jkwxy0123456789";

// Every advanced marker, flattened across the four tables — the splice source
// for branch (a). Built from the header's tables so the test and the selector
// share the single source of truth.
std::vector<std::string_view> all_advanced_markers() {
    std::vector<std::string_view> out;
    for (const std::string_view m : mm::k_cloth_markers) out.push_back(m);
    for (const std::string_view m : mm::k_subsurface_markers) out.push_back(m);
    for (const std::string_view m : mm::k_clear_coat_markers) out.push_back(m);
    for (const std::string_view m : mm::k_anisotropic_markers) out.push_back(m);
    return out;
}

// Generate a token that provably contains no advanced marker.
rc::Gen<std::string> genMarkerFreeToken() {
    return rc::gen::container<std::string>(rc::gen::elementOf(kSafeChars));
}

// Splice a real advanced marker into a random position of a marker-free base,
// so the result is guaranteed to contain a marker substring (branch (a)).
rc::Gen<std::string> genTokenWithMarker() {
    return rc::gen::apply(
        [](std::string base, std::size_t marker_idx, std::size_t pos) {
            const std::vector<std::string_view> markers = all_advanced_markers();
            const std::string_view marker = markers[marker_idx % markers.size()];
            const std::size_t at = base.empty() ? 0 : pos % (base.size() + 1);
            base.insert(at, std::string(marker));
            return base;
        },
        genMarkerFreeToken(),
        rc::gen::arbitrary<std::size_t>(),
        rc::gen::arbitrary<std::size_t>());
}

bool run_shipping_look_fallthrough_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 2: disabled / unmatched surfaces fall through to the "
        "Shipping_Look model (Validates: Requirements 5.2, 5.4, 1.4, 2.4, "
        "3.4, 4.5)",
        [] {
            // The model the surface resolves to today is one of the two
            // Shipping_Look models; the selector must return exactly this in
            // both fall-through situations.
            const mm::Material_Model shipping_look =
                *rc::gen::element(mm::Material_Model::Standard_Lit,
                                  mm::Material_Model::Unlit);

            // Pick which fall-through situation to exercise this run.
            const bool all_disabled = *rc::gen::arbitrary<bool>();

            mm::EnabledModels enabled;
            std::string surface_name;
            std::string material_name;
            std::vector<std::string> parent_chain;

            if (all_disabled) {
                // Branch (a): every advanced model is OFF. The surface tokens
                // are allowed to carry real markers (spliced with even odds per
                // token) — the all-disabled gate must still win (and
                // the parity contract 1.4 / 2.4 / 3.4 / 4.5).
                enabled = mm::EnabledModels{false, false, false, false};

                auto maybe_marked = [] {
                    return *rc::gen::arbitrary<bool>() ? *genTokenWithMarker()
                                                       : *genMarkerFreeToken();
                };
                surface_name = maybe_marked();
                material_name = maybe_marked();
                std::vector<std::string> chain =
                    *rc::gen::container<std::vector<std::string>>(
                        genMarkerFreeToken());
                for (std::string& p : chain) {
                    if (*rc::gen::arbitrary<bool>()) p = *genTokenWithMarker();
                }
                parent_chain = std::move(chain);
            } else {
                // Branch (b): any combination of advanced models may be ON, but
                // the surface tokens are marker-free by construction, so no
                // advanced rule matches and the surface keeps its Shipping_Look
                // model.
                enabled = mm::EnabledModels{
                    *rc::gen::arbitrary<bool>(), *rc::gen::arbitrary<bool>(),
                    *rc::gen::arbitrary<bool>(), *rc::gen::arbitrary<bool>()};
                surface_name = *genMarkerFreeToken();
                material_name = *genMarkerFreeToken();
                parent_chain =
                    *rc::gen::container<std::vector<std::string>>(
                        genMarkerFreeToken());
            }

            const mm::Material_Model result = mm::select_material_model(
                surface_name, material_name, parent_chain, shipping_look,
                enabled);

            // The core fall-through guarantee: the selector returns the
            // Shipping_Look model in both situations.
            RC_ASSERT(result == shipping_look);

            // Purity / determinism: the same inputs yield the same model, so
            // the fall-through is reproducible (no hidden state).
            RC_ASSERT(mm::select_material_model(surface_name, material_name,
                                                parent_chain, shipping_look,
                                                enabled) == result);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_shipping_look_fallthrough{
    run_shipping_look_fallthrough_property};

} // namespace
