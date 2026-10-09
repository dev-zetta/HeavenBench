#include "rhi/profile_bridge.h"
#include <algorithm>
#include <cstring>

namespace heaven {
namespace {
profile::Context convert(utils::profiling::Context c) noexcept {
    return {c.attempt_id, c.submission_id, c.filament_frame_id,
        c.renderer_id, c.view_id, c.capture_id};
}
profile::SampleStatus convert(utils::profiling::GpuStatus status) noexcept {
    using S = utils::profiling::GpuStatus;
    switch (status) {
        case S::AVAILABLE: return profile::SampleStatus::Available;
        case S::UNSUPPORTED: return profile::SampleStatus::Unsupported;
        case S::DISJOINT: return profile::SampleStatus::Disjoint;
        case S::ERROR: return profile::SampleStatus::Error;
        case S::DROPPED: return profile::SampleStatus::Dropped;
        case S::PENDING: return profile::SampleStatus::Pending;
    }
    return profile::SampleStatus::Error;
}
const char* sample_reason(utils::profiling::GpuStatus status) noexcept {
    using S = utils::profiling::GpuStatus;
    switch (status) {
        case S::AVAILABLE: return "Uncalibrated GPU frame-relative timestamps";
        case S::PENDING: return "GPU timestamp results have not completed";
        case S::UNSUPPORTED: return "Hardware timestamps unavailable; see gpu_timing_reason metadata";
        case S::DISJOINT: return "GPU clock became disjoint; interval is unavailable";
        case S::ERROR: return "GPU query, clock ordering, context or teardown failure";
        case S::DROPPED: return "GPU storage exhausted: 256 passes per frame or 16 unresolved frames";
    }
    return "Unknown GPU timing status";
}
}

ProfileBridge::ProfileBridge(profile::Recorder& recorder)
    : recorder_(recorder), observer_{this, cpu, gpu, capability} {
    utils::profiling::setObserver(&observer_);
    utils::profiling::setEnabled(recorder_.active());
}
ProfileBridge::~ProfileBridge() {
    utils::profiling::setEnabled(false);
    utils::profiling::setObserver(nullptr);
}
void ProfileBridge::update(profile::Context c) {
    utils::profiling::setContext({c.attempt_id, c.submission_id,
        c.filament_frame_id, c.renderer_id, c.view_id, c.capture_id});
    utils::profiling::setEnabled(recorder_.active());
}
void ProfileBridge::cpu(void* user, const utils::profiling::CpuEvent& e) noexcept {
    auto& recorder = static_cast<ProfileBridge*>(user)->recorder_;
    const auto context = convert(e.context);
    const auto name = e.name ? e.name : "Filament";
    const auto category = e.category ? e.category : "Filament";
    using K = utils::profiling::CpuEventKind;
    if (e.kind == K::SCOPE && e.begin_ns >= 0 && e.end_ns >= e.begin_ns) {
        recorder.external_span(name, category, static_cast<uint64_t>(e.begin_ns),
            static_cast<uint64_t>(e.end_ns), context, e.scope_id ? e.scope_id | (uint64_t{1} << 63) : 0,
            e.parent_id ? e.parent_id | (uint64_t{1} << 63) : 0);
    } else if (e.kind == K::ASYNC_BEGIN || e.kind == K::ASYNC_END) {
        recorder.flow(e.cookie, e.kind == K::ASYNC_BEGIN ? profile::FlowPhase::Begin
            : profile::FlowPhase::End, name, context);
    } else if (e.kind == K::COUNTER || e.kind == K::DROPPED) {
        recorder.counter(name, static_cast<double>(e.value), category, context);
    } else if (e.kind == K::INSTANT && e.begin_ns >= 0) {
        recorder.external_span(name, category, static_cast<uint64_t>(e.begin_ns),
            static_cast<uint64_t>(e.begin_ns), context,
            e.scope_id ? e.scope_id | (uint64_t{1} << 63) : 0,
            e.parent_id ? e.parent_id | (uint64_t{1} << 63) : 0);
    }
}
void ProfileBridge::gpu(void* user, const utils::profiling::GpuSample& sample) noexcept {
    auto& recorder = static_cast<ProfileBridge*>(user)->recorder_;
    const auto context = convert(sample.context);
    const bool valid = sample.status == utils::profiling::GpuStatus::AVAILABLE &&
        sample.begin_ns >= 0 && sample.end_ns >= sample.begin_ns;
    recorder.gpu_sample(context, sample.pass_id, sample.parent_pass_id,
        sample.name ? sample.name : "GPU pass",
        valid ? static_cast<uint64_t>(sample.begin_ns) : 0,
        valid ? static_cast<uint64_t>(sample.end_ns) : 0,
        valid ? profile::SampleStatus::Available
            : sample.status == utils::profiling::GpuStatus::AVAILABLE
                ? profile::SampleStatus::Error : convert(sample.status),
        valid ? profile::TimerSource::HardwareTimestamp : profile::TimerSource::Unavailable,
        sample_reason(sample.status == utils::profiling::GpuStatus::AVAILABLE && !valid
            ? utils::profiling::GpuStatus::ERROR : sample.status), sample.command_count,
        sample.primitive_count, sample.visible_renderables, sample.draw_instance_invocations);
}
void ProfileBridge::capability(void* user, const utils::profiling::GpuCapabilities& capability) noexcept {
    auto& self = *static_cast<ProfileBridge*>(user);
    std::unique_lock lock(self.capability_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    self.capability_ = capability;
    const char* reason = capability.reason ? capability.reason : "";
    const auto size = std::min(std::strlen(reason), self.reason_.size() - 1);
    std::memcpy(self.reason_.data(), reason, size);
    self.reason_[size] = '\0';
    self.capability_.reason = self.reason_.data();
}
TraceCapabilities ProfileBridge::capabilities() const {
    std::scoped_lock lock(capability_mutex_);
    TraceCapabilities result;
    result.hardware_timestamps = capability_.hardware_timestamps;
    result.cpu_clock_calibrated = capability_.cpu_clock_calibrated;
    result.backend = capability_.backend == utils::profiling::GpuBackend::VULKAN ? "Vulkan"
        : capability_.backend == utils::profiling::GpuBackend::OPENGL ? "OpenGL" : "Unknown";
    if (capability_.reason) result.reason = reason_.data();
    result.timestamp_valid_bits = capability_.timestamp_valid_bits;
    result.timestamp_period_ns = capability_.timestamp_period_ns;
    return result;
}
} // namespace heaven
