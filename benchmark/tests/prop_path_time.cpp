/*
 * Playback time accumulates 1:1 with elapsed frame time.
 *
 *
 * A CameraPathPlayer owns the playback clock for a run. After reset() the clock
 * is zero; each advance(dt) adds dt to the clock with NO scaling (1:1 with real
 * elapsed time). So for any sequence of advanced deltas, the
 * reported time() must equal the running sum of those deltas.
 *
 * The player's clock is independent of the path contents — advance()/time()
 * only touch the float clock — so a default CameraPath{} is a valid object to
 * construct the player over. The expected sum is accumulated as a float in the
 * SAME order the implementation adds them (time_ += dt), mirroring the
 * accumulation exactly; a small relative+absolute epsilon absorbs any residual
 * float rounding over a long sequence of adds. Because the deltas are typical
 * positive frame times, an applied scale factor of anything other than exactly
 * 1 would push time() outside this epsilon, so the test proves no scaling.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB)
 * in benchmark/CMakeLists.txt picks it up without editing the entry point.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

#include <rapidcheck.h>

#include "engine/camera_path.h"

namespace heaven::testing {

// Mirrors the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is identical across test TUs (ODR-safe).
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Relative comparison with an absolute floor. The expected sum is accumulated
// operation-for-operation like the player, so the difference is typically zero;
// the epsilon only guards against residual rounding over a long add sequence
// while staying far tighter than any non-unit scale factor would produce.
bool approx_equal(float a, float b) {
    const float diff  = std::abs(a - b);
    const float scale = std::max({1.0f, std::abs(a), std::abs(b)});
    return diff <= 1e-5f * scale;
}

bool run_path_time_property() {
    // RapidCheck's default is 100 iterations (maxSuccess = 100), meeting the
    // "≥ 100 iterations" requirement; it can be raised via
    // RC_PARAMS="max_success=...".
    return rc::check(
        "Property 4: playback time accumulates 1:1 with elapsed frame time "
        "(Validates: Requirements 2.2)",
        [] {
            // Bounded, positive frame-time deltas in [0, 0.1] seconds — the
            // realistic domain for elapsed frame time — keeps the running float
            // sum well-conditioned. Generated in milliseconds so values land on
            // clean increments across the whole range.
            const auto delta_gen =
                rc::gen::map(rc::gen::inRange<int>(0, 101),
                             [](int milli) {
                                 return static_cast<float>(milli) / 1000.0f;
                             });

            // A sequence of advances; lengths range from empty up to a long run
            // so accumulation over many adds is exercised.
            const std::vector<float> deltas =
                *rc::gen::container<std::vector<float>>(delta_gen);

            // The clock is independent of the path; a default-constructed path
            // is a valid object to wrap.
            const heaven::CameraPath path{};
            heaven::CameraPathPlayer player(&path);
            player.reset();

            // time() starts at zero after reset() (baseline).
            RC_ASSERT(player.time() == 0.0f);

            // Advance the player and mirror the accumulation as a float in the
            // same order the implementation uses (time_ += dt, no scaling).
            float expected = 0.0f;
            for (float dt : deltas) {
                player.advance(dt);
                expected += dt;
            }

            // time() == Σ advanced deltas, with no scaling.
            RC_ASSERT(approx_equal(player.time(), expected));
        });
}

const heaven::testing::Register reg_path_time{&run_path_time_property};

} // namespace
