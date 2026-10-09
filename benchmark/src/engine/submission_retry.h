#ifndef HEAVENBENCH_SRC_ENGINE_SUBMISSION_RETRY_H_
#define HEAVENBENCH_SRC_ENGINE_SUBMISSION_RETRY_H_

#include <algorithm>
#include <chrono>
#include <cmath>

namespace heaven {

// Owner-thread backoff for a rejected submission. GPU timings arrive later
// than their frames; retain the last valid arrival independently of the
// current attempt's reported timing, which can legitimately be missing.
class SubmissionRetryDelay {
public:
    void observe_gpu_ms(double gpu_ms) noexcept {
        if (std::isfinite(gpu_ms) && gpu_ms > 0.0) last_gpu_ms_ = gpu_ms;
    }

    void reset() noexcept { last_gpu_ms_ = 0.0; }

    [[nodiscard]] std::chrono::duration<double, std::milli> delay() const noexcept {
        return std::chrono::duration<double, std::milli>{
            last_gpu_ms_ > 0.0 ? std::min(last_gpu_ms_ / 16.0, 8.0) : 1.0};
    }

private:
    double last_gpu_ms_ = 0.0;
};

} // namespace heaven

#endif
