#ifndef HEAVEN_FRAME_GENERATION_CAPTURE_H
#define HEAVEN_FRAME_GENERATION_CAPTURE_H

#include <bluevk/BlueVK.h>

#include <cstdint>
#include <memory>
#include <string>

namespace heaven {

// Explicit diagnostic readback, never part of ordinary performance captures.
// HEAVEN_FG_CAPTURE_DIR opts in to at most four native generated/real pairs.
// HEAVEN_FG_CAPTURE_START_SOURCE selects the first eligible source (default 1).
// HEAVEN_FG_CAPTURE_UI_INPUTS=1 additionally retains the exact pre-UI generated
// image and raw UI layer, within the same 128 MiB diagnostic allocation limit.
class FrameGenerationCapture final {
public:
    FrameGenerationCapture();
    ~FrameGenerationCapture();
    bool enabled() const noexcept;
    bool wants_source(uint64_t source_frame) const noexcept;
    bool ui_inputs_enabled() const noexcept;
    // Inputs must both be RGBA8_UNORM at the native extent. Preparation allocates
    // bounded storage; recording only copies TRANSFER_SRC_OPTIMAL images. The
    // caller validates image usage/layout and restores layouts after recording.
    bool prepare_ui_inputs(VkDevice device, VkPhysicalDevice physical, VkExtent2D size,
            VkFormat generated_format, VkFormat ui_format, uint64_t source_frame);
    bool record_ui_inputs(VkCommandBuffer commands, VkImage generated_before_ui, VkImage ui_layer);
    // Commit after successful recording and arming of this source. This does
    // not establish submission or completion; export still waits on the consumer
    // fence. Abandon discarded/unarmed work without freeing GPU storage.
    bool commit_ui_inputs(uint64_t source_frame) noexcept;
    void abandon_ui_inputs(uint64_t source_frame = 0) noexcept;
    bool begin_pair(VkDevice device, VkPhysicalDevice physical, VkExtent2D size,
            VkFormat format, uint64_t source_frame);
    // Images are already in TRANSFER_SRC_OPTIMAL; records only image-to-buffer.
    void record_generated(VkCommandBuffer commands, VkImage image);
    void record_real(VkCommandBuffer commands, VkImage image);
    // The real consumer fence covers all earlier same-queue readbacks. Waits and
    // writes only in this explicitly enabled diagnostic mode. Extended captures
    // publish a receipt only when the committed input source matches the pair.
    bool finish_pair(VkFence real_consumer, uint64_t generated_present_ns,
            uint64_t real_present_ns);
    std::string last_error() const;
    void release_after_drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven

#endif // HEAVEN_FRAME_GENERATION_CAPTURE_H
