/*
 * HeavenBench — startup timing helpers
 */
#ifndef HEAVENBENCH_SRC_ENGINE_STARTUP_TIMER_H_
#define HEAVENBENCH_SRC_ENGINE_STARTUP_TIMER_H_

#include "benchmark/microprofiler.h"
#include <chrono>
#include <cstdio>
#include <utility>

namespace heaven {

class ScopedStartupTimer {
public:
    explicit ScopedStartupTimer(const char* label)
        : trace_(label, "Loading"), label_(label), start_(Clock::now()) {}

    ScopedStartupTimer(const ScopedStartupTimer&) = delete;
    ScopedStartupTimer& operator=(const ScopedStartupTimer&) = delete;

    ~ScopedStartupTimer() {
        std::printf("  [startup] %-34s %8.2f ms\n", label_, elapsed_ms(start_));
    }

private:
    using Clock = std::chrono::steady_clock;

    static double elapsed_ms(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    profile::Scope   trace_;
    const char*       label_;
    Clock::time_point start_;
};

inline std::chrono::steady_clock::time_point startup_timer_now() {
    return std::chrono::steady_clock::now();
}

inline double startup_elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_STARTUP_TIMER_H_
