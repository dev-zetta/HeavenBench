#include "frame_generation_history.h"

#include <cstdio>

namespace {
bool check(bool value, const char* reason) {
    if (!value) std::fprintf(stderr, "Frame-generation history: FAIL: %s\n", reason);
    return value;
}
}

int main() {
    heaven::FrameGenerationHistoryEpoch native;
    heaven::FrameGenerationHistoryLatch callback;
    bool ok = check(callback.should_reset(true, native.current()), "first source did not retain explicit startup reset");
    ok &= check(!callback.should_reset(false, native.current()), "unchanged native history reset repeatedly");

    // Application prepares this packet before acquire discovers an out-of-date
    // swapchain. No application status/event consumer runs before the callback.
    constexpr bool queued_packet_reset = false;
    const auto before_recreate = native.current();
    native.invalidate();
    ok &= check(native.current() > before_recreate, "native recreation did not publish a new epoch");
    ok &= check(callback.should_reset(queued_packet_reset, native.current()),
        "queued non-reset packet reused history after implicit same-size recreation");
    ok &= check(!callback.should_reset(false, native.current()), "native recreation reset did not latch");

    native.invalidate();
    native.invalidate();
    ok &= check(callback.should_reset(false, native.current()), "multiple native recreations were missed");
    ok &= check(!callback.should_reset(false, native.current()), "multiple recreations caused a persistent reset");
    ok &= check(callback.should_reset(true, native.current()), "explicit same-size resize/camera reset was lost");
    ok &= check(!callback.should_reset(false, native.current()), "explicit reset changed the native epoch");
    // The platform also publishes an invalidation on failed native submission.
    // A retry may already have been queued before its status event is consumed.
    native.invalidate();
    ok &= check(callback.should_reset(queued_packet_reset, native.current()),
        "queued retry reused history after a failed native submission");
    ok &= check(!callback.should_reset(false, native.current()), "successful retry kept resetting history");
    if (!ok) return 1;
    std::puts("Frame-generation history: PASS (queued packets, implicit epochs, explicit resets, failed submission retries)");
}
