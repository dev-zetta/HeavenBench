/*
 * HeavenBench — Filament shared utilities implementation
 *
 * Pure, state-free helpers extracted from filament_backend.cpp's
 * render_frame so the per-frame logic reads as a sequence of named
 * steps. No Filament engine state here — only string classification.
 */
#include "rhi/filament_util.h"
#include "rhi/render_feature.h"            // SceneLightInfo
#include "engine/shadow_light_select_ref.h" // pure selection algorithm

namespace heaven {

bool is_lod_imposter(std::string_view surface_name) {
    auto nonzero_at = [&](size_t idx_pos) {
        return idx_pos < surface_name.size()
            && surface_name[idx_pos] >= '1' && surface_name[idx_pos] <= '9';
    };
    // LOD marker preceded by an underscore: buildings, structures, and
    // the fir *trunks* use this form (`building_windows_lod1`,
    // `fir_trunk_lod_1`, …).
    auto skip_after = [&](std::string_view marker) {
        auto p = surface_name.find(marker);
        return p != std::string_view::npos
            && nonzero_at(p + marker.size());
    };
    // LOD marker at the very START of the surface name. The fir tree
    // *branch* cards (meshes fir_1..fir_4) are named bare `lod_0`..
    // `lod_4` with no prefix, so the `_lod` matcher above never caught
    // them and all four simplified billboard imposters rendered on top
    // of the detailed `lod_0`, muddying the trees into a pale criss-
    // cross of flat cards.
    auto skip_prefix = [&](std::string_view marker) {
        return surface_name.starts_with(marker)
            && nonzero_at(marker.size());
    };
    return skip_after("_lod")  ||   // "..._lod1", "..._lod2"
           skip_after("_lod_") ||   // "..._lod_1", "..._lod_2"
           skip_prefix("lod")  ||   // "lod1", "lod2"
           skip_prefix("lod_");     // "lod_1", "lod_2"
}

EmissionLightTuning classify_emission_light(std::string_view surface_name) {
    auto contains = [&](std::string_view k) {
        return surface_name.find(k) != std::string_view::npos;
    };

    // Heuristic by surface name → colour and intensity. EVERY emission
    // light is a POINT light — see the long-form rationale in
    // EXTRACTED_SHADERS.md. Spot cones derived from the surface→
    // building-centre vector ended up aimed in arbitrary directions
    // because that vector is sensitive to whatever the bbox happens to
    // include (gable peaks, eaves, attached props), producing the
    // visible "cone stamp" artefact on walls behind windows/lanterns.

    // Tighter falloff radii than visually optimal — each light's
    // bounding sphere is what Filament uses to decide which screen
    // tiles a light affects, and overlapping spheres from many adjacent
    // lamps can overflow the per-tile light record buffer (showing up
    // as screen-tile-aligned dark rectangles on lit walls). Pulling
    // falloff in and boosting intensity keeps the visible reach the
    // same while drastically cutting the number of tiles each light
    // claims.
    EmissionLightTuning t;  // default: warm window-ish
    if (contains("lamp") || contains("torch") || contains("lantern")) {
        t.color = {1.0f, 0.55f, 0.20f};   // warm orange flame
        t.intensity = 180000.0f;
        t.falloff   = 9.0f;
    } else if (contains("glass")) {
        // Glass lantern element — classic street lamp look.
        t.color = {1.0f, 0.65f, 0.30f};
        t.intensity = 150000.0f;
        t.falloff   = 8.0f;
    } else if (contains("window")) {
        t.color = {1.0f, 0.78f, 0.45f};
        t.intensity = 90000.0f;
        t.falloff   = 6.0f;
    } else if (contains("door")) {
        t.color = {1.0f, 0.80f, 0.55f};
        t.intensity = 50000.0f;
        t.falloff   = 4.5f;
    } else if (contains("mill")) {
        t.color = {1.0f, 0.85f, 0.55f};
        t.intensity = 30000.0f;
        t.falloff   = 4.0f;
    }
    return t;
}

std::vector<size_t> select_shadow_casting_lights(
        std::span<const SceneLightInfo> lights, int budget) {
    // Adapt each SceneLightInfo into the dependency-free {intensity, id} view
    // the pure algorithm operates on, then delegate. The id is the light's
    // stable entity identifier — the deterministic tie-break between
    // equal-intensity lights.
    std::vector<shadow_light_ref::ShadowLightCandidate> candidates;
    candidates.reserve(lights.size());
    for (const SceneLightInfo& l : lights) {
        candidates.push_back({l.intensity, l.entity.getId(), l.shadow_eligible});
    }
    return shadow_light_ref::select_shadow_casting_lights(candidates, budget);
}

} // namespace heaven
