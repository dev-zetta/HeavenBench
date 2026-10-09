/*
 * HeavenBench — Property tests for the advanced-rendering Quality_Knobs
 *
 * Feature: advanced-rendering-features,: Quality levels map to strictly increasing, bounded sample/step counts
 *
 *
 * The Quality_Knob math lives entirely in pure, header-only mapping functions
 * in engine/types.h (the single source of truth shared by main.cpp's CLI parser
 * and these tests, per the WeatherConfig precedent).  pins down the
 * monotonicity + boundedness contract those mappings promise:
 *
 *   - soft_shadow_sample_count(level)          —
 *   - contact_shadow_steps(level, knobs)       —
 *   - cloud_raymarch_steps(level, knobs)       —
 *   - ssgi_samples(level, knobs)               —
 *
 * For each mapping the test asserts the two guarantees that matter
 *
 *
 *   - Strictly increasing across ordered levels: count(Low) < count(Medium) <
 *     count(High). A higher quality level always costs strictly more
 *     samples/steps than the level below it.
 *
 *   - Bounded: every level's count lies inside its configured bound range, so
 *     no knob is unbounded at its highest setting. The cloud minimum
 *     is additionally pinned at ≥ 1 step.
 *
 * The bound fields (contact/cloud min+max, ssgi max) are generated as part of a
 * QualityKnobs value. Generators constrain the bound spans to the documented
 * strict-monotonicity contract (span ≥ 2 across the three levels — see
 * level_to_count's contract in types.h); a degenerate span < 2 cannot yield
 * three strictly increasing integers and is explicitly out of the mappings'
 * promised range. The default QualityKnobs (Shipping_Look baseline) is exercised
 * on every case so the shipped configuration is always covered.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined. Tasks 1.5–1.7 append further Quality_Knob
 * properties to this same file using the same Register pattern.
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <span>
#include <vector>

#include <rapidcheck.h>

#include "engine/types.h"

// The registry lives in test_main.cpp (external linkage, no shared header), so
// mirror its minimal interface here to append our tests. The struct definition
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

// Assert a level→count mapping is strictly increasing across Low<Medium<High
// and that every level's count lies within [lo, hi]. `name` tags the failing
// assertion so a counterexample identifies the offending knob.
template <typename Fn>
void check_increasing_and_bounded(Fn&& count_of, int lo, int hi) {
    const int c_low    = count_of(kLevels[0]);
    const int c_medium = count_of(kLevels[1]);
    const int c_high   = count_of(kLevels[2]);

    // Strictly increasing across ordered levels: a higher level
    // always costs strictly more than the level below it.
    RC_ASSERT(c_low < c_medium);
    RC_ASSERT(c_medium < c_high);

    // Bounded by the configured range at every level: nothing falls
    // below the minimum or above the maximum, so the highest setting is bounded.
    for (int c : {c_low, c_medium, c_high}) {
        RC_ASSERT(c >= lo);
        RC_ASSERT(c <= hi);
    }
}

bool run_quality_knob_monotonic_bounded_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 2: quality levels map to strictly increasing, bounded "
        "sample/step counts (Validates: Requirements 1.3, 2.3, 6.3, 7.3, "
        "11.2, 11.3)",
        [] {
            // Start from the shipped defaults so the Shipping_Look baseline is
            // covered on every case, then override the bound fields with
            // generated values that honour the strict-monotonicity contract.
            heaven::QualityKnobs k{};

            // Contact-shadow bounds: min ≥ 0, span ≥ 2 so the three levels are
            // strictly increasing integers across [min, max].
            const int contact_min = *rc::gen::inRange<int>(0, 4096);
            const int contact_span = *rc::gen::inRange<int>(2, 4096);
            k.contact_shadow_step_min = contact_min;
            k.contact_shadow_step_max = contact_min + contact_span;

            // Cloud bounds: the mapping forces the effective minimum to ≥ 1
            // Generate a raw minimum that may dip to 0 to exercise
            // that clamp, then size the maximum off the *effective* low bound so
            // the post-clamp span stays ≥ 2 (level_to_count's documented
            // strict-monotonicity precondition — see types.h).
            const int cloud_min_raw = *rc::gen::inRange<int>(0, 4096);
            const int cloud_eff_lo  = std::max(1, cloud_min_raw);
            const int cloud_span = *rc::gen::inRange<int>(2, 4096);
            k.cloud_step_min = cloud_min_raw;
            k.cloud_step_max = cloud_eff_lo + cloud_span;

            // SSGI maximum: ssgi_samples derives its low end as max/4, so a max
            // ≥ 8 guarantees hi - lo ≥ 2 (strictly increasing). Bound generously.
            k.ssgi_sample_max = *rc::gen::inRange<int>(8, 200000);

            // ──  — soft-shadow sample count (fixed ladder 8/16/24) ──
            // Strictly increasing; its bounds are its own Low/High endpoints.
            check_increasing_and_bounded(
                [](heaven::QualityLevel lvl) {
                    return heaven::soft_shadow_sample_count(lvl);
                },
                heaven::soft_shadow_sample_count(heaven::QualityLevel::Low),
                heaven::soft_shadow_sample_count(heaven::QualityLevel::High));

            // ──  — contact-shadow step count, bounded by [min, max] ──
            check_increasing_and_bounded(
                [&](heaven::QualityLevel lvl) {
                    return heaven::contact_shadow_steps(lvl, k);
                },
                k.contact_shadow_step_min, k.contact_shadow_step_max);

            // ──  — cloud raymarch steps, bounded by [max(1,min), max] ──
            // The effective minimum is forced to ≥ 1 step, so assert against
            // the clamped low bound and the (clamped) high bound.
            const int cloud_lo = std::max(1, k.cloud_step_min);
            const int cloud_hi = std::max(cloud_lo, k.cloud_step_max);
            // Cloud minimum is at least 1 step.
            RC_ASSERT(cloud_lo >= 1);
            RC_ASSERT(heaven::cloud_raymarch_steps(heaven::QualityLevel::Low, k) >= 1);
            check_increasing_and_bounded(
                [&](heaven::QualityLevel lvl) {
                    return heaven::cloud_raymarch_steps(lvl, k);
                },
                cloud_lo, cloud_hi);

            // ──  — SSGI sample count, bounded by [max/4, max] ──
            const int ssgi_hi = std::max(2, k.ssgi_sample_max);
            const int ssgi_lo = std::max(1, ssgi_hi / 4);
            check_increasing_and_bounded(
                [&](heaven::QualityLevel lvl) {
                    return heaven::ssgi_samples(lvl, k);
                },
                ssgi_lo, ssgi_hi);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_quality_knob_monotonic_bounded{
    run_quality_knob_monotonic_bounded_property};

// ── Additional Quality_Knob properties (tasks 1.5–1.7) append below ──
// Follow the pattern above: a `bool run_*_property()` defining one rc::check,
// then a `const heaven::testing::Register g_register_*{...};` to self-register.

// Feature: advanced-rendering-features,: Out-of-range knob values clamp to the nearest valid bound


// The three clamping knobs in engine/types.h share one out-of-range contract
// (CLAMP-and-report): a value outside the valid range is pulled to the nearest
// valid bound and the `clamped` flag is set; an in-range value passes through
// unchanged with clamped == false.  pins that contract for each of
// the three clamp helpers (the single source of truth shared with the CLI):

//   - clamp_quality_level(raw)                       → QualityLevelClamp
//       valid range [0,2] == {Low, Medium, High}
//   - clamp_quality_shadow_light_budget(raw, knobs)  → IntKnobClamp
//       valid range [0, max(0, shadow_light_budget_max)]
//   - clamp_quality_foliage_density(raw, knobs)      → FloatKnobClamp
//       valid range [1.0, max(1.0, foliage_density_max)], NaN → 1.0 baseline

// Generators deliberately straddle each range (below, inside, above) so every
// case exercises both the clamping branch and the pass-through branch. The
// default QualityKnobs is used for the bound source so the shipped
// configuration is covered, with the bound fields overridden by generated
// values to widen coverage.
bool run_quality_knob_clamp_property() {
    return rc::check(
        "Property 3: out-of-range knob values clamp to the nearest valid "
        "bound (Validates: Requirements 1.7, 11.4)",
        [] {
            // ── clamp_quality_level — ordered-level knobs, valid range [0,2] ──
            // Straddle the range generously below 0 and above 2.
            {
                const int raw = *rc::gen::inRange<int>(-1000, 1003);
                const heaven::QualityLevelClamp c = heaven::clamp_quality_level(raw);
                const int valid_lo = 0; // QualityLevel::Low
                const int valid_hi = 2; // QualityLevel::High
                const int expected = std::clamp(raw, valid_lo, valid_hi);

                // Result is always pulled into the valid ordered range.
                RC_ASSERT(static_cast<int>(c.value) == expected);

                if (raw < valid_lo || raw > valid_hi) {
                    // Out of range: clamped to the nearest valid bound and flagged.
                    RC_ASSERT(c.clamped);
                    RC_ASSERT(static_cast<int>(c.value) ==
                              (raw < valid_lo ? valid_lo : valid_hi));
                } else {
                    // In range: passes through unchanged, clamped == false.
                    RC_ASSERT(!c.clamped);
                    RC_ASSERT(static_cast<int>(c.value) == raw);
                }
            }

            // ── clamp_quality_shadow_light_budget — int knob, [0, hi] ──
            {
                heaven::QualityKnobs k{};
                k.shadow_light_budget_max = *rc::gen::inRange<int>(0, 4096);
                const int hi = std::max(0, k.shadow_light_budget_max);

                // Straddle below 0 and above hi.
                const int raw = *rc::gen::inRange<int>(-1000, hi + 1001);
                const heaven::IntKnobClamp c =
                    heaven::clamp_quality_shadow_light_budget(raw, k);
                const int expected = std::clamp(raw, 0, hi);

                RC_ASSERT(c.value == expected);
                RC_ASSERT(c.value >= 0);
                RC_ASSERT(c.value <= hi);

                if (raw < 0 || raw > hi) {
                    // Out of range: clamped to the nearest bound and flagged.
                    RC_ASSERT(c.clamped);
                    RC_ASSERT(c.value == (raw < 0 ? 0 : hi));
                } else {
                    // In range: unchanged, clamped == false.
                    RC_ASSERT(!c.clamped);
                    RC_ASSERT(c.value == raw);
                }
            }

            // ── clamp_quality_foliage_density — float knob, [1.0, hi] ──
            {
                heaven::QualityKnobs k{};
                // Keep the configured maximum at or above the 1.0 baseline so
                // the valid range is non-degenerate; the helper itself also
                // guards this with max(1.0,...).
                k.foliage_density_max = static_cast<float>(*rc::gen::inRange<int>(1, 1000));
                const float hi = std::max(1.0f, k.foliage_density_max);

                // Straddle below 1.0 (including negatives) and above hi.
                const float raw = static_cast<float>(*rc::gen::inRange<int>(-2000, static_cast<int>(hi * 100.0f) + 2001)) / 100.0f;
                const heaven::FloatKnobClamp c =
                    heaven::clamp_quality_foliage_density(raw, k);
                const float expected = std::clamp(raw, 1.0f, hi);

                RC_ASSERT(c.value == expected);
                RC_ASSERT(c.value >= 1.0f);
                RC_ASSERT(c.value <= hi);

                if (raw < 1.0f || raw > hi) {
                    // Out of range: clamped to the nearest bound and flagged.
                    RC_ASSERT(c.clamped);
                    RC_ASSERT(c.value == (raw < 1.0f ? 1.0f : hi));
                } else {
                    // In range: passes through unchanged, clamped == false.
                    RC_ASSERT(!c.clamped);
                    RC_ASSERT(c.value == raw);
                }
            }

            // NaN foliage density collapses to the 1.0 Shipping_Look baseline
            // and is reported as clamped.
            {
                heaven::QualityKnobs k{};
                const heaven::FloatKnobClamp c = heaven::clamp_quality_foliage_density(
                    std::numeric_limits<float>::quiet_NaN(), k);
                RC_ASSERT(c.clamped);
                RC_ASSERT(c.value == 1.0f);
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_quality_knob_clamp{
    run_quality_knob_clamp_property};

// Feature: advanced-rendering-features,: MSAA accepts exactly {1,2,4} and rejects-and-retains otherwise


// MSAA has REJECT-and-retain semantics, distinct from the CLAMP-and-report
// knobs above. The two pure helpers in engine/types.h encode the
// contract (the single source of truth shared with the CLI):

//   - msaa_accepts(n)                      → true iff n ∈ {1, 2, 4}
//   - apply_msaa_samples(requested, prev)  → MsaaApplyResult

// pins both halves of the apply contract:

//   - Accepted request (n ∈ {1,2,4}): value == requested, rejected == false,
//     and msaa_accepts agrees.
//   - Out-of-set request: value == previous (the prior configured count is
//     RETAINED, not clamped to a nearby valid value), rejected == true,
//     rejected_value == requested, and msaa_accepts agrees.

// The generator straddles the accepted set: it draws an arbitrary integer over
// a wide range (covering the accepted {1,2,4} and a dense band of rejected
// values, including negatives and 3) and an independent `previous` drawn from
// the accepted set, so every case exercises both branches over many inputs.
bool run_msaa_accept_reject_property() {
    return rc::check(
        "Property 4: MSAA accepts exactly {1,2,4} and rejects-and-retains "
        "otherwise (Validates: Requirements 4.2, 4.3)",
        [] {
            // `previous` is a currently-configured (already valid) sample count
            // drawn from the accepted set, so a rejected request must retain it.
            static constexpr std::array<int, 3> kAccepted{1, 2, 4};
            const int previous = kAccepted[*rc::gen::inRange<std::size_t>(0, 3)];

            // The requested value straddles the accepted set generously: the
            // wide range covers 1/2/4 plus many rejected values (0, 3, 5…,
            // negatives) so both branches are exercised across the run.
            const int requested = *rc::gen::inRange<int>(-1000, 1001);

            const heaven::MsaaApplyResult r =
                heaven::apply_msaa_samples(requested, previous);

            // The accept predicate and the apply helper must agree on whether
            // the request is in {1,2,4}.
            const bool accepted = heaven::msaa_accepts(requested);
            RC_ASSERT(accepted == (requested == 1 || requested == 2 || requested == 4));

            if (accepted) {
                // Accepted: the request becomes the new value, nothing rejected.
                RC_ASSERT(r.value == requested);
                RC_ASSERT(!r.rejected);
            } else {
                // Rejected: the previously configured count is RETAINED (not
                // clamped), the request is flagged as rejected, and the rejected
                // value is reported so the CLI can name it.
                RC_ASSERT(r.value == previous);
                RC_ASSERT(r.rejected);
                RC_ASSERT(r.rejected_value == requested);
                // Retained value is itself a valid, accepted count.
                RC_ASSERT(heaven::msaa_accepts(r.value));
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_msaa_accept_reject{
    run_msaa_accept_reject_property};

// Feature: advanced-rendering-features,: Minimum knob settings return cost proxies to the Shipping_Look baseline


// makes the deliberate GPU load of the advanced-rendering feature set
// opt-in and scalable: lowering every Quality_Knob to its minimum returns GPU
// cost toward the Shipping_Look baseline.  pins that contract using
// the pure mapping functions in engine/types.h as cost proxies (the single
// source of truth shared with the CLI). For each ordered-level knob the minimum
// setting is QualityLevel::Low; for the foliage knob the minimum is the 1.0
// Shipping_Look density baseline.

// At the minimum setting every cost proxy must:
//   - equal the minimum of its range / the Shipping_Look baseline value, and
//   - be strictly less than the cost at any higher setting (so raising the knob
//     only ever adds load — minimum is genuinely the floor).

// Concretely:
//   - soft_shadow_sample_count(Low)         == the fixed ladder's minimum (8)
//   - contact_shadow_steps(Low, k)          == k.contact_shadow_step_min
//   - cloud_raymarch_steps(Low, k)          == max(1, k.cloud_step_min)
//   - ssgi_samples(Low, k)                  == max(1, max(2,ssgi_max)/4)
//   - foliage_blade_count(1.0, k)           == k.foliage_blade_baseline

// The bound fields are generated honouring the same strict-monotonicity
// contract  relies on (span ≥ 2 across the three levels), so "minimum
// strictly less than higher settings" is meaningful. The default QualityKnobs
// (Shipping_Look baseline) is the starting point on every case so the shipped
// configuration is always covered.
bool run_quality_knob_minimum_baseline_property() {
    return rc::check(
        "Property 13: minimum knob settings return cost proxies to the "
        "Shipping_Look baseline (Validates: Requirements 11.5)",
        [] {
            // Start from the shipped defaults so the Shipping_Look baseline is
            // exercised on every case, then widen coverage with generated bound
            // fields that honour the strict-monotonicity contract (span ≥ 2).
            heaven::QualityKnobs k{};

            // Contact-shadow bounds: min ≥ 0, span ≥ 2.
            const int contact_min = *rc::gen::inRange<int>(0, 4096);
            const int contact_span = *rc::gen::inRange<int>(2, 4096);
            k.contact_shadow_step_min = contact_min;
            k.contact_shadow_step_max = contact_min + contact_span;

            // Cloud bounds: effective minimum is forced to ≥ 1; size
            // the maximum off the effective low bound so the span stays ≥ 2.
            const int cloud_min_raw = *rc::gen::inRange<int>(0, 4096);
            const int cloud_eff_lo  = std::max(1, cloud_min_raw);
            const int cloud_span = *rc::gen::inRange<int>(2, 4096);
            k.cloud_step_min = cloud_min_raw;
            k.cloud_step_max = cloud_eff_lo + cloud_span;

            // SSGI maximum: ssgi_samples derives its low end as max/4, so a max
            // ≥ 8 guarantees hi - lo ≥ 2 (strictly increasing).
            k.ssgi_sample_max = *rc::gen::inRange<int>(8, 200000);

            // Foliage: generate a baseline blade count and a maximum at or above
            // it so foliage_blade_count(1.0) == baseline is not clipped by the
            // cap. density 1.0 is the Shipping_Look minimum.
            k.foliage_blade_baseline = *rc::gen::inRange<int>(0, 1000000);
            k.foliage_blade_max =
                k.foliage_blade_baseline + *rc::gen::inRange<int>(0, 1000000);

            // ──  — soft-shadow sample count: Low is the ladder minimum ──
            const int ss_low = heaven::soft_shadow_sample_count(heaven::QualityLevel::Low);
            RC_ASSERT(ss_low == 8); // fixed Shipping_Look-floor ladder (8/16/24)
            RC_ASSERT(ss_low < heaven::soft_shadow_sample_count(heaven::QualityLevel::Medium));
            RC_ASSERT(ss_low < heaven::soft_shadow_sample_count(heaven::QualityLevel::High));

            // ──  — contact-shadow steps: Low == configured minimum ──
            const int cs_low = heaven::contact_shadow_steps(heaven::QualityLevel::Low, k);
            RC_ASSERT(cs_low == k.contact_shadow_step_min);
            RC_ASSERT(cs_low < heaven::contact_shadow_steps(heaven::QualityLevel::Medium, k));
            RC_ASSERT(cs_low < heaven::contact_shadow_steps(heaven::QualityLevel::High, k));

            // ──  — cloud raymarch steps: Low == effective minimum (≥1) ──
            const int cloud_low = heaven::cloud_raymarch_steps(heaven::QualityLevel::Low, k);
            RC_ASSERT(cloud_low == std::max(1, k.cloud_step_min));
            RC_ASSERT(cloud_low < heaven::cloud_raymarch_steps(heaven::QualityLevel::Medium, k));
            RC_ASSERT(cloud_low < heaven::cloud_raymarch_steps(heaven::QualityLevel::High, k));

            // ──  — SSGI samples: Low == derived minimum (max/4) ──
            const int ssgi_hi  = std::max(2, k.ssgi_sample_max);
            const int ssgi_lo  = std::max(1, ssgi_hi / 4);
            const int ssgi_low = heaven::ssgi_samples(heaven::QualityLevel::Low, k);
            RC_ASSERT(ssgi_low == ssgi_lo);
            RC_ASSERT(ssgi_low < heaven::ssgi_samples(heaven::QualityLevel::Medium, k));
            RC_ASSERT(ssgi_low < heaven::ssgi_samples(heaven::QualityLevel::High, k));

            // ──  — foliage blade count at the 1.0 density baseline ──
            // The minimum density (1.0) returns exactly the Shipping_Look
            // baseline blade count, and any higher density is ≥ baseline.
            const int blades_baseline = heaven::foliage_blade_count(1.0f, k);
            RC_ASSERT(blades_baseline == k.foliage_blade_baseline);
            const float higher_density =
                1.0f + static_cast<float>(*rc::gen::inRange<int>(1, 100)) / 100.0f;
            RC_ASSERT(heaven::foliage_blade_count(higher_density, k) >= blades_baseline);
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_quality_knob_minimum_baseline{
    run_quality_knob_minimum_baseline_property};

// Feature: advanced-rendering-features,: Soft-shadow penumbra widens monotonically with blocker-to-receiver distance


// pins the soft-shadow penumbra width as a monotonically non-decreasing
// function of the blocker-to-receiver distance: a larger distance never
// produces a narrower penumbra. The contract lives in the pure, header-only
// penumbra_width(distance) estimator in engine/types.h (the host-testable
// estimator the soft-shadow knob feeds, per the WeatherConfig precedent), and
// the matching PCSS shader path widens the penumbra with distance by
// construction.

// asserts that estimator's two guarantees over many inputs:

//   - Monotonic non-decreasing: for any two distances d1 <= d2,
//     penumbra_width(d1) <= penumbra_width(d2). A pair is generated as a base
//     distance plus a non-negative delta so d1 <= d2 holds by construction,
//     while still ranging over negatives, zero, and large positives.

//   - Negative distances clamp to zero: penumbra_width(d) for any d <= 0 equals
//     penumbra_width(0) (the floor), so no negative distance yields a narrower-
//     than-zero (or negative) penumbra. The estimator is also never negative.

// Distances are drawn over a wide range that straddles zero (including clearly
// negative values) so both the clamp branch and the monotone-positive branch
// are exercised on every run.
bool run_penumbra_width_monotonic_property() {
    return rc::check(
        "Property 1: soft-shadow penumbra widens monotonically with "
        "blocker-to-receiver distance (Validates: Requirements 1.2)",
        [] {
            // A base distance straddling zero (in hundredths, so fractional
            // distances are covered) plus a non-negative delta gives an ordered
            // pair d1 <= d2 by construction.
            const float d1 = static_cast<float>(*rc::gen::inRange<int>(-100000, 100001)) / 100.0f;
            const float delta = static_cast<float>(*rc::gen::inRange<int>(0, 200001)) / 100.0f;
            const float d2 = d1 + delta;

            const float w1 = heaven::penumbra_width(d1);
            const float w2 = heaven::penumbra_width(d2);

            // Monotonic non-decreasing in distance: a larger distance
            // never produces a narrower penumbra.
            RC_ASSERT(w1 <= w2);

            // The penumbra width is never negative.
            RC_ASSERT(w1 >= 0.0f);
            RC_ASSERT(w2 >= 0.0f);

            // Negative distances clamp to the zero-distance floor: any d <= 0
            // yields exactly penumbra_width(0), so a negative distance is never
            // narrower than the zero baseline.
            const float w_zero = heaven::penumbra_width(0.0f);
            if (d1 <= 0.0f) {
                RC_ASSERT(w1 == w_zero);
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_penumbra_width_monotonic{
    run_penumbra_width_monotonic_property};

// Feature: advanced-rendering-features,: MSAA unsupported-count fallback picks the highest supported count ≤ request


// governs what happens when the MSAA sample count requested via the
// Quality_Knob is not actually supported by the GPU: the renderer falls back to
// the highest supported sample count at or below the request (down to 1 sample
// when nothing supported is at or below the request), rather than aborting. The
// pure, header-only msaa_fallback(requested, supported) helper in engine/types.h
// encodes that selection (the single source of truth shared with the init-time
// capability query, which feeds in the GPU-reported set and emits the
// requested-vs-applied diagnostic).

// pins the helper's full contract over many inputs:

//   - Never greater than the request: the applied count is always ≤ requested
//     (so the fallback never escalates past what was asked for).

//   - Always 1 or a member of `supported`: the result is either the 1-sample
//     floor (MSAA effectively off) or one of the actually-supported counts —
//     never an invented value.

//   - Highest supported ≤ request: the result equals max{c ∈ supported: c ≤
//     requested} whenever any such c exists; otherwise it is exactly 1. No
//     supported count that is ≤ requested is ever strictly greater than the
//     applied result (the helper genuinely picks the *highest* qualifying one).

// The generators straddle the relationship between the request and the
// supported set: a freely-drawn requested count (covering negatives, 0, the
// common {1,2,4,8,…} values, and counts above the whole set) and an arbitrary
// supported set (possibly empty, possibly with members above and below the
// request) so both the "something qualifies" and "nothing qualifies → 1"
// branches are exercised across the run.
bool run_msaa_fallback_property() {
    return rc::check(
        "Property 5: MSAA unsupported-count fallback picks the highest "
        "supported count ≤ request (Validates: Requirements 4.5)",
        [] {
            // A requested sample count straddling the realistic range: well
            // below 1, the common power-of-two counts, and above any supported
            // value. Negatives/zero exercise the "nothing qualifies" floor.
            const int requested = *rc::gen::inRange<int>(-8, 65);

            // An arbitrary supported set: sample counts the GPU reports. Kept
            // positive (a real driver never reports ≤ 0 samples) and ranging
            // both below and above the request so the highest-≤-request choice
            // is meaningful; the set may be empty to exercise the 1-sample floor.
            const std::vector<int> supported =
                *rc::gen::container<std::vector<int>>(rc::gen::inRange<int>(1, 65));

            const int applied = heaven::msaa_fallback(requested, supported);

            // Never greater than the request (the fallback never escalates).
            RC_ASSERT(applied <= requested || applied == 1);
            // More precisely: when the request is ≥ 1 the result cannot exceed
            // it; when the request is < 1 the result is pinned to the 1 floor.
            if (requested >= 1) {
                RC_ASSERT(applied <= requested);
            } else {
                RC_ASSERT(applied == 1);
            }

            // The result is always either the 1-sample floor or an actual
            // member of the supported set — never an invented value.
            const bool is_floor = (applied == 1);
            const bool is_member =
                std::find(supported.begin(), supported.end(), applied) != supported.end();
            RC_ASSERT(is_floor || is_member);

            // Compute the expected highest supported count ≤ request directly.
            int expected = 1;
            for (int c : supported) {
                if (c <= requested && c > expected) {
                    expected = c;
                }
            }
            RC_ASSERT(applied == expected);

            // No supported count that qualifies (≤ request) is ever strictly
            // greater than the applied result — i.e. the helper picks the
            // genuinely highest qualifying count.
            for (int c : supported) {
                if (c <= requested) {
                    RC_ASSERT(c <= applied);
                }
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_msaa_fallback{
    run_msaa_fallback_property};

// Feature: advanced-rendering-features,: Foliage blade count scales with density and never exceeds the configured maximum


// make massive foliage opt-in and bounded: the active blade count
// scales with the foliage density multiplier but is hard-capped at the
// configured maximum so the GPU load can never run away. The pure, header-only
// foliage_blade_count(density, knobs) mapping in engine/types.h is the single
// source of truth shared with the MassiveFoliageFeature; it computes
// clamp(round(baseline * density), 0, foliage_blade_max).

// pins that mapping's three guarantees over many inputs:

//   - Bounded above: the count never exceeds knobs.foliage_blade_max
//     for ANY density — including densities well above 1.0 that would otherwise
//     scale past the cap — and is never negative.

//   - Monotonically non-decreasing in density: for any two densities
//     d1 <= d2, foliage_blade_count(d1) <= foliage_blade_count(d2). A larger
//     density never yields fewer blades. The pair is generated as a base
//     density plus a non-negative delta so d1 <= d2 holds by construction.

//   - Baseline at density 1.0: when the baseline fits under the cap
//     (foliage_blade_baseline <= foliage_blade_max), density 1.0 returns exactly
//     the Shipping_Look baseline blade count.

// Densities are drawn over a wide range starting at 0 (no negative multipliers —
// foliage density is clamped to [1.0, max] upstream, but 0..large here also
// covers the cap-saturation branch) and the bound fields are generated with the
// baseline allowed both below and above the cap so both the "baseline fits" and
// "baseline already clipped" branches are exercised. The default QualityKnobs
// (Shipping_Look baseline) is the starting point on every case so the shipped
// configuration is always covered.
bool run_foliage_blade_count_property() {
    return rc::check(
        "Property 12: foliage blade count scales with density and never "
        "exceeds the configured maximum (Validates: Requirements 8.1, 8.2)",
        [] {
            // Start from the shipped defaults so the Shipping_Look baseline is
            // covered on every case, then override the foliage bound fields with
            // generated values to widen coverage.
            heaven::QualityKnobs k{};
            k.foliage_blade_baseline = *rc::gen::inRange<int>(0, 2000000);
            k.foliage_blade_max = *rc::gen::inRange<int>(0, 2000000);

            // An ordered density pair d1 <= d2 by construction: a non-negative
            // base (in thousandths so fractional multipliers are covered) plus a
            // non-negative delta. The range reaches well above 1.0 so the cap is
            // exercised; 0 is the floor.
            const float d1 = static_cast<float>(*rc::gen::inRange<int>(0, 50001)) / 1000.0f;
            const float delta = static_cast<float>(*rc::gen::inRange<int>(0, 50001)) / 1000.0f;
            const float d2 = d1 + delta;

            const int c1 = heaven::foliage_blade_count(d1, k);
            const int c2 = heaven::foliage_blade_count(d2, k);

            // Bounded above by the configured maximum and never negative
            // the count can never exceed foliage_blade_max for any
            // density, so the GPU load stays capped.
            RC_ASSERT(c1 >= 0);
            RC_ASSERT(c2 >= 0);
            RC_ASSERT(c1 <= k.foliage_blade_max);
            RC_ASSERT(c2 <= k.foliage_blade_max);

            // Monotonically non-decreasing in density: a larger
            // density never yields fewer blades.
            RC_ASSERT(c1 <= c2);

            // At density 1.0 the count equals the Shipping_Look baseline,
            // provided the baseline fits under the cap; when the
            // baseline already exceeds the cap it is clipped to the maximum.
            const int at_unit = heaven::foliage_blade_count(1.0f, k);
            if (k.foliage_blade_baseline <= k.foliage_blade_max) {
                RC_ASSERT(at_unit == k.foliage_blade_baseline);
            } else {
                RC_ASSERT(at_unit == k.foliage_blade_max);
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_foliage_blade_count{
    run_foliage_blade_count_property};

} // namespace
