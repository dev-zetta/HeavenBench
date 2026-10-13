#ifndef HEAVEN_FRAME_GENERATION_HISTORY_H
#define HEAVEN_FRAME_GENERATION_HISTORY_H

#include <atomic>
#include <chrono>
#include <cstdint>

namespace heaven {

// Prepared packets can remain queued while every application thread is paused.
// Check the native callback clock as well as the frontend's source clock.
inline bool frame_generation_callback_gap(std::chrono::steady_clock::time_point previous,
        std::chrono::steady_clock::time_point current) noexcept {
    return previous.time_since_epoch().count() != 0 &&
        current - previous > std::chrono::milliseconds(250);
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
