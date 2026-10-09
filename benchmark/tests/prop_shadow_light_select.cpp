/*
 * HeavenBench — Property test for deterministic shadow-light selection
 *
 * Feature: advanced-rendering-features,: Shadow-casting light selection is highest-intensity-first, budget-bounded, and deterministic
 *
 *
 * The shadow-casting point/spot light feature  must pick which
 * luminaires cast shadows from a budget. The selection itself is a pure,
 * dependency-free algorithm in engine/shadow_light_select_ref.h:
 *
 *   heaven::shadow_light_ref::select_shadow_casting_lights
 *       std::span<const ShadowLightCandidate>, int budget) -> std::vector<size_t>
 *
 * operating on a tiny {float intensity, uint32_t id} view (NOT the Filament-
 * coupled SceneLightInfo, which the headless test target cannot link). Property
 * 6 pins the four guarantees the selection contract promises over many inputs:
 *
 *   - Budget-bounded size: the selected set has size exactly
 *     min(budget, n) — a budget ≤ 0 selects nothing, a budget ≥ n selects all,
 *     and a budget between selects exactly `budget`.
 *
 *   - Highest-intensity-first: every selected light's intensity
 *     is ≥ every non-selected candidate's intensity, so the chosen set is the
 *     brightest `keep` luminaires (no dimmer light displaces a brighter one).
 *
 *   - Deterministic tie-break by ascending id: when an intensity
 *     boundary is split between equal-intensity lights, the ones with the
 *     smaller stable id win, so the chosen set is fully determined by the input.
 *
 *   - Run-stable / idempotent: calling the algorithm repeatedly on
 *     identical input + budget yields byte-identical output (and the indices
 *     come back in ascending, canonical order).
 *
 * Generators draw an arbitrary candidate list (possibly empty, with repeated
 * intensities and repeated ids so the tie-break and the boundary logic are
 * exercised) and a budget straddling [≤0.. ≥n], so every case covers the
 * none / partial / all branches.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined. RapidCheck runs 100 successful cases by default
 * (TestParams::maxSuccess == 100), meeting the ">= 100 iterations" requirement.
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <rapidcheck.h>

#include "engine/shadow_light_select_ref.h"

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

using heaven::shadow_light_ref::ShadowLightCandidate;
using heaven::shadow_light_ref::select_shadow_casting_lights;

bool run_shadow_light_select_property() {
    return rc::check(
        "Property 6: shadow-casting light selection is highest-intensity-"
        "first, budget-bounded, and deterministic (Validates: Requirements "
        "3.1, 3.2, 3.3, 3.4)",
        [] {
            // An arbitrary candidate list. Intensities are drawn from a small
            // band (in tenths) and ids from a small range so equal intensities
            // and repeated ids occur often, exercising the deterministic
            // tie-break and the intensity-boundary logic. The list may be empty.
            const auto raw = *rc::gen::container<std::vector<std::pair<int, int>>>(
                rc::gen::pair(rc::gen::inRange<int>(-50, 51),
                              rc::gen::inRange<int>(0, 20)));

            std::vector<ShadowLightCandidate> candidates;
            candidates.reserve(raw.size());
            for (const auto& [intensity_tenths, id] : raw) {
                candidates.push_back(ShadowLightCandidate{
                    static_cast<float>(intensity_tenths) / 10.0f,
                    static_cast<uint32_t>(id)});
            }
            const size_t n = candidates.size();

            // A budget straddling the full range: clearly ≤ 0 (select none),
            // every interior value (select exactly `budget`), and ≥ n (all).
            const int budget = *rc::gen::inRange<int>(-3, static_cast<int>(n) + 4);

            const std::vector<size_t> selected =
                select_shadow_casting_lights(candidates, budget);

            // ── Budget-bounded size  ──
            // size == min(budget, n); ≤0 → 0; ≥n → all.
            const size_t expected_size =
                budget <= 0 ? 0u
                            : std::min(static_cast<size_t>(budget), n);
            RC_ASSERT(selected.size() == expected_size);

            // Every returned index is in range and the result is in ascending,
            // duplicate-free canonical order.
            for (size_t i = 0; i < selected.size(); ++i) {
                RC_ASSERT(selected[i] < n);
                if (i > 0) RC_ASSERT(selected[i - 1] < selected[i]);
            }

            // Mark which original indices were selected for the cross-checks.
            std::vector<bool> is_selected(n, false);
            for (size_t idx : selected) is_selected[idx] = true;

            // ── Highest-intensity-first + deterministic tie-break  ──
            // For every selected light S and every non-selected light U, S must
            // dominate U under the selection ordering (intensity desc, then id
            // asc). Equivalently: U is never "better" than S — U does not have a
            // strictly higher intensity, nor equal intensity with a strictly
            // smaller id. This proves both that the brightest are chosen and
            // that ties are broken toward smaller ids deterministically.
            for (size_t s = 0; s < n; ++s) {
                if (!is_selected[s]) continue;
                for (size_t u = 0; u < n; ++u) {
                    if (is_selected[u]) continue;
                    const ShadowLightCandidate& cs = candidates[s];
                    const ShadowLightCandidate& cu = candidates[u];
                    const bool u_better =
                        (cu.intensity > cs.intensity) ||
                        (cu.intensity == cs.intensity && cu.id < cs.id);
                    RC_ASSERT(!u_better);
                }
            }

            // ── Run-stable / idempotent  ──
            // Identical input + budget yields byte-identical output across
            // repeated calls.
            const std::vector<size_t> again =
                select_shadow_casting_lights(candidates, budget);
            RC_ASSERT(again == selected);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_shadow_light_select{
    run_shadow_light_select_property};

} // namespace
