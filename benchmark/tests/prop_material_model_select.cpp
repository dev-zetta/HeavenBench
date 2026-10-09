/*
 * HeavenBench — Property test for material-model selection purity
 *
 * Feature: advanced-materials-and-grading,: Material-model selection is total, pure, and data-driven
 *
 *
 * make the
 * Material_Model_Selector a PURE, TOTAL, DATA-DRIVEN function of the binary
 * xml0-derived surface/material data — the surface name, the resolved material
 * name, and the `parent="..."` chain — plus the per-model `EnabledModels` gate,
 * with NO text-format mapping file and NO other input (no disk read, no
 * GPU/Filament/SDL state, no hidden mutable state). The contract lives in the
 * header-only, dependency-free selector in engine/material_model_ref.h (the
 * single source of truth shared by `MaterialInstanceCache::get_or_create` and
 * this test, per the glass/foliage-detector precedent):
 *
 *   - select_material_model(surface_name, material_name, parent_chain,
 *                           shipping_look_model, enabled) -> Material_Model
 *   - the per-model marker tables (k_cloth/subsurface/clear_coat/anisotropic)
 *
 * pins the full totality / purity / determinism contract over many
 * inputs, for ANY EnabledModels combination:
 *
 *   - Totality: the selector always returns exactly one of the six
 *     closed `Material_Model` enum values — never an out-of-range / undefined
 *     result — for every input, including empty names and empty parent chains.
 *
 *   - Determinism / purity: calling the selector twice with the
 *     same inputs yields the same model (no hidden state, no time/RNG/disk
 *     influence). Interleaving an unrelated call between two identical calls
 *     does not change the answer, so there is no carried-over mutable state.
 *
 *   - Data-driven input independence: the result
 *     depends ONLY on the name + material + parent-chain bytes and the enable
 *     gate. Appending marker-free tokens to the parent chain never changes the
 *     model, and rebuilding the identical input from separate storage yields
 *     the identical model — nothing else (no file, no global) feeds in.
 *
 * The generators straddle the marker set deliberately: tokens are drawn from a
 * marker-free alphabet (so no advanced marker can form by accident), then with
 * even odds a real marker drawn from ANY of the four selection tables is
 * spliced in at a random position — so both the "advanced model" and the
 * "Shipping_Look fall-through" branches are exercised every run — and the
 * EnabledModels gate and shipping_look_model are themselves arbitrary.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
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

// A marker-free alphabet: every Group-A selection marker is built from the
// letters {a,b,c,d,e,f,g,h,i,l,m,n,o,p,q,r,s,t,u,v,w}, so an alphabet drawn
// only from {j,k,x,y,z,_,digits} can never form ANY marker substring
// (cloth/subsurface/clear-coat/anisotropic). A name/chain assembled solely from
// these characters is therefore guaranteed to match no advanced model.
constexpr std::string_view kSafeChars = "jkxyz_0123456789";

// The union of every advanced-model marker, used to splice a guaranteed marker
// into an otherwise marker-free base so the "advanced model" branch fires.
const std::vector<std::string_view>& allMarkers() {
    static const std::vector<std::string_view> markers = [] {
        std::vector<std::string_view> m;
        for (std::string_view s : mm::k_cloth_markers) m.push_back(s);
        for (std::string_view s : mm::k_subsurface_markers) m.push_back(s);
        for (std::string_view s : mm::k_clear_coat_markers) m.push_back(s);
        for (std::string_view s : mm::k_anisotropic_markers) m.push_back(s);
        return m;
    }();
    return markers;
}

// Generate a token that provably contains no advanced-model marker.
rc::Gen<std::string> genMarkerFreeToken() {
    return rc::gen::container<std::string>(rc::gen::elementOf(kSafeChars));
}

// Splice a real marker (from any selection table) into an arbitrary position of
// a marker-free base, so the result is guaranteed to contain a marker.
rc::Gen<std::string> genTokenWithMarker() {
    return rc::gen::apply(
        [](std::string base, std::size_t marker_idx, std::size_t pos) {
            const std::vector<std::string_view>& markers = allMarkers();
            const std::string_view marker = markers[marker_idx % markers.size()];
            const std::size_t at = base.empty() ? 0 : pos % (base.size() + 1);
            base.insert(at, std::string(marker));
            return base;
        },
        genMarkerFreeToken(),
        rc::gen::arbitrary<std::size_t>(),
        rc::gen::arbitrary<std::size_t>());
}

// A token that, with even odds, carries a marker or is marker-free, so each
// generated name straddles both the advanced and fall-through branches.
rc::Gen<std::string> genMaybeMarkedToken() {
    return rc::gen::apply(
        [](bool marked, const std::string& free_tok, const std::string& marked_tok) {
            return marked ? marked_tok : free_tok;
        },
        rc::gen::arbitrary<bool>(),
        genMarkerFreeToken(),
        genTokenWithMarker());
}

// True iff `m` is one of the six closed Material_Model enum values (totality).
bool is_valid_model(mm::Material_Model m) {
    switch (m) {
        case mm::Material_Model::Standard_Lit:
        case mm::Material_Model::Unlit:
        case mm::Material_Model::Clear_Coat:
        case mm::Material_Model::Anisotropic:
        case mm::Material_Model::Subsurface_Translucent:
        case mm::Material_Model::Cloth:
            return true;
    }
    return false;
}

bool run_material_model_selection_purity_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 1: material-model selection is total, pure, and data-driven "
        "(Validates: Requirements 1.3, 2.3, 3.3, 4.3, 5.1, 5.3, 5.6)",
        [] {
            // Arbitrary surface inputs that straddle the marker set.
            const std::string surface_name = *genMaybeMarkedToken();
            const std::string material_name = *genMaybeMarkedToken();
            const std::vector<std::string> parent_chain =
                *rc::gen::container<std::vector<std::string>>(
                    genMaybeMarkedToken());

            // Arbitrary per-model enable gate (every combination is valid).
            const mm::EnabledModels enabled{
                *rc::gen::arbitrary<bool>(),
                *rc::gen::arbitrary<bool>(),
                *rc::gen::arbitrary<bool>(),
                *rc::gen::arbitrary<bool>()};

            // The Shipping_Look model a surface resolves to today is either
            // Standard_Lit or Unlit; both are valid fall-through targets.
            const mm::Material_Model shipping_look =
                *rc::gen::arbitrary<bool>() ? mm::Material_Model::Standard_Lit
                                            : mm::Material_Model::Unlit;

            const mm::Material_Model result = mm::select_material_model(
                surface_name, material_name, parent_chain, shipping_look,
                enabled);

            // ── Totality  ──
            // Always exactly one of the six closed enum values, for any input.
            RC_ASSERT(is_valid_model(result));

            // ── Determinism / purity  ──
            // Identical inputs → identical output, with no hidden state.
            RC_ASSERT(mm::select_material_model(surface_name, material_name,
                                                parent_chain, shipping_look,
                                                enabled) == result);

            // Interleaving an unrelated selection call between two identical
            // calls must not perturb the answer — proves no carried-over state.
            const auto decoy_result = mm::select_material_model(
                "jkx_decoy", "zzy_decoy", {}, mm::Material_Model::Unlit,
                mm::EnabledModels{true, true, true, true});
            RC_ASSERT(is_valid_model(decoy_result));
            RC_ASSERT(mm::select_material_model(surface_name, material_name,
                                                parent_chain, shipping_look,
                                                enabled) == result);

            // ── Data-driven input independence  ──
            // Rebuilding the identical input from separate storage yields the
            // identical model: only the bytes feed in, nothing global.
            const std::string surface_copy(surface_name.begin(),
                                           surface_name.end());
            const std::string material_copy(material_name.begin(),
                                            material_name.end());
            const std::vector<std::string> parent_copy(parent_chain.begin(),
                                                       parent_chain.end());
            RC_ASSERT(mm::select_material_model(surface_copy, material_copy,
                                                parent_copy, shipping_look,
                                                enabled) == result);

            // Appending marker-free tokens to the parent chain never changes
            // the model (only the name + parent-chain bytes feed in, and these
            // additions carry no marker).
            std::vector<std::string> padded = parent_chain;
            const std::vector<std::string> filler =
                *rc::gen::container<std::vector<std::string>>(
                    genMarkerFreeToken());
            padded.insert(padded.end(), filler.begin(), filler.end());
            RC_ASSERT(mm::select_material_model(surface_name, material_name,
                                                padded, shipping_look,
                                                enabled) == result);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_material_model_selection_purity{
    run_material_model_selection_purity_property};

} // namespace
