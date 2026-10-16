#ifndef HEAVENBENCH_SRC_BENCHMARK_INPUT_RESPONSE_H_
#define HEAVENBENCH_SRC_BENCHMARK_INPUT_RESPONSE_H_

#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace heaven {

enum class InputResponseKind { UiToggle, CameraLook };

// Main-thread-only diagnostic. Storage is allocated once; recording never
// allocates or waits. Times are host steady-clock observations, not scanout.
class InputResponseRecorder {
public:
    static constexpr std::size_t event_capacity = 4096;
    static constexpr std::size_t frame_capacity = 64;

    InputResponseRecorder();
    ~InputResponseRecorder();
    InputResponseRecorder(const InputResponseRecorder&) = delete;
    InputResponseRecorder& operator=(const InputResponseRecorder&) = delete;

    [[nodiscard]] static std::int64_t now_ns();
    void set_initial_state(bool ui, double yaw, double pitch);
    [[nodiscard]] bool record(InputResponseKind kind, std::int64_t consumed_ns,
        double x, double y = 0);
    void set_response_state(bool ui, double yaw, double pitch);
    void accepted_frame(std::uint64_t attempt, std::uint64_t submission,
        std::int64_t begin_lo, std::int64_t begin_hi, bool ui_rendered);
    void frontend_complete(std::uint64_t submission, std::int64_t ns);
    void gpu_frame(std::uint32_t filament_id, std::int64_t begin_ns,
        std::int64_t backend_end_ns, std::int64_t gpu_complete_ns);
    void history_missing();
    // Finalizes unresolved observations and clears the baseline. Reload can
    // set_initial_state(), or establish it with an accepted warmup frame.
    void retire_pending(const char* reason);
    [[nodiscard]] nlohmann::json report() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven
#endif
