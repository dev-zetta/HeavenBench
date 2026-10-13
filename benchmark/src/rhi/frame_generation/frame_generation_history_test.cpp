#include "frame_generation_history.h"

#include <cstdio>

namespace {
bool check(bool value, const char* reason) {
    if (!value) std::fprintf(stderr, "Frame-generation history: FAIL: %s\n", reason);
    return value;
}
}

int main() {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    const auto callback = Clock::time_point(1s);
    bool ok = check(!heaven::frame_generation_callback_gap({}, callback), "first callback invented a long gap");
    ok &= check(!heaven::frame_generation_callback_gap(callback, callback + 250ms), "threshold callback reset unnecessarily");
    ok &= check(heaven::frame_generation_callback_gap(callback, callback + 250ms + 1ns), "queued callback gap escaped reset");
    ok &= check(heaven::frame_generation_callback_gap(callback, callback + 1s), "process-pause callback gap escaped reset");
    ok &= check(!heaven::frame_generation_callback_gap(callback + 1s, callback + 1s + 16ms), "callback history did not recover");
    ok &= check(!heaven::frame_generation_callback_gap(callback + 1s, callback), "future callback timestamp invented a gap");
    heaven::FrameGenerationHistoryEpoch native;
    heaven::FrameGenerationHistoryLatch latch;
    ok &= check(latch.should_reset(true, native.current()), "first source did not retain explicit startup reset");
    ok &= check(!latch.should_reset(false, native.current()), "unchanged native history reset repeatedly");

    // Application prepares this packet before acquire discovers an out-of-date
    // swapchain. No application status/event consumer runs before the callback.
    constexpr bool queued_packet_reset = false;
    const auto before_recreate = native.current();
    native.invalidate();
    ok &= check(native.current() > before_recreate, "native recreation did not publish a new epoch");
    ok &= check(latch.should_reset(queued_packet_reset, native.current()),
        "queued non-reset packet reused history after implicit same-size recreation");
    ok &= check(!latch.should_reset(false, native.current()), "native recreation reset did not latch");

    native.invalidate();
    native.invalidate();
    ok &= check(latch.should_reset(false, native.current()), "multiple native recreations were missed");
    ok &= check(!latch.should_reset(false, native.current()), "multiple recreations caused a persistent reset");
    ok &= check(latch.should_reset(true, native.current()), "explicit same-size resize/camera reset was lost");
    ok &= check(!latch.should_reset(false, native.current()), "explicit reset changed the native epoch");
    // The platform also publishes an invalidation on failed native submission.
    // A retry may already have been queued before its status event is consumed.
    native.invalidate();
    ok &= check(latch.should_reset(queued_packet_reset, native.current()),
        "queued retry reused history after a failed native submission");
    ok &= check(!latch.should_reset(false, native.current()), "successful retry kept resetting history");
    if (!ok) return 1;
    std::puts("Frame-generation history: PASS (queued packets, implicit epochs, explicit resets, failed submission retries)");
}
