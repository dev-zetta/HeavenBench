/*
 * HeavenBench — Shadow-casting light selection (pure algorithm)
 *
 * Header-only, dependency-free, pure free function that selects which of the
 * scene's point/spot lights should cast shadows, given a budget. It powers the
 * shadow-casting point/spot light feature: only the brightest
 * `budget` luminaires cast shadows, the rest do not.
 *
 * Why this lives in its own header (and not in rhi/filament_util.h):
 * `rhi/filament_util.h` includes Filament math headers, and the renderer's
 * `SceneLightInfo` (rhi/render_feature.h) carries a `utils::Entity`, so neither
 * can be included by the headless `heaven_benchmark_tests` target (which links
 * NO Filament — see benchmark/CMakeLists.txt). To keep the selection logic
 * host-testable for the property test (tests/prop_shadow_light_select.cpp)
 * we mirror the exact discipline already used by `engine/glass_material_ref.h`
 * the pure algorithm operates on a small dependency-free
 * `{intensity, id}` view, and the Filament-facing wrapper
 * `heaven::select_shadow_casting_lights(span<const SceneLightInfo>, int)` in
 * rhi/filament_util.{h,cpp} simply adapts each `SceneLightInfo` into a
 * `ShadowLightCandidate` and delegates here.
 *
 * Selection contract:
 *   - The selected set has size exactly `min(budget, n)` (n = eligible count);
 *     a budget ≤ 0 selects nothing, a budget ≥ n selects all.
 *   - It consists of the lights with the highest configured luminous intensity.
 *   - Ties between equal-intensity lights are broken by a STABLE per-light
 *     identifier (the light's entity id) so the selected set is identical
 *     across runs given identical scene-light input (deterministic).
 *   - Selected indices are returned in ascending original-index order, giving a
 *     canonical set representation that is trivially comparable across runs.
 *
 * Everything here is pure: deterministic, side-effect free, no I/O, no GPU /
 * Filament / SDL state — so the property test can hammer the whole input space
 * without a GPU.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_SHADOW_LIGHT_SELECT_REF_H_
#define HEAVENBENCH_SRC_ENGINE_SHADOW_LIGHT_SELECT_REF_H_

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace heaven::shadow_light_ref {

// A dependency-free view of one candidate point/spot light: just the two values
// the selection depends on — its configured luminous intensity and its stable
// per-light identifier (the entity id, used purely as the deterministic
// tie-break between equal-intensity lights).
struct ShadowLightCandidate {
    float    intensity = 0.0f;  // configured luminous intensity (higher = brighter)
    uint32_t id        = 0;     // stable per-light identifier (entity id)
    bool     eligible  = true;  // authored or enclosed-source shadow policy
};

// Select the highest-intensity eligible lights, ties broken by ascending id,
// returned as ascending original indices.

// `candidates` — the candidate point/spot lights (order is the caller's; the
//                returned indices reference this span).
// `budget`     — the maximum number of concurrently shadow-casting lights. A
//                value ≤ 0 selects none; a value ≥ n selects all eligible lights.
[[nodiscard]] inline std::vector<size_t> select_shadow_casting_lights(
        std::span<const ShadowLightCandidate> candidates, int budget) {
    const size_t n = candidates.size();
    if (budget <= 0 || n == 0) return {};

    std::vector<size_t> order;
    order.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (candidates[i].eligible) order.push_back(i);
    }
    const size_t keep = std::min(static_cast<size_t>(budget), order.size());
    if (keep == order.size()) return order;

    // Rank the candidates by (intensity desc, id asc, index asc). The id tie-
    // break makes the chosen set deterministic across runs for equal-intensity
    // lights; the trailing index term keeps the sort total even if two
    // candidates share both intensity and id.
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const ShadowLightCandidate& ca = candidates[a];
        const ShadowLightCandidate& cb = candidates[b];
        if (ca.intensity != cb.intensity) return ca.intensity > cb.intensity;
        if (ca.id != cb.id)               return ca.id < cb.id;
        return a < b;
    });

    // Keep the top `keep`, then return their original indices in ascending
    // order so the result is a canonical, run-stable representation of the set.
    std::vector<size_t> selected(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(keep));
    std::sort(selected.begin(), selected.end());
    return selected;
}

} // namespace heaven::shadow_light_ref

#endif // HEAVENBENCH_SRC_ENGINE_SHADOW_LIGHT_SELECT_REF_H_
