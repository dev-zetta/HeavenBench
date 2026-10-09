/*
 * HeavenBench — Property test for per-model enable independence
 *
 * Feature: advanced-materials-and-grading,: Per-model enable independence
 *
 *
 * (and the cross-cutting parity) demands that
 * toggling one advanced material model's enable flag is INDEPENDENT of every
 * other model's assignment: disabling a feature reverts only that feature's
 * surfaces to their Shipping_Look model, while leaving every other enabled
 * feature's assignment untouched. The contract lives in the pure CPU mirror
 * heaven::material_model_ref::select_material_model in
 * engine/material_model_ref.h — the single source of truth shared by
 * MaterialInstanceCache::get_or_create and this test (the glass_material_ref.h /
 * prop_glass_identify.cpp precedent).
 *
 * pins per-model enable independence over the whole input space:
 *
 *   For any surface (name + material name + parent chain), any Shipping_Look
 *   model (Standard_Lit / Unlit), any combination of enable flags for the OTHER
 *   models, and any single advanced model M, compare the model selected with
 *   M's flag set true (`before`) against the model selected with M's flag set
 *   false and every other flag held identical (`after`):
 *
 *     - If `before` selected M's model, then `after` reverts that surface to a
 *       NON-M model — specifically the Shipping_Look model it resolves to today
 *       (M's natural-model surfaces fall through, never migrating to another
 *       advanced model). This is the "disabling M reverts only M's surfaces"
 *       half of.
 *
 *     - Otherwise `before` did not select M, and `after` is IDENTICAL to
 *       `before`: flipping M's flag never changes the model chosen for a surface
 *       whose natural model is some other (possibly enabled) model. This is the
 *       "leaves the other features' assignments unchanged" half of  /
 *
 *
 * The generators straddle every marker table (cloth / subsurface / clear-coat /
 * anisotropic) so each natural model — and the no-match Shipping_Look fall-
 * through — is exercised across the run; the per-model enable flags and the
 * flipped model M are drawn arbitrarily so all four flip directions are hit.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <cstddef>
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

using heaven::material_model_ref::EnabledModels;
using heaven::material_model_ref::Material_Model;
using heaven::material_model_ref::select_material_model;

// A purely numeric/underscore alphabet: every advanced marker is alphabetic, so
// a token built solely from these characters can never contain any marker
// substring — it is guaranteed to match no advanced model.
constexpr std::string_view kSafeChars = "0123456789_";

// The four advanced models, indexed in the selector's fixed marker precedence
// (cloth → subsurface → clear-coat → anisotropic). `flag_index` doubles as the
// identity of the model M whose enable flag we flip.
enum AdvancedIndex : int {
    kCloth = 0,
    kSubsurface = 1,
    kClearCoat = 2,
    kAnisotropic = 3,
};

// The Material_Model produced when advanced model `idx` is the natural, enabled
// model of a surface.
Material_Model model_of(int idx) {
    switch (idx) {
        case kCloth:       return Material_Model::Cloth;
        case kSubsurface:  return Material_Model::Subsurface_Translucent;
        case kClearCoat:   return Material_Model::Clear_Coat;
        default:           return Material_Model::Anisotropic;
    }
}

// A marker drawn from advanced model `idx`'s table (wrapping `marker_idx`).
std::string_view advanced_marker(int idx, std::size_t marker_idx) {
    using namespace heaven::material_model_ref;
    switch (idx) {
        case kCloth:
            return k_cloth_markers[marker_idx % std::size(k_cloth_markers)];
        case kSubsurface:
            return k_subsurface_markers[marker_idx % std::size(k_subsurface_markers)];
        case kClearCoat:
            return k_clear_coat_markers[marker_idx % std::size(k_clear_coat_markers)];
        default:
            return k_anisotropic_markers[marker_idx % std::size(k_anisotropic_markers)];
    }
}

// Read/write the enable flag for advanced model `idx` on an EnabledModels.
void set_flag(EnabledModels& en, int idx, bool value) {
    switch (idx) {
        case kCloth:       en.cloth = value; break;
        case kSubsurface:  en.subsurface = value; break;
        case kClearCoat:   en.clear_coat = value; break;
        default:           en.anisotropic = value; break;
    }
}

// A token that provably contains no advanced marker.
rc::Gen<std::string> genMarkerFreeToken() {
    return rc::gen::container<std::string>(rc::gen::elementOf(kSafeChars));
}

// Splice a marker from advanced model `idx`'s table into a marker-free base, so
// the result is guaranteed to carry that model's marker as a substring.
rc::Gen<std::string> genTokenWithMarker(int idx) {
    return rc::gen::apply(
        [idx](std::string base, std::size_t marker_idx, std::size_t pos) {
            const std::string_view marker = advanced_marker(idx, marker_idx);
            const std::size_t at = base.empty() ? 0 : pos % (base.size() + 1);
            base.insert(at, std::string(marker));
            return base;
        },
        genMarkerFreeToken(),
        rc::gen::arbitrary<std::size_t>(),
        rc::gen::arbitrary<std::size_t>());
}

bool run_per_model_enable_independence_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 3: per-model enable independence "
        "(Validates: Requirements 5.5, 14.3)",
        [] {
            // ── Build a surface straddling the marker tables ──
            // The surface name, material name, and (optionally) one parent in
            // the chain each independently either stay marker-free or splice a
            // marker from a randomly chosen advanced table, so every natural
            // model and the no-match fall-through are all reachable.
            const bool name_marked = *rc::gen::arbitrary<bool>();
            const std::string surface_name = name_marked
                ? *genTokenWithMarker(*rc::gen::inRange(0, 4))
                : *genMarkerFreeToken();

            const bool mat_marked = *rc::gen::arbitrary<bool>();
            const std::string material_name = mat_marked
                ? *genTokenWithMarker(*rc::gen::inRange(0, 4))
                : *genMarkerFreeToken();

            std::vector<std::string> parent_chain =
                *rc::gen::container<std::vector<std::string>>(
                    genMarkerFreeToken());
            if (!parent_chain.empty() && *rc::gen::arbitrary<bool>()) {
                const std::size_t idx =
                    *rc::gen::inRange<std::size_t>(0, parent_chain.size());
                parent_chain[idx] = *genTokenWithMarker(*rc::gen::inRange(0, 4));
            }

            // The Shipping_Look model the surface resolves to today.
            const Material_Model shipping_look = *rc::gen::arbitrary<bool>()
                ? Material_Model::Standard_Lit
                : Material_Model::Unlit;

            // Arbitrary enable flags for the OTHER models (held identical
            // between `before` and `after`); the single model M to flip.
            EnabledModels base;
            base.cloth       = *rc::gen::arbitrary<bool>();
            base.subsurface  = *rc::gen::arbitrary<bool>();
            base.clear_coat  = *rc::gen::arbitrary<bool>();
            base.anisotropic = *rc::gen::arbitrary<bool>();

            const int m = *rc::gen::inRange(0, 4);
            const Material_Model m_model = model_of(m);

            // `before`: M enabled. `after`: M disabled, every other flag held
            // exactly as in `base` (so only M's flag differs).
            EnabledModels enabled_before = base;
            set_flag(enabled_before, m, true);
            EnabledModels enabled_after = base;
            set_flag(enabled_after, m, false);

            const Material_Model before = select_material_model(
                surface_name, material_name, parent_chain,
                shipping_look, enabled_before);
            const Material_Model after = select_material_model(
                surface_name, material_name, parent_chain,
                shipping_look, enabled_after);

            if (before == m_model) {
                // Disabling M reverts ONLY M's surfaces, and they fall through
                // to the Shipping_Look model they resolve to today — never to
                // another advanced model.
                RC_ASSERT(after != m_model);
                RC_ASSERT(after == shipping_look);
            } else {
                // A surface whose natural model is not M is wholly unaffected by
                // flipping M's flag: any other enabled model's assignment is
                // left unchanged.
                RC_ASSERT(after == before);
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_per_model_enable_independence{
    run_per_model_enable_independence_property};

} // namespace
