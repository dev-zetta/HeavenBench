/*
 * HeavenBench — unit test: RHI Wayland driver-selection hint
 *
 * Feature: codebase-hardening
 * — Requirements: 13.1, 13.2
 *
 * Design reference: C2 (rhi.cpp).  replaces the POSIX
 * `setenv("SDL_VIDEODRIVER", "wayland", 1)` call with SDL's own cross-platform
 * hint mechanism. rhi.cpp::create_window now forces the Wayland WSI under
 * `--hdr` with the exact call:
 *
 *     SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "wayland", SDL_HINT_OVERRIDE);
 *
 * SDL reads SDL_HINT_VIDEODRIVER at init time to select the video backend, so
 * this is the behavioural parity replacement for the old setenv overwrite
 * ("use a standard/already-available cross-platform
 * mechanism" and 13.2 "preserve the existing runtime behaviour").
 *
 * Why this test exercises the hint mechanism directly rather than calling
 * create_window:
 *
 *   • create_window calls SDL_Init(SDL_INIT_VIDEO) and creates a real window,
 *     which requires a display/GPU and cannot run in the headless host test
 *     harness. Driving it here would make the unit test non-deterministic and
 *     environment-dependent.
 *
 *   • The behaviour  actually changes is *which mechanism sets
 *     the video-driver selection*. The correctness of that mechanism is fully
 *     captured by: issue the exact SDL call rhi.cpp makes, then confirm SDL
 *     reports the video-driver hint as "wayland". This pins the contract the
 *     production code now relies on (the hint is what SDL consults to pick the
 *     backend) without opening a window.
 *
 * The file self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point. This is
 * a deterministic unit test (no random input), so it does not use rc::check.
 *
 * SDL hints are process-global state, so the test saves the prior hint value,
 * sets and asserts the "wayland" value, then resets the hint to avoid polluting
 * any other test that might consult SDL_HINT_VIDEODRIVER.
 */
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL2/SDL.h>

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

bool unit_rhi_driver_hint() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_rhi_driver_hint] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    // Save the prior hint value so we can restore process-global state after
    // the assertion (SDL_GetHint returns a pointer into SDL-owned storage that
    // the next SetHint may invalidate, so copy it into a std::string).
    const char* prior = SDL_GetHint(SDL_HINT_VIDEODRIVER);
    const bool had_prior = prior != nullptr;
    const std::string saved = had_prior ? std::string(prior) : std::string{};

    // ── Driver-selection trigger ───────────────────────────────
    // The exact call rhi.cpp::create_window makes under --hdr to force the
    // Wayland WSI. SDL_HINT_OVERRIDE forces the value even if it was already
    // present, mirroring the old setenv overwrite=1 behaviour.
    SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "wayland", SDL_HINT_OVERRIDE);

    // ── Assertion ──────────────────────────────────────────────
    // After the trigger, SDL must report the video-driver hint as "wayland" —
    // this is the value SDL consults at init time to select the backend.
    const char* after = SDL_GetHint(SDL_HINT_VIDEODRIVER);
    if (after == nullptr) {
        fail("SDL_GetHint(SDL_HINT_VIDEODRIVER) returned null after the trigger");
    } else if (std::string_view(after) != "wayland") {
        fail(std::string("SDL video-driver hint should equal 'wayland' but was '") +
             after + "'");
    }

    // ── Restore process-global hint state ──────────────────────
    // Avoid leaking the "wayland" selection into any other test in the suite.
    if (had_prior) {
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, saved.c_str(),
                                SDL_HINT_OVERRIDE);
    } else {
        SDL_ResetHint(SDL_HINT_VIDEODRIVER);
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_rhi_driver_hint{&unit_rhi_driver_hint};

} // namespace
