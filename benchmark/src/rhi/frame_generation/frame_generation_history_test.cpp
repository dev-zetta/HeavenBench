#include <cstdio>
#ifdef __linux__
#include <ctime>
namespace {
bool simulate_clock = false;
int clock_result = 0;
timespec boot_time{1, 0};
timespec awake_time{1, 0};
int history_clock_gettime(clockid_t clock, timespec* value) noexcept {
    if (!simulate_clock) return ::clock_gettime(clock, value);
    *value = clock == CLOCK_BOOTTIME ? boot_time : awake_time;
    return clock_result;
}
}
// Interpose only the OS read in the actual production header. The independent
// boot/awake clocks model sleep without also inventing an awake long gap.
#define clock_gettime history_clock_gettime
#endif
#include "frame_generation_history.h"
#ifdef __linux__
#undef clock_gettime
#endif

namespace {
bool check(bool value, const char* reason) {
    if (!value) std::fprintf(stderr, "Frame-generation history: FAIL: %s\n", reason);
    return value;
}
}

int main() {
    using namespace std::chrono_literals;
    using Clock = heaven::FrameGenerationHistoryClock;
    const auto callback = Clock::time_point(1s);
    bool ok = check(!heaven::frame_generation_callback_gap({}, callback), "first callback invented a long gap");
    ok &= check(!heaven::frame_generation_callback_gap(callback, callback + 250ms), "threshold callback reset unnecessarily");
    ok &= check(heaven::frame_generation_callback_gap(callback, callback + 250ms + 1ns), "queued callback gap escaped reset");
    ok &= check(heaven::frame_generation_callback_gap(callback, callback + 1s), "process-pause callback gap escaped reset");
    ok &= check(!heaven::frame_generation_callback_gap(callback + 1s, callback + 1s + 16ms), "callback history did not recover");
    ok &= check(heaven::frame_generation_callback_gap(callback + 1s, callback), "clock rollback reused temporal history");
    ok &= check(Clock::valid(Clock::now()), "native history clock unavailable");
#ifdef __linux__
    simulate_clock = true;
    const auto before_sleep = Clock::now();
    boot_time.tv_sec += 10;
    awake_time.tv_nsec += 16'000'000;
    const auto after_sleep = Clock::now();
    ok &= check(after_sleep - before_sleep == 10s &&
        heaven::frame_generation_callback_gap(before_sleep, after_sleep),
        "physical sleep escaped reset while the awake clock advanced only 16ms");
    boot_time.tv_nsec = 16'000'000;
    ok &= check(!heaven::frame_generation_callback_gap(after_sleep, Clock::now()),
        "sleep-inclusive history did not recover");
    clock_result = -1;
    const auto failed = Clock::now();
    ok &= check(!Clock::valid(failed) && heaven::frame_generation_callback_gap(after_sleep, failed),
        "failed clock read reused temporal history");
    clock_result = 0;
    ok &= check(heaven::frame_generation_callback_gap(failed, Clock::now()),
        "clock recovery forgot the invalid prior sample");
    boot_time.tv_nsec = 1'000'000'000;
    ok &= check(!Clock::valid(Clock::now()), "invalid native nanoseconds accepted");
    boot_time = {-1, 0};
    ok &= check(!Clock::valid(Clock::now()), "negative native time accepted");
    constexpr auto overflow_seconds =
        (std::numeric_limits<Clock::duration::rep>::max() / 1'000'000'000) * 2 + 2;
    if constexpr (std::numeric_limits<decltype(boot_time.tv_sec)>::max() >= overflow_seconds) {
        // A naive signed multiply can wrap to a positive value here, so the
        // validity predicate alone cannot accidentally hide the missing guard.
        boot_time = {overflow_seconds, 0};
        ok &= check(!Clock::valid(Clock::now()), "overflowed native time accepted");
    }
    simulate_clock = false;
#endif
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
