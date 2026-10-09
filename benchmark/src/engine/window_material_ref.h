/*
 * HeavenBench — Window material classification (pure predicate)
 *
 * Header-only, dependency-free, pure free functions that classify whether a
 * Heaven surface/material is a "window" — one of the OPAQUE, height-mapped wall
 * surfaces with the glass panes, mullions, and frame baked into a full PBR
 * texture set (`building_windows`, `fort_window`, `fort_small_window`,
 * `lab_windows`, `building_tessellation_windows`, …). These are NOT transparent
 * glass (see engine/glass_material_ref.h for the deliberate exclusion of the
 * `window` token from the glass classifier) and must stay opaque, so they are
 * routed to the dedicated, opaque heaven_window base material rather than the
 * refractive heaven_glass one.
 *
 * This mirrors the name-based discipline already used for glass
 * (`glass_material_ref.h`) and foliage/advanced-model detection
 * (`material_model_ref.h`): classification is driven EXCLUSIVELY from the
 * xml0-derived material name and its `parent="..."` chain — there is NO
 * text-format mapping file, nothing reads from disk, and every function is
 * deterministic and side-effect free.
 *
 * The marker comes from the actual Heaven material naming observed in the
 * extracted scene (`extracted/heaven/heaven/materials/heaven_buildings.mat`):
 * every window material's name contains the `window` token, and night variants
 * (`building_windows_on`) inherit it through their parent chain.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_WINDOW_MATERIAL_REF_H_
#define HEAVENBENCH_SRC_ENGINE_WINDOW_MATERIAL_REF_H_

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace heaven::window_ref {

// The xml0-derived marker that denotes a (baked, opaque) window wall surface.
// Kept as a single source of truth so the predicate and any future property
// test agree. A material is a window when its name (or any name in its parent
// chain) contains this token.
inline constexpr std::string_view k_window_markers[] = {
    "window",
};

// True when `name` contains any of the window markers as a substring.
[[nodiscard]] inline bool name_has_window_marker(std::string_view name) {
    return std::ranges::any_of(k_window_markers, [name](std::string_view marker) {
        return name.find(marker) != std::string_view::npos;
    });
}

// Pure window-classification predicate.

// A surface is a window when EITHER its own xml0-derived material name carries a
// window marker, OR any material in its `parent="..."` chain does (so night
// variants such as `building_windows_on` are caught through their parent).

// `material_name`  — the resolved xml0 material name for the surface (may be
//                    empty; an empty name is not a window).
// `parent_chain`   — the ordered ancestor material names walked from the xml0
//                    `parent="..."` attributes (may be empty). Only these
//                    binary-derived names are consulted; no file is read.
[[nodiscard]] inline bool is_window_material(
        std::string_view material_name,
        const std::vector<std::string>& parent_chain) {
    if (name_has_window_marker(material_name)) return true;
    return std::ranges::any_of(parent_chain, [](const std::string& parent) {
        return name_has_window_marker(parent);
    });
}

// Convenience overload for callers with no parent chain handy.
[[nodiscard]] inline bool is_window_material(std::string_view material_name) {
    return name_has_window_marker(material_name);
}

} // namespace heaven::window_ref

#endif // HEAVENBENCH_SRC_ENGINE_WINDOW_MATERIAL_REF_H_
