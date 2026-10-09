#ifndef HEAVENBENCH_SRC_BENCHMARK_MICROPROFILER_H_
#define HEAVENBENCH_SRC_BENCHMARK_MICROPROFILER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace heaven::profile {

// IDs are correlation keys, not timestamps. Zero means not supplied.
struct Context {
    std::uint64_t attempt_id = 0;
    std::uint64_t submission_id = 0;
    std::uint64_t filament_frame_id = 0;
    std::uint64_t renderer_id = 0;
    std::uint64_t view_id = 0;
    std::uint64_t capture_id = 0;
    bool operator==(const Context&) const = default;
};

enum class FrameKind { Render, Loading };
enum class FrameOutcome { Pending, Accepted, Rejected, Cancelled };
enum class FlowPhase { Begin, Step, End };
enum class SampleStatus { Available, Pending, Unsupported, Disjoint, Error, Dropped };
enum class TimerSource { HardwareTimestamp, FenceEstimate, Unavailable };

struct ThreadInfo { std::uint64_t id = 0; std::string name; };
struct Span {
    std::uint64_t id = 0, parent_id = 0, thread_id = 0;
    std::uint64_t begin_ns = 0, end_ns = 0;
    std::string name, category;
    Context context;
    bool incomplete = false;
};
struct Frame {
    Context context;
    std::uint64_t begin_ns = 0, end_ns = 0;
    FrameKind kind = FrameKind::Render;
    FrameOutcome outcome = FrameOutcome::Pending;
    bool incomplete = false;
};
struct Counter {
    std::uint64_t thread_id = 0, timestamp_ns = 0;
    std::string name, category;
    double value = 0.0;
    Context context;
};
struct Flow {
    std::uint64_t id = 0, thread_id = 0, timestamp_ns = 0;
    std::string name;
    FlowPhase phase = FlowPhase::Begin;
    Context context;
};
struct GpuSample {
    Context context;
    std::uint64_t pass_id = 0, parent_pass_id = 0;
    // Uncalibrated GPU times are relative to their own GPU frame. They must
    // never be drawn as if they shared the CPU clock's absolute timeline.
    std::uint64_t begin_ns = 0, end_ns = 0;
    std::string name;
    SampleStatus status = SampleStatus::Unsupported;
    TimerSource source = TimerSource::Unavailable;
    std::string capability;
    std::uint64_t command_count = 0, primitive_count = 0, visible_renderables = 0;
    // Sum of instances over executed draws, not unique visible placements.
    std::uint64_t draw_instance_invocations = 0;
};

struct Limits {
    std::uint64_t history_ns = 10'000'000'000ull;
#ifdef __ANDROID__
    std::size_t memory_bytes = std::size_t{16} * 1024 * 1024;
#else
    std::size_t memory_bytes = std::size_t{64} * 1024 * 1024;
#endif
    std::size_t max_threads = 64;
};

struct Snapshot {
    std::uint32_t schema = 1;
    std::uint64_t session_id = 0;
    // The steady-clock origin is serialized as a decimal string for JS safety.
    std::uint64_t origin_ns = 0, begin_ns = 0, end_ns = 0;
    bool active = false;
    std::uint64_t dropped_events = 0, overwritten_events = 0, truncated_labels = 0;
    // Expected imports from a previous capture are not lost current events.
    std::uint64_t stale_epoch_events = 0;
    std::size_t memory_bytes = 0;
    std::map<std::string, std::string> metadata;
    std::vector<ThreadInfo> threads;
    std::vector<Span> spans;
    std::vector<Frame> frames;
    std::vector<Counter> counters;
    std::vector<Flow> flows;
    std::vector<GpuSample> gpu_samples;
};

class Scope;
class ContextScope;
class Recorder {
public:
    explicit Recorder(Limits limits = {});
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // First activation preallocates bounded storage; recording callbacks do
    // not allocate. Stopping preserves history. Capture/export is diagnostic.
    void set_active(bool enabled);
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::uint64_t session_id() const noexcept;
    // Control-thread operation. Clears retained events while disabled, then
    // changes the correlation epoch. Old callback contexts are rejected.
    void reset();
    void begin_capture(std::uint64_t history_ns = 10'000'000'000ull);
    void set_thread_name(std::string_view name) noexcept;
    void set_frame_context(Context context) noexcept;
    [[nodiscard]] Context frame_context() const noexcept;
    void set_metadata(std::string key, std::string value);

    void frame_begin(Context context, FrameKind kind = FrameKind::Render) noexcept;
    void frame_end(Context context, FrameOutcome outcome,
                   FrameKind kind = FrameKind::Render) noexcept;
    void counter(std::string_view name, double value,
                 std::string_view category = "Counter",
                 std::optional<Context> context = {}) noexcept;
    void flow(std::uint64_t id, FlowPhase phase, std::string_view name,
              std::optional<Context> context = {}) noexcept;
    // External CPU spans use absolute steady-clock nanoseconds. Their owner
    // must provide inherited context and a stable thread name; no guessed
    // association with a global latest frame is made.
    void external_span(std::string_view name, std::string_view category,
                       std::uint64_t begin_steady_ns, std::uint64_t end_steady_ns,
                       Context context = {}, std::uint64_t id = 0,
                       std::uint64_t parent_id = 0) noexcept;
    // Import copies bounded labels immediately; samples may arrive late.
    void gpu_sample(const GpuSample& sample) noexcept;
    void gpu_sample(Context context, std::uint64_t pass_id, std::uint64_t parent_pass_id,
                    std::string_view name, std::uint64_t begin_ns, std::uint64_t end_ns,
                    SampleStatus status, TimerSource source,
                    std::string_view capability = {}, std::uint64_t command_count = 0,
                    std::uint64_t primitive_count = 0, std::uint64_t visible_renderables = 0,
                    std::uint64_t draw_instance_invocations = 0) noexcept;
    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] static std::uint64_t steady_now_ns() noexcept;

private:
    friend class Scope;
    friend class ContextScope;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint64_t begin_scope(std::string_view name, std::string_view category,
                              std::optional<Context> context) noexcept;
    void end_scope(std::uint64_t id) noexcept;
};

// The application installs one borrowed recorder and keeps it alive until all
// instrumented workers/backend callbacks are drained. Clear it before destroy.
void set_recorder(Recorder* recorder) noexcept;
[[nodiscard]] Recorder* recorder() noexcept;

class Scope {
public:
    explicit Scope(const char* name, const char* category = "CPU") noexcept;
    Scope(Recorder& recorder, std::string_view name, std::string_view category = "CPU",
          std::optional<Context> context = {}) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Recorder* recorder_ = nullptr;
    std::uint64_t id_ = 0, previous_parent_ = 0;
};

class ContextScope {
public:
    explicit ContextScope(Context context) noexcept;
    ContextScope(Recorder& recorder, Context context) noexcept;
    ~ContextScope();
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
private:
    Recorder* recorder_ = nullptr;
    Context previous_;
};

} // namespace heaven::profile
#endif
