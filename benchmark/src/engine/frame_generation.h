/* GPU-independent frame-generation configuration and event accounting. */
#ifndef HEAVENBENCH_SRC_ENGINE_FRAME_GENERATION_H_
#define HEAVENBENCH_SRC_ENGINE_FRAME_GENERATION_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace heaven {

enum class FrameGenerationMode : uint8_t { Off, Fsr3, Dlss };

[[nodiscard]] std::string_view frame_generation_mode_id(FrameGenerationMode mode) noexcept;
[[nodiscard]] std::optional<FrameGenerationMode> parse_frame_generation_mode(std::string_view id) noexcept;

// Availability is established by the actual provider, device and presentation
// path. Selecting a mode never manufactures a supported or active provider.
struct FrameGenerationCapability {
    FrameGenerationMode mode = FrameGenerationMode::Off;
    bool available = false;
    std::string reason = "No frame-generation provider is available";
};

struct FrameGenerationCounters {
    uint64_t real_frames = 0;
    uint64_t rejected_real_frames = 0;
    uint64_t generated_frames = 0;
    uint64_t presented_frames = 0;
    uint64_t real_presented_frames = 0;
    uint64_t generated_presented_frames = 0;
    uint64_t failed_presentations = 0;
    uint64_t confirmed_displayed_frames = 0;
    // A provider must establish complete coverage before publishing these
    // counters. A zero with unavailable coverage is not a measured zero.
    bool generated_frames_available = false;
    bool presentation_submissions_available = false;
    bool presentation_feedback_available = false;
};

struct FrameGenerationStatus {
    FrameGenerationMode requested = FrameGenerationMode::Off;
    FrameGenerationMode effective = FrameGenerationMode::Off;
    FrameGenerationCapability capability;
    FrameGenerationCounters counters;
    // Some SDKs expose only a combined real/generated native submission total.
    // This is independent of the exact split counters and is not display feedback.
    std::optional<uint64_t> sdk_presented_frames;
    std::string reason = "Disabled";
    uint64_t history_epoch = 0;
    bool history_ready = false;
};

// Owner-thread state. IDs are independent: a source ID identifies an accepted
// real render, a generated ID identifies successfully recorded interpolation
// work (GPU completion is not inferred from command recording),
// and a presentation ID identifies a successful presentation submission. None
// of these establishes that the compositor or display showed the image.
class FrameGenerationState {
public:
    void request(FrameGenerationMode mode);
    void set_capability(FrameGenerationCapability capability);
    void invalidate_history() noexcept;
    [[nodiscard]] FrameGenerationStatus status() const;

    // Rejected attempts do not advance real-frame IDs or temporal history.
    uint64_t record_real_frame(bool accepted) noexcept;
    // FSR3/DLSS 2x paths produce at most one interpolation per accepted source.
    // A reset requires two new accepted source frames before interpolation.
    uint64_t record_generated_frame(uint64_t source_frame_id) noexcept;
    uint64_t record_presented_frame(bool generated, bool submitted) noexcept;
    // Feed an actual cumulative native display-feedback count, never a render
    // or vkQueuePresent count. False rejects regressing/impossible feedback.
    bool record_display_feedback(uint64_t displayed_frames) noexcept;

private:
    void update_effective();
    FrameGenerationStatus status_;
    uint8_t history_real_frames_ = 0;
    uint64_t last_generated_source_ = 0;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_FRAME_GENERATION_H_
