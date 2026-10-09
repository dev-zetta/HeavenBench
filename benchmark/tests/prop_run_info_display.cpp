// Feature: benchmark-scoring-system,
/*
 * HeavenBench — Property test for Display_Mode_Description formatting.
 *
 * Feature: benchmark-scoring-system
 *   •: Display_Mode_Description formats resolution, AA, and window
 *     state by their bounds
 *
 *
 * `heaven::format_display_mode(const DisplayRaw&)` assembles the
 * Display_Mode_Description string "<w>x<h> <aa> <windowed|fullscreen>"
 * The formatter is PURE (standard library only, no
 * Filament/SDL/OS dependency), so this exercises it directly on the host.
 *
 *
 *   For all DisplayRaw inputs:
 *     • Resolution group: when width and height are both positive integers
 *       (>= 1) the description begins with "<w>x<h>", otherwise that group is
 *       the per-datum placeholder token (kUnknownDisplay).
 *     • Anti-aliasing token: "no AA" exactly when aa_samples == 1, "<n>xAA"
 *       for a sample count in [2, 64], and the placeholder token when the
 *       count is outside [1, 64].
 *     • Window-state token: "fullscreen" when the fullscreen flag is set,
 *       "windowed" otherwise.
 *     • The result is never blank.
 *
 * The generators deliberately span each datum below / at / above its bound:
 * width and height take a non-positive value, exactly 1, or a value > 1;
 * aa_samples takes a value < 1, exactly 1, a value in [2, 64], or a value > 64.
 * This guarantees every branch of the formatter is visited across iterations.
 *
 * The file self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the ">= 100
 * iterations" requirement (raise via RC_PARAMS=max_success=...).
 */
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidcheck.h>

#include "benchmark/run_info.h"

// Mirror the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using heaven::DisplayRaw;
using heaven::format_display_mode;
using heaven::kUnknownDisplay;

// Generate a dimension (width or height) spanning the positivity bound:
//   category 0 → non-positive (<= 0, undetermined)
//   category 1 → exactly 1    (the lower edge of "positive")
//   category 2 → > 1          (clearly positive)
int gen_dimension() {
    const int category = *rc::gen::inRange(0, 3);
    switch (category) {
        case 0:  return *rc::gen::inRange(-1000, 1);     // [-1000, 0]
        case 1:  return 1;
        default: return *rc::gen::inRange(2, 100001);    // [2, 100000]
    }
}

// Generate an anti-aliasing sample count spanning the [1, 64] bounds:
//   category 0 → < 1   (below the valid range, undetermined)
//   category 1 → == 1  ("no AA")
//   category 2 → [2, 64] ("<n>xAA")
//   category 3 → > 64  (above the valid range, undetermined)
int gen_aa_samples() {
    const int category = *rc::gen::inRange(0, 4);
    switch (category) {
        case 0:  return *rc::gen::inRange(-1000, 1);     // [-1000, 0]
        case 1:  return 1;
        case 2:  return *rc::gen::inRange(2, 65);        // [2, 64]
        default: return *rc::gen::inRange(65, 100001);   // [65, 100000]
    }
}

// The expected resolution group for the given dimensions.
std::string expected_resolution(int width, int height) {
    if (width >= 1 && height >= 1) {
        return std::to_string(width) + "x" + std::to_string(height);
    }
    return {kUnknownDisplay};
}

// The expected anti-aliasing token for the given sample count.
std::string expected_aa(int aa_samples) {
    if (aa_samples == 1) return "no AA";
    if (aa_samples >= 2 && aa_samples <= 64) return std::to_string(aa_samples) + "xAA";
    return {kUnknownDisplay};
}

bool run_p18_display_mode_property() {
    return rc::check(
        "Feature: benchmark-scoring-system, Property 18: Display_Mode_Description "
        "formats resolution, AA, and window state by their bounds "
        "(Validates: Requirements 13.3)",
        [] {
            DisplayRaw raw;
            raw.width      = gen_dimension();
            raw.height     = gen_dimension();
            raw.aa_samples = gen_aa_samples();
            raw.fullscreen = *rc::gen::arbitrary<bool>();

            const std::string result = format_display_mode(raw);

            // The result is never blank (never-blank guarantee).
            RC_ASSERT(!result.empty());

            // ── Resolution group ──
            // Begins with "<w>x<h>" when both dimensions are positive, else with
            // the placeholder token.
            const std::string resolution = expected_resolution(raw.width, raw.height);
            RC_ASSERT(result.rfind(resolution, 0) == 0u);
            if (raw.width >= 1 && raw.height >= 1) {
                // The "<w>x<h>" prefix is present (not the placeholder).
                RC_ASSERT(result.rfind(std::string(kUnknownDisplay), 0) != 0u);
            }

            // ── Anti-aliasing token ──
            const std::string aa = expected_aa(raw.aa_samples);

            // ── Window-state token ──
            const std::string window = raw.fullscreen ? "fullscreen" : "windowed";

            // The fully assembled description is exactly the three groups joined
            // by single spaces. Asserting equality simultaneously verifies the
            // prefix, the AA token, and the window token.
            const std::string expected = resolution + " " + aa + " " + window;
            RC_ASSERT(result == expected);

            // Explicit token presence (redundant with the equality above, but
            // states each acceptance sub-clause directly).
            RC_ASSERT(result.find(aa) != std::string::npos);
            const std::string_view tail(result);
            RC_ASSERT(tail.size() >= window.size() &&
                      tail.substr(tail.size() - window.size()) == window);
        });
}

const heaven::testing::Register g_register_run_info_display{
    &run_p18_display_mode_property};

} // namespace
