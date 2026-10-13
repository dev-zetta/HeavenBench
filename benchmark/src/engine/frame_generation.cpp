#include "engine/frame_generation.h"

#include <utility>

namespace heaven {

std::string_view frame_generation_mode_id(FrameGenerationMode mode) noexcept {
    switch (mode) {
        case FrameGenerationMode::Off: return "off";
        case FrameGenerationMode::Fsr3: return "fsr3";
        case FrameGenerationMode::Dlss: return "dlss";
    }
    return "off";
}

std::optional<FrameGenerationMode> parse_frame_generation_mode(std::string_view id) noexcept {
    if (id == "off") return FrameGenerationMode::Off;
    if (id == "fsr3") return FrameGenerationMode::Fsr3;
    if (id == "dlss") return FrameGenerationMode::Dlss;
    return std::nullopt;
}

void FrameGenerationState::request(FrameGenerationMode mode) {
    if (status_.requested != mode) invalidate_history();
    status_.requested = mode;
    update_effective();
}

void FrameGenerationState::set_capability(FrameGenerationCapability capability) {
    const bool changed = status_.capability.mode != capability.mode ||
                         status_.capability.available != capability.available;
    status_.capability = std::move(capability);
    if (changed) invalidate_history();
    update_effective();
}

void FrameGenerationState::update_effective() {
    status_.effective = FrameGenerationMode::Off;
    if (status_.requested == FrameGenerationMode::Off) {
        status_.reason = "Disabled";
    } else if (status_.capability.mode != status_.requested) {
        status_.reason = "The selected frame-generation provider is unavailable";
    } else if (!status_.capability.available) {
        status_.reason = status_.capability.reason.empty() ?
            "The selected frame-generation provider is unavailable" : status_.capability.reason;
    } else {
        status_.effective = status_.requested;
        status_.reason = status_.capability.reason.empty() ? "Available" : status_.capability.reason;
    }
}

void FrameGenerationState::invalidate_history() noexcept {
    ++status_.history_epoch;
    status_.history_ready = false;
    history_real_frames_ = 0;
    last_generated_source_ = status_.counters.real_frames;
}

FrameGenerationStatus FrameGenerationState::status() const {
    return status_;
}

uint64_t FrameGenerationState::record_real_frame(bool accepted) noexcept {
    if (!accepted) {
        ++status_.counters.rejected_real_frames;
        return 0;
    }
    if (history_real_frames_ < 2) ++history_real_frames_;
    status_.history_ready = history_real_frames_ == 2;
    return ++status_.counters.real_frames;
}

uint64_t FrameGenerationState::record_generated_frame(uint64_t source_frame_id) noexcept {
    if (status_.effective == FrameGenerationMode::Off || !status_.history_ready ||
        source_frame_id == 0 || source_frame_id != status_.counters.real_frames ||
        source_frame_id <= last_generated_source_) return 0;
    last_generated_source_ = source_frame_id;
    return ++status_.counters.generated_frames;
}

uint64_t FrameGenerationState::record_presented_frame(bool generated, bool submitted) noexcept {
    const bool produced = generated ?
        status_.counters.generated_presented_frames < status_.counters.generated_frames :
        status_.counters.real_presented_frames < status_.counters.real_frames;
    if (!submitted || !produced) {
        ++status_.counters.failed_presentations;
        return 0;
    }
    if (generated) ++status_.counters.generated_presented_frames;
    else ++status_.counters.real_presented_frames;
    return ++status_.counters.presented_frames;
}

bool FrameGenerationState::record_display_feedback(uint64_t displayed_frames) noexcept {
    if (displayed_frames < status_.counters.confirmed_displayed_frames ||
        displayed_frames > status_.counters.presented_frames) return false;
    status_.counters.confirmed_displayed_frames = displayed_frames;
    status_.counters.presentation_feedback_available = true;
    return true;
}

} // namespace heaven
