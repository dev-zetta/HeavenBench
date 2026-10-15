#ifndef HEAVEN_FRAME_GENERATION_HISTORY_H
#define HEAVEN_FRAME_GENERATION_HISTORY_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#ifdef __linux__
#include <ctime>
#endif

namespace heaven {

// Linux steady_clock excludes physical sleep. Keep history on a separate,
// suspend-inclusive monotonic clock; do not change profiling or pacing clocks.
// A failed read is an invalid sample, never a switch to another clock domain.
struct FrameGenerationHistoryClock {
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<FrameGenerationHistoryClock, duration>;

    static time_point now() noexcept {
#ifdef __linux__
        timespec value{};
        constexpr auto billion = 1'000'000'000LL;
        constexpr auto limit = std::numeric_limits<duration::rep>::max();
        if (::clock_gettime(CLOCK_BOOTTIME, &value) != 0 || value.tv_sec < 0 ||
                value.tv_nsec < 0 || value.tv_nsec >= billion ||
                value.tv_sec > (limit - value.tv_nsec) / billion)
            return time_point(duration(-1));
        return time_point(duration(value.tv_sec * billion + value.tv_nsec));
#else
        return time_point(std::chrono::duration_cast<duration>(
            std::chrono::steady_clock::now().time_since_epoch()));
#endif
    }

    static bool valid(time_point value) noexcept {
        return value.time_since_epoch().count() > 0;
    }
};

// Prepared packets can remain queued while every application thread is paused.
// Invalid/rolled-back samples also reset; a failed read must not make a later
// successful read appear to be uninitialized history. Zero alone means unset.
inline bool frame_generation_callback_gap(FrameGenerationHistoryClock::time_point previous,
        FrameGenerationHistoryClock::time_point current) noexcept {
    return !FrameGenerationHistoryClock::valid(current) ||
        previous.time_since_epoch().count() < 0 ||
        (previous.time_since_epoch().count() != 0 &&
            (current < previous || current - previous > std::chrono::milliseconds(250)));
}

// Native swapchain invalidation must reach the driver callback even when the
// application has already queued its frame and has not consumed status events.
class FrameGenerationHistoryEpoch {
public:
    uint64_t current() const noexcept { return epoch_.load(std::memory_order_acquire); }
    void invalidate() noexcept { epoch_.fetch_add(1, std::memory_order_release); }
private:
    std::atomic<uint64_t> epoch_{1};
};

// Driver-thread state: compare the current native epoch immediately before SDK
// recording, retaining explicit camera/resize resets from the application.
class FrameGenerationHistoryLatch {
public:
    bool should_reset(bool explicit_reset, uint64_t native_epoch) noexcept {
        const bool changed = native_epoch != previous_epoch_;
        previous_epoch_ = native_epoch;
        return explicit_reset || changed;
    }
private:
    uint64_t previous_epoch_ = 0;
};

} // namespace heaven

#endif // HEAVEN_FRAME_GENERATION_HISTORY_H
