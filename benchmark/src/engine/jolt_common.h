/*
 * HeavenBench — shared Jolt Physics initialization
 *
 * Jolt's `RegisterTypes()` / `Factory` / allocator setup is
 * process-global and must run exactly once. Two independent
 * subsystems use Jolt today — the bridge rope simulation
 * (rope_sim.cpp) and camera collision (camera_collision.cpp) —
 * so the one-time registration lives here behind a function-local
 * static guard (thread-safe, single-init in C++11+).
 *
 * Both subsystems call ensure_jolt_registered() before creating
 * their own PhysicsSystem; calling it more than once is a no-op.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_JOLT_COMMON_H_
#define HEAVENBENCH_SRC_ENGINE_JOLT_COMMON_H_

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>

#include <cstdarg>
#include <cstdio>

namespace heaven {

inline void jolt_trace_impl(const char* fmt, ...) {
    va_list list;
    va_start(list, fmt);
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, list);
    va_end(list);
    std::printf("    [Jolt] %s\n", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
inline bool jolt_assert_impl(const char* expr, const char* msg,
                             const char* file, JPH::uint line) {
    std::fprintf(stderr, "Jolt Assert: %s:%u: %s (%s)\n",
                 file, line, expr, msg ? msg : "");
    return true;
}
#endif

//Perform Jolt's one-time global registration. Safe to call from
//any subsystem any number of times; only the first call does work.
inline void ensure_jolt_registered() {
    [[maybe_unused]] static const bool done = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = jolt_trace_impl;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = jolt_assert_impl;
#endif
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_JOLT_COMMON_H_
