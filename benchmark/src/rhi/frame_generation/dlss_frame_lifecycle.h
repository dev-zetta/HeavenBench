#ifndef HEAVEN_DLSS_FRAME_LIFECYCLE_H
#define HEAVEN_DLSS_FRAME_LIFECYCLE_H

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>

namespace heaven {

struct DlssTokenReservation {
    uint64_t logical_frame = 0;
};

// Streamline 2.14.1 returns one of six mutable native token objects. A slot
// cannot rotate again while a packet, presentation or tag owner still uses it.
class DlssTokenReservations {
public:
    explicit DlssTokenReservations(uint64_t first_frame = 1) : next_frame_(first_frame) {}
    std::shared_ptr<const DlssTokenReservation> reserve() {
        const std::scoped_lock lock(mutex_);
        if (blocked_ || !next_frame_ || next_frame_ > std::numeric_limits<uint32_t>::max()) return {};
        if (!slots_[next_slot_].expired()) return {};
        auto lease = std::make_shared<const DlssTokenReservation>(DlssTokenReservation{next_frame_});
        slots_[next_slot_] = lease;
        next_slot_ = (next_slot_ + 1) % slots_.size();
        ++next_frame_;
        return lease;
    }
    void disable() {
        const std::scoped_lock lock(mutex_);
        blocked_ = true;
    }
    bool exhausted() const {
        const std::scoped_lock lock(mutex_);
        return !next_frame_ || next_frame_ > std::numeric_limits<uint32_t>::max();
    }
private:
    mutable std::mutex mutex_;
    std::array<std::weak_ptr<const DlssTokenReservation>, 6> slots_;
    size_t next_slot_ = 0;
    uint64_t next_frame_;
    bool blocked_ = false;
};

struct DlssFrameToken {
    void* native_token = nullptr;
    // Logical prospective source identity; independent of application attempts.
    uint64_t application_frame = 0;
    uint32_t stream_frame = 0;
    std::shared_ptr<const DlssTokenReservation> reservation;
};

enum class DlssLatencyMarker : uint8_t {
    SimulationStart, SimulationEnd, RenderSubmitStart, RenderSubmitEnd,
    PresentStart, PresentEnd, TriggerFlash, LatencyPing
};

// One prospective rendered source can include several rejected application
// attempts. Keep its token and simulation interval until a render is accepted.
class DlssProspectiveFrame {
public:
    template<class Acquire> const DlssFrameToken& begin(Acquire&& acquire) {
        if (token_.native_token) return token_;
        token_ = acquire();
        return token_;
    }

    template<class Mark> void begin_submission(Mark&& mark) {
        if (!token_.native_token || submission_started_) return;
        // Mark each boundary once even if an SDK call fails. The provider
        // records that failure and refuses native preparation for this token.
        submission_started_ = true;
        mark(token_, DlssLatencyMarker::SimulationEnd);
        mark(token_, DlssLatencyMarker::RenderSubmitStart);
    }

    const DlssFrameToken& current() const { return token_; }
    DlssFrameToken accept() {
        auto accepted = token_;
        cancel();
        return accepted;
    }
    void cancel() {
        token_ = {};
        submission_started_ = false;
    }
private:
    DlssFrameToken token_;
    bool submission_started_ = false;
};

} // namespace heaven

#endif // HEAVEN_DLSS_FRAME_LIFECYCLE_H
