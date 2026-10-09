#ifndef HEAVENBENCH_SRC_RHI_PROFILE_BRIDGE_H_
#define HEAVENBENCH_SRC_RHI_PROFILE_BRIDGE_H_

#include "benchmark/microprofiler.h"
#include "rhi/rhi.h"
#include <utils/ProfilerTrace.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string>

namespace heaven {

// Lifetime spans Engine destruction: its producer callbacks can finish while
// the engine drains workers and queued driver commands.
class ProfileBridge {
public:
    explicit ProfileBridge(profile::Recorder& recorder);
    ~ProfileBridge();
    void update(profile::Context c);
    [[nodiscard]] TraceCapabilities capabilities() const;
private:
    static void cpu(void* user, const utils::profiling::CpuEvent& e) noexcept;
    static void gpu(void* user, const utils::profiling::GpuSample& sample) noexcept;
    static void capability(void* user, const utils::profiling::GpuCapabilities& capability) noexcept;
    profile::Recorder& recorder_;
    utils::profiling::Observer observer_;
    mutable std::mutex capability_mutex_;
    utils::profiling::GpuCapabilities capability_;
    std::array<char, 192> reason_{};
};

} // namespace heaven
#endif
