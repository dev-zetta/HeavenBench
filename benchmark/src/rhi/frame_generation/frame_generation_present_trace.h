#ifndef HEAVEN_FRAME_GENERATION_PRESENT_TRACE_H
#define HEAVEN_FRAME_GENERATION_PRESENT_TRACE_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace heaven {

enum class PresentTraceKind : uint8_t { Real, Generated, Unattributed };

struct PresentTraceSubmission {
    uint64_t chain = 0;
    uint64_t native_chain = 0;
    uint64_t present_id = 0;
    uint64_t source_frame = 0;
    uint64_t submit_begin_ns = 0;
    uint64_t submit_end_ns = 0;
    PresentTraceKind kind = PresentTraceKind::Unattributed;
    int32_t present_mode = INT32_MAX;
    int32_t queue_result = 0;
};

// Explicit diagnostic, default off. The adapter supplies vkWaitForPresentKHR;
// this observer owns no GPU objects and never waits on the submitting thread.
// Completion bounds describe the KHR present-ID signal, not physical scanout.
class FrameGenerationPresentTrace final {
public:
    using Wait = int32_t (*)(void*, uint64_t native_chain, uint64_t present_id,
            uint64_t timeout_ns);
    FrameGenerationPresentTrace();
    explicit FrameGenerationPresentTrace(std::filesystem::path output);
    ~FrameGenerationPresentTrace();
    bool requested() const noexcept;
    bool available() const noexcept;
    void configure(bool available, std::string reason, Wait wait = nullptr,
            void* userdata = nullptr);
    // Ordinary publication supports concurrent producers. Configuration,
    // finish and destruction must be serialized after all producers drain.
    void submitted(const PresentTraceSubmission& submission) noexcept;
    // Join the old observer before retiring ANY observed native swapchain or
    // device. Ready records remain retained with explicit unresolved status.
    // An idle replacement is registered here; only a later submission after
    // successful recreation can provide a new borrowed handle to observe.
    void finish(const char* reason) noexcept;
    static uint64_t now_ns() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven

#endif // HEAVEN_FRAME_GENERATION_PRESENT_TRACE_H
