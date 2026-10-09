// Feature: weather-and-rendering-features,: Wetness increases with surface-up alignment
/*
 * Wetness increases with surface-up alignment.
 *
 *
 * The lit materials weight the per-surface wetness contribution by the surface
 * normal's vertical alignment toward world-up (Z-up world), and the CPU mirror
 * in engine/weather_shader_ref.h is the source of truth for that block:
 *
 *     float up  = clamp(getWorldNormal().z, 0.0, 1.0); Z-up
 *     float wet = wetAmount * up;
 *
 * i.e. wetness_for_normal(wetAmount, n) = wetAmount * clamp(n.z, 0, 1).
 *
 * This property asserts the three clauses the reference is the source of truth
 * for, for any fixed non-negative wet amount:
 *
 *   - Monotonic in up-alignment. For two normals n1, n2 with n1.z <= n2.z, the
 *     wetness for n1 is <= the wetness for n2. clamp is monotonic non-decreasing
 *     in n.z and wetAmount >= 0, so w * clamp(n1.z) <= w * clamp(n2.z) holds
 *     exactly (float multiplication preserves the ordering of non-negative
 *     operands). An upward-facing surface therefore never receives less wetness
 *     than an otherwise identical downward-facing one.
 *   - Downward-facing yields exactly zero. Any normal with n.z <= 0 clamps to 0,
 *     so the contribution is exactly 0.0 regardless of wetAmount.
 *   - Fully up yields exactly the wet amount. A normal with n.z == 1 clamps to
 *     1, so the contribution is exactly wetAmount.
 *
 * Only n.z drives the result (x and y are ignored by the reference), but the
 * generators still vary x and y freely so the test exercises arbitrary normals,
 * not just canonical ones. Generators are finite and bounded (integer grids
 * mapped onto closed float intervals; no NaN/Inf) so the ordering comparisons
 * are well defined. The function under test is header-only and GLM-only (no
 * GPU/Filament/SDL), so this runs on the host. This translation unit
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "engine/weather_shader_ref.h"

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

namespace wr = heaven::weather_ref;

// Map an integer grid onto a closed [lo, hi] float interval so the endpoints
// are exactly reachable while the interior is covered densely.
rc::Gen<float> ranged(float lo, float hi) {
    return rc::gen::map(rc::gen::inRange<int>(0, 1'000'001),
                        [lo, hi](int g) {
                            return lo + (static_cast<float>(g) / 1'000'000.0f) * (hi - lo);
                        });
}

bool run_wetness_up_alignment_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: weather-and-rendering-features, Property 8: Wetness increases "
        "with surface-up alignment (Validates: Requirements 3.3)",
        [] {
            // A fixed, non-negative wet amount for this case (holds for any
            // wetAmount >= 0; the endpoints span 0 and 1 exactly).
            const float wetAmount = *ranged(0.0f, 1.0f);

            // ── Monotonic in up-alignment ──────────────────────────────────
            // Two arbitrary normals; x and y vary freely (ignored by the
            // reference) while the z components are ordered so n1.z <= n2.z.
            float z1 = *ranged(-1.0f, 1.0f);
            float z2 = *ranged(-1.0f, 1.0f);
            if (z1 > z2) std::swap(z1, z2);

            const glm::vec3 n1(*ranged(-1.0f, 1.0f), *ranged(-1.0f, 1.0f), z1);
            const glm::vec3 n2(*ranged(-1.0f, 1.0f), *ranged(-1.0f, 1.0f), z2);

            const float w1 = wr::wetness_for_normal(wetAmount, n1);
            const float w2 = wr::wetness_for_normal(wetAmount, n2);

            // Greater up-alignment never yields less wetness.
            RC_ASSERT(w1 <= w2);

            // Wetness is non-negative throughout (clamp floor is 0, wetAmount >= 0).
            RC_ASSERT(w1 >= 0.0f);
            RC_ASSERT(w2 >= 0.0f);

            // ── Downward-facing yields exactly zero ────────────────────────
            // Any normal whose z is at or below the horizon receives no wetness.
            const glm::vec3 down(*ranged(-1.0f, 1.0f),
                                 *ranged(-1.0f, 1.0f),
                                 *ranged(-1.0f, 0.0f));
            RC_ASSERT(wr::wetness_for_normal(wetAmount, down) == 0.0f);

            // ── Fully up yields exactly the wet amount ─────────────────────
            // A normal with n.z == 1 clamps to 1, so the contribution equals
            // wetAmount exactly.
            const glm::vec3 up(*ranged(-1.0f, 1.0f), *ranged(-1.0f, 1.0f), 1.0f);
            RC_ASSERT(wr::wetness_for_normal(wetAmount, up) == wetAmount);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_wetness_up_alignment{
    &run_wetness_up_alignment_property};

} // namespace
