/*
 * HeavenBench — Property test for the specular-AA strength ladder
 *
 * Feature: advanced-materials-and-grading,: Specular-AA strength ladder is strictly increasing and bounded
 *
 *
 * makes the specular-AA Quality_Knob an ordered ladder of at
 * least three discrete levels (Low < Medium < High) where each higher level
 * applies STRICTLY stronger highlight-variance suppression, bounded by a
 * configured maximum.  pins the variance/threshold endpoints as
 * the single source of truth (QualityKnobs::k_spec_aa_*_min / *_max) shared by
 * MaterialInstanceCache and this test.
 *
 * The contract lives in the pure, header-only ladder in engine/types.h:
 *
 *   - spec_aa_strength(level) -> SpecAaStrength { variance, threshold }
 *   - bounds: QualityKnobs::k_spec_aa_variance_min / k_spec_aa_variance_max
 *             QualityKnobs::k_spec_aa_threshold_min / k_spec_aa_threshold_max
 *
 * asserts the ladder's two guarantees over many inputs:
 *
 *   - Strictly increasing across ordered levels: both the variance and
 *     the threshold satisfy strength(Low) < strength(Medium) < strength(High).
 *     A higher quality level always suppresses strictly more highlight variance
 *     than the level below it.
 *
 *   - Bounded by the configured maximum: every level's variance
 *     and threshold lie within their configured [min, max] endpoints, and the
 *     High level never exceeds the configured maximum strength. The minimum
 *     (Low) is at or above the configured minimum endpoint.
 *
 * The ladder takes only a QualityLevel and reads the static endpoints, so the
 * core ordering/boundedness is fixed across the three canonical levels. To
 * exercise the input space on every run the generator also draws an arbitrary
 * raw integer level (covering in-range {0,1,2} and clearly out-of-range values)
 * and asserts the ladder clamps it into the bounded range without ever
 * exceeding the configured maximum — pinning the "never exceeds the maximum at
 * the highest level" guarantee even for over-range requests.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <array>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "engine/types.h"

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

// The three ordered quality levels, lowest to highest.
constexpr std::array<heaven::QualityLevel, 3> kLevels{
    heaven::QualityLevel::Low,
    heaven::QualityLevel::Medium,
    heaven::QualityLevel::High,
};

bool run_specular_aa_ladder_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 6: specular-AA strength ladder is strictly increasing and "
        "bounded (Validates: Requirements 8.3, 15.2)",
        [] {
            // The configured endpoints are the single source of truth (Req
            // the ladder interpolates between them and is bounded by the
            // *_max values.
            constexpr float var_min = heaven::QualityKnobs::k_spec_aa_variance_min;
            constexpr float var_max = heaven::QualityKnobs::k_spec_aa_variance_max;
            constexpr float thr_min = heaven::QualityKnobs::k_spec_aa_threshold_min;
            constexpr float thr_max = heaven::QualityKnobs::k_spec_aa_threshold_max;

            const heaven::SpecAaStrength low    = heaven::spec_aa_strength(kLevels[0]);
            const heaven::SpecAaStrength medium = heaven::spec_aa_strength(kLevels[1]);
            const heaven::SpecAaStrength high   = heaven::spec_aa_strength(kLevels[2]);

            // ── Strictly increasing across ordered levels  ──
            // Each higher level applies strictly stronger variance/threshold
            // suppression than the level below it.
            RC_ASSERT(low.variance < medium.variance);
            RC_ASSERT(medium.variance < high.variance);
            RC_ASSERT(low.threshold < medium.threshold);
            RC_ASSERT(medium.threshold < high.threshold);

            // ── Bounded by the configured endpoints  ──
            // Every level lies within [min, max]; the highest level never
            // exceeds the configured maximum strength.
            for (const heaven::SpecAaStrength& s : {low, medium, high}) {
                RC_ASSERT(s.variance  >= var_min);
                RC_ASSERT(s.variance  <= var_max);
                RC_ASSERT(s.threshold >= thr_min);
                RC_ASSERT(s.threshold <= thr_max);
            }

            // The High level is the bounded maximum: it never exceeds the
            // configured maximum endpoints.
            RC_ASSERT(high.variance  <= var_max);
            RC_ASSERT(high.threshold <= thr_max);

            // ── Out-of-range levels clamp into the bounded range  ──
            // An arbitrary raw level — including clearly out-of-range values —
            // is clamped to {Low, Medium, High}, so the ladder can never
            // over/under-shoot its configured endpoints. This exercises the
            // input space on every run while pinning the "never exceeds the
            // maximum" guarantee for over-range requests.
            const int raw = *rc::gen::inRange<int>(-1000, 1001);
            const heaven::SpecAaStrength clamped =
                heaven::spec_aa_strength(static_cast<heaven::QualityLevel>(raw));
            RC_ASSERT(clamped.variance  >= var_min);
            RC_ASSERT(clamped.variance  <= var_max);
            RC_ASSERT(clamped.threshold >= thr_min);
            RC_ASSERT(clamped.threshold <= thr_max);

            // A raw value at or above High maps to the High rung (the bounded
            // maximum); a raw value at or below Low maps to the Low rung.
            if (raw >= static_cast<int>(heaven::QualityLevel::High)) {
                RC_ASSERT(clamped.variance  == high.variance);
                RC_ASSERT(clamped.threshold == high.threshold);
            } else if (raw <= static_cast<int>(heaven::QualityLevel::Low)) {
                RC_ASSERT(clamped.variance  == low.variance);
                RC_ASSERT(clamped.threshold == low.threshold);
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_specular_aa_ladder{
    run_specular_aa_ladder_property};

} // namespace
