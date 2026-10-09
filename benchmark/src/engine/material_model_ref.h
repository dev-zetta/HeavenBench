/*
 * HeavenBench — Material_Model_Selector (pure CPU mirror)
 *
 * Header-only, dependency-free, pure free functions that resolve which Filament
 * shading model (Material_Model) a Heaven surface renders with, purely from the
 * xml0-derived surface name, material name, and parent chain — the same binary
 * Material_Data the foliage and glass detectors already key off (AGENTS.md
 * "Materials" / `glass_material_ref.h`). This is the heart of Group A (advanced
 * material shading models) and is the single source of truth shared by
 * `MaterialInstanceCache::get_or_create` and the RapidCheck property tests in
 * `heaven_benchmark_tests` (Properties 1–4).
 *
 * Hard rule (and the AGENTS.md "no material_map.txt" rule):
 * material-model selection is driven EXCLUSIVELY from the binary xml0 surface /
 * material data (the surface name, the resolved material name, and its
 * `parent="..."` chain) plus the documented surface-name heuristics. There is
 * NO text-format mapping file, and nothing here reads from disk or touches any
 * GPU/Filament/SDL state. Every function is deterministic and side-effect free
 * so the property tests can hammer the whole input space without a GPU
 *
 *
 * Selection is total (exactly one model per surface), deterministic
 * (identical input → identical output), and falls through to the
 * Shipping_Look model the surface resolves to today on no advanced match or
 * when the matched model is disabled.
 *
 * The marker tables below come from the actual Heaven material naming observed
 * in the assembled scene (the `assembled`.mtl files, the extracted
 * `extracted/heaven`.mat parent chains, and the.node surface bindings):
 *
 *   - Clear coat (lacquered / glazed / coated metal): the dragon statue
 *     (`dragon_dragon_*`), authored lacquer/varnish/glaze tokens, and the
 *     airship metal (`airship_metal1/2/3`).
 *   - Anisotropic (machined / brushed metal): gears (`crane_gears`,
 *     `crane_gear_wood`), the airship engine (`airship_engine_engine`), the
 *     propeller (`port_port_propeller`), the cannons (`*_cannon*`), and brushed
 *     tokens.
 *   - Subsurface / translucency (thin organic + fabric): foliage tokens
 *     (`leaf`, `leaves`, `branch`, `fir`), thin-fabric and grass-tip tokens.
 *   - Cloth / sheen (textile): flags, banners, and fabric/cloth tokens.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_MATERIAL_MODEL_REF_H_
#define HEAVENBENCH_SRC_ENGINE_MATERIAL_MODEL_REF_H_

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace heaven::material_model_ref {

// The closed set of Filament shading models a surface can resolve to. Each
// surface maps to exactly one. `Standard_Lit` / `Unlit` are the
// Shipping_Look models a surface resolves to today; the remaining four are the
// Group-A advanced models gated by `EnabledModels`.
enum class Material_Model {
    Standard_Lit,
    Unlit,
    Clear_Coat,
    Anisotropic,
    Subsurface_Translucent,
    Cloth,
};

// Per-model enable gate carried in from `AppConfig::features`. With every field
// false, the selector only ever returns the Shipping_Look model; each
// field independently gates whether its model may be returned.
struct EnabledModels {
    bool clear_coat  = false;
    bool anisotropic = false;
    bool subsurface  = false;
    bool cloth       = false;
};

// xml0-derived markers per advanced model — kept as the single source of truth
// so the selector and its property tests agree, mirroring
// `glass_ref::k_glass_markers`. A surface matches a model when its surface
// name, its resolved material name, or any name in its parent chain contains
// one of these tokens as a substring (the same "name contains token" discipline
// the foliage/glass detectors use).

// Clear coat: the dragon statue, authored lacquer/varnish/glaze surfaces, and
// the airship metal. `glaze` also covers `glazed` / `glazed_tile`; the
// `airship_metal` token matches `airship_metal1/2/3`.
inline constexpr std::string_view k_clear_coat_markers[] = {
    "dragon",
    "lacquer",
    "varnish",
    "glaze",
    "airship_metal",
};

// Anisotropic: machined / brushed metal — gears, engine, propeller, cannons.
inline constexpr std::string_view k_anisotropic_markers[] = {
    "gear",
    "engine",
    "propeller",
    "cannon",
    "brushed",
};

// Subsurface / translucency: thin organic foliage and grass tips, plus thin
// fabric. `fabric` is shared with the cloth table; cloth wins by precedence
// (see `select_material_model`), so `fabric` only resolves here when cloth is
// not the surface's natural model.
inline constexpr std::string_view k_subsurface_markers[] = {
    "leaf",
    "leaves",
    "branch",
    "fir",
    "fabric",
    "grass",
};

// Cloth / sheen: textile surfaces — flags, banners, and fabric/cloth tokens
// (including the PBD-simulated flag surfaces, whose simulation is untouched —
// only shading changes).
inline constexpr std::string_view k_cloth_markers[] = {
    "flag",
    "banner",
    "fabric",
    "cloth",
};

// Foliage shadow-caster markers: the exact set the renderer's foliage
// auto-detection already uses (surface names containing leaf/leaves/branch/fir).
inline constexpr std::string_view k_foliage_caster_markers[] = {
    "leaf",
    "leaves",
    "branch",
    "fir",
};

// True when `name` contains any token in `markers` as a substring.
[[nodiscard]] inline bool name_has_marker(
        std::string_view name,
        const std::string_view* markers,
        std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (name.find(markers[i]) != std::string_view::npos) return true;
    }
    return false;
}

// True when `surface_name`, `material_name`, or ANY name in `parent_chain`
// contains a marker. This is the same name+parent-chain discipline
// `glass_ref::is_glass_material` uses: leaf materials inherit their model
// through their `parent="..."` ancestry, so a renamed leaf is still caught.
[[nodiscard]] inline bool any_name_has_marker(
        std::string_view surface_name,
        std::string_view material_name,
        const std::vector<std::string>& parent_chain,
        const std::string_view* markers,
        std::size_t count) {
    if (name_has_marker(surface_name, markers, count)) return true;
    if (name_has_marker(material_name, markers, count)) return true;
    return std::ranges::any_of(parent_chain, [&](const std::string& parent) {
        return name_has_marker(parent, markers, count);
    });
}

// Pure material-model selection.

// Resolution proceeds in two steps so the result is total, deterministic, and
// per-model independent:

//   1. The surface's NATURAL advanced model is found by a fixed marker
//      precedence — cloth → subsurface → clear-coat → anisotropic — taking the
//      first model whose marker table matches. Precedence is over the marker
//      tables only, NOT over the enable flags, so a surface has exactly one
//      natural model regardless of which features are on. (The `fabric` overlap
//      between cloth and subsurface resolves to Cloth because cloth is checked
//      first.)
//   2. If a natural advanced model was found AND that model is enabled, it is
//      returned; otherwise the surface falls through to `shipping_look_model`
//      (the Standard_Lit / Unlit model it resolves to today).

// This keeps per-model enabling independent: toggling one model's
// enable flag only ever changes the surfaces whose natural model is that model;
// a surface's natural model never migrates to a different advanced model when
// some other model is disabled (it falls through to Shipping_Look instead).

// Because the function reads only its byte inputs and the fixed marker tables,
// running it twice over identical input yields identical output, and
// disabling every advanced model yields every surface's Shipping_Look model


// `surface_name`         — the xml0 surface name (may be empty).
// `material_name`        — the resolved xml0 material name (may be empty).
// `parent_chain`         — ordered ancestor material names from the xml0
//                          `parent="..."` attributes (may be empty).
// `shipping_look_model`  — the model the surface resolves to today
//                          (Standard_Lit or Unlit); returned on no advanced
//                          match or when the matched model is disabled.
// `enabled`              — the per-model enable gate.
[[nodiscard]] inline Material_Model select_material_model(
        std::string_view surface_name,
        std::string_view material_name,
        const std::vector<std::string>& parent_chain,
        Material_Model shipping_look_model,
        const EnabledModels& enabled) {
    // Step 1: the natural advanced model by fixed marker precedence.
    if (any_name_has_marker(surface_name, material_name, parent_chain,
                            k_cloth_markers, std::size(k_cloth_markers))) {
        return enabled.cloth ? Material_Model::Cloth : shipping_look_model;
    }
    if (any_name_has_marker(surface_name, material_name, parent_chain,
                            k_subsurface_markers, std::size(k_subsurface_markers))) {
        return enabled.subsurface ? Material_Model::Subsurface_Translucent
                                  : shipping_look_model;
    }
    if (any_name_has_marker(surface_name, material_name, parent_chain,
                            k_clear_coat_markers, std::size(k_clear_coat_markers))) {
        return enabled.clear_coat ? Material_Model::Clear_Coat : shipping_look_model;
    }
    if (any_name_has_marker(surface_name, material_name, parent_chain,
                            k_anisotropic_markers, std::size(k_anisotropic_markers))) {
        return enabled.anisotropic ? Material_Model::Anisotropic : shipping_look_model;
    }
    // Step 2: no advanced marker matched — Shipping_Look.
    return shipping_look_model;
}

// Pure foliage shadow-caster predicate.

// True when the surface is an alpha-tested foliage caster — identified
// EXCLUSIVELY from the xml0 surface/material names and their parent chain via
// the existing leaf/leaves/branch/fir foliage heuristic. Used by the
// transparent-shadow path to route foliage casters to the coverage-modulated
// shadow material; no text-format mapping is consulted.
[[nodiscard]] inline bool is_foliage_shadow_caster(
        std::string_view surface_name,
        std::string_view material_name,
        const std::vector<std::string>& parent_chain) {
    return any_name_has_marker(surface_name, material_name, parent_chain,
                               k_foliage_caster_markers,
                               std::size(k_foliage_caster_markers));
}

// Convenience overload for callers with only a single name and no parent chain.
[[nodiscard]] inline bool is_foliage_shadow_caster(std::string_view name) {
    return name_has_marker(name, k_foliage_caster_markers,
                           std::size(k_foliage_caster_markers));
}

// Result of clamping a clear-coat layer strength read from Material_Data:
// `value` is always within the valid non-negative range, and `rejected` is true
// iff the raw input was out of range (negative or NaN) and was replaced.
struct ClearCoatStrengthClamp {
    float value    = 0.0f;
    bool  rejected = false;
};

// Clamp a clear-coat layer strength to its valid non-negative range.

// A negative strength is physically meaningless: it is rejected, clamped to 0,
// and flagged so the cache can emit a one-time diagnostic identifying the
// rejected value. A NaN request collapses to 0 and is likewise reported
// rejected (mirroring the `clamp_quality_*` NaN handling in `engine/types.h`).
// A valid non-negative strength passes through unchanged and unflagged.
[[nodiscard]] inline ClearCoatStrengthClamp clamp_clear_coat_strength(float raw) {
    ClearCoatStrengthClamp r;
    if (std::isnan(raw) || raw < 0.0f) {
        r.value    = 0.0f;
        r.rejected = true;
        return r;
    }
    r.value    = raw;
    r.rejected = false;
    return r;
}

} // namespace heaven::material_model_ref

#endif // HEAVENBENCH_SRC_ENGINE_MATERIAL_MODEL_REF_H_
