#pragma once

#include <atomic>
#include <cstdint>

namespace heaven {

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
