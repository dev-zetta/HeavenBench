/*
 * HeavenBench — preview-only submission deadlines (no renderer dependency)
 */
#ifndef HEAVENBENCH_SRC_ENGINE_PREVIEW_FRAME_PACING_H_
#define HEAVENBENCH_SRC_ENGINE_PREVIEW_FRAME_PACING_H_

#include <chrono>
#include <optional>

namespace heaven {

[[nodiscard]] constexpr bool preview_pacing_allowed(
        bool benchmark_running, bool capture_active, bool profile_active,
        bool paused) {
    return !benchmark_running && !capture_active && !profile_active && !paused;
}

class PreviewFramePacer {
public:
    using Clock = std::chrono::steady_clock;

    void reset() {
        next_frame_.reset();
        fps_ = 0;
    }

    // Recheck after sleeping: an early wake cannot submit early. Missed
    // deadlines rebase on this actual start, never allowing catch-up bursts.
    [[nodiscard]] Clock::duration wait_before_frame(
            Clock::time_point now, int fps, bool allowed) {
        if (!allowed || fps <= 0) {
            reset();
            return Clock::duration::zero();
        }
        if (fps != fps_ || !next_frame_) {
            fps_ = fps;
            next_frame_ = now;
        }
        if (now < *next_frame_) return *next_frame_ - now;
        next_frame_ = now + std::chrono::duration_cast<Clock::duration>(
                                std::chrono::duration<double>(
                                    1.0 / static_cast<double>(fps)));
        return Clock::duration::zero();
    }

private:
    std::optional<Clock::time_point> next_frame_;
    int fps_ = 0;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_PREVIEW_FRAME_PACING_H_
