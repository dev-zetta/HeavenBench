/*
 * HeavenBench — Glass material classification (pure predicate)
 *
 * Header-only, dependency-free, pure free functions that classify whether a
 * Heaven surface/material is "glass" (a window, airship glass, or lab
 * equipment) purely from the xml0-derived material name and its parent chain.
 *
 * This mirrors the existing name-based discipline already used for foliage
 * auto-detection in `material_rhi.cpp` (surface names containing leaf/leaves/
 * branch/fir get alpha_ref/two_sided) — see AGENTS.md. The glass
 * predicate exists so the screen-space-refraction feature  can
 * decide, per surface, whether to route it to the refractive material domain.
 *
 * Hard rule (and the AGENTS.md "no material_map.txt" rule):
 * glass identification is driven EXCLUSIVELY from the binary xml0 surface /
 * material data (the material name and its `parent="..."` chain). There is NO
 * text-format mapping file, and nothing here reads from disk or touches any
 * GPU/Filament/SDL state. Every function is deterministic and side-effect free
 * so the property-based tests in `heaven_benchmark_tests`
 * can hammer the whole input space without a GPU.
 *
 * The marker below comes from the actual Heaven material naming observed in the
 * extracted scene (the `.mat` files under
 * `extracted/heaven/heaven/materials/` and the `.node` files under
 * `.../nodes/`):
 *
 *   - Glass / glazing: building_glass, building_glass_on, glass01, glass02,
 *                      lamp_glass, lamp_post_glass, port_lamp_glass (the airship
 *                      and lab equipment reuse building_glass for their glazing)
 *
 * IMPORTANT — `*window*` materials are NOT glass. Materials such as
 * building_windows, fort_window, fort_small_window, mansion_building_windows,
 * lab_windows, and building_tessellation_windows are OPAQUE WALL surfaces with
 * the window apertures painted into a full PBR texture set
 * (`building_windows_d/_n/_s/_h/_t`) plus an emission map
 * (`building_windows_e`) that lights the panes at night. They are height-mapped
 * and tessellated like any other wall. Routing them through screen-space
 * refraction makes the wall itself transmit the scene behind it — the "windows
 * suddenly transparent" bug. The genuine transparent glass panes are authored
 * separately and always carry a `glass` token, so a substring match on that one
 * marker — applied to the material name and every name in its parent chain — is
 * sufficient and keeps the same low-ceremony discipline as the foliage detector.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_GLASS_MATERIAL_REF_H_
#define HEAVENBENCH_SRC_ENGINE_GLASS_MATERIAL_REF_H_

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace heaven::glass_ref {

// The xml0-derived marker that denotes glazing. Kept as a single source of
// truth so the predicate and its property test agree. A material is glass when
// its name (or any name in its parent chain) contains this token.

// Only `glass` is a marker. `window` is deliberately NOT a marker: Heaven's
// `*window*` materials (building_windows, fort_window, …) are opaque,
// height-mapped/tessellated WALL surfaces with the panes baked into their
// diffuse + emission textures, not transparent panes — see the file header.
inline constexpr std::string_view k_glass_markers[] = {
    "glass",
};

// True when `name` contains any of the glass markers as a substring. This is
// the same "name contains token" test the foliage detector uses for
// leaf/leaves/branch/fir.
[[nodiscard]] inline bool name_has_glass_marker(std::string_view name) {
    return std::ranges::any_of(k_glass_markers, [&](std::string_view marker) {
        return name.find(marker) != std::string_view::npos;
    });
}

// Pure glass-classification predicate.

// A surface is glass when EITHER its own xml0-derived material name carries a
// glass marker, OR any material in its `parent="..."` chain does. Passing the
// parent chain mirrors how Heaven authors glazing: leaf materials such as
// `building_glass_on` (the lit night variant) inherit from `building_glass`,
// and a future renamed leaf would still be caught through its parent.

// `material_name`  — the resolved xml0 material name for the surface (may be
//                    empty when a surface has no binding; an empty name is not
//                    glass).
// `parent_chain`   — the ordered list of ancestor material names walked from
//                    the xml0 `parent="..."` attributes (may be empty). Only
//                    these binary-derived names are consulted; no file is read.
[[nodiscard]] inline bool is_glass_material(
        std::string_view material_name,
        const std::vector<std::string>& parent_chain) {
    if (name_has_glass_marker(material_name)) return true;
    return std::ranges::any_of(parent_chain, [](const std::string& parent) {
        return name_has_glass_marker(parent);
    });
}

// Convenience overload for callers that have no parent chain handy (e.g. a bare
// surface-name check). Equivalent to passing an empty parent chain.
[[nodiscard]] inline bool is_glass_material(std::string_view material_name) {
    return name_has_glass_marker(material_name);
}

} // namespace heaven::glass_ref

#endif // HEAVENBENCH_SRC_ENGINE_GLASS_MATERIAL_REF_H_
