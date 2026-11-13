#include "microprofiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace heaven::profile {
namespace {
std::atomic<Recorder*> g_recorder{nullptr};
std::atomic<std::uint64_t> g_generation{1};
struct ThreadState {
    const void* owner = nullptr;
    std::uint64_t generation = 0;
    std::size_t slot = std::numeric_limits<std::size_t>::max();
    Context context;
    std::uint64_t parent = 0;
};
thread_local ThreadState g_thread;
constexpr std::size_t kNameSize = 80;
constexpr std::size_t kCategorySize = 32;
constexpr std::size_t kCapabilitySize = 96;
enum class Kind { Begin, End, External, FrameBegin, FrameEnd, Counter, Flow, Gpu, PipelineUse };
struct Record {
    Kind kind = Kind::Begin;
    Context context;
    std::uint64_t timestamp = 0, end = 0, id = 0, parent = 0, gpu_begin = 0;
    std::uint64_t command_count = 0, primitive_count = 0, visible_renderables = 0;
    std::uint64_t draw_instance_invocations = 0;
    std::uint64_t native_draw_commands_full = 0;
    std::uint64_t native_draw_commands_2x1 = 0;
    std::uint64_t native_draw_commands_2x2 = 0;
    bool native_draw_commands_available = false;
    SampleStatus pipeline_statistics_status = SampleStatus::Unsupported;
    std::array<char, kCapabilitySize> pipeline_statistics_reason{};
    std::uint64_t input_assembly_vertices = 0, input_assembly_primitives = 0;
    std::uint64_t vertex_shader_invocations = 0;
    std::uint64_t clipping_invocations = 0, clipping_primitives = 0;
    std::uint64_t fragment_shader_invocations = 0;
    double value = 0;
    FrameKind frame_kind = FrameKind::Render;
    FrameOutcome outcome = FrameOutcome::Pending;
    FlowPhase phase = FlowPhase::Begin;
    SampleStatus status = SampleStatus::Unsupported;
    TimerSource source = TimerSource::Unavailable;
    std::array<char, kNameSize> name{};
    std::array<char, kCategorySize> category{};
    std::array<char, kCapabilitySize> capability{};
};
// Compile records contain references into separately bounded immutable label
// storage and generation-checked IR slots. Optional text never enlarges every
// statistic record or displaces protected pipeline/material-variant headers.
struct PipelineRecord {
    Context context;
    std::uint64_t pipeline_id = 0, program_id = 0, pass_id = 0;
    std::uint64_t original_size = 0, artifact_generation = 0;
    std::uint32_t executable_index = 0, stages = 0, subgroup_size = 0;
    std::uint32_t program_name = 0, name = 0, description = 0, reason = 0;
    std::uint32_t artifact_slot = std::numeric_limits<std::uint32_t>::max();
    PipelineEventKind kind = PipelineEventKind::Pipeline;
    SampleStatus status = SampleStatus::Unsupported;
    PipelineStatisticFormat format = PipelineStatisticFormat::Uint64;
    bool bool_value = false, truncated = false;
    std::int64_t signed_value = 0;
    std::uint64_t unsigned_value = 0;
    double float_value = 0.0;
};
struct PipelineString {
    std::uint64_t hash = 0;
    std::uint32_t offset = 0, length = 0;
};
struct PipelineArtifact {
    std::uint64_t generation = 0;
    std::array<char, 4097> text{};
};
struct Ring {
    mutable std::mutex mutex;
    Record* records = nullptr;
    std::size_t capacity = 0, count = 0, next = 0;
    std::uint64_t thread_id = 0;
    std::array<char, kNameSize> name{};
};
template<std::size_t N>
bool copy_label(std::array<char, N>& out, std::string_view text) noexcept {
    const auto count = std::min(text.size(), N - 1);
    std::copy_n(text.begin(), count, out.begin());
    out[count] = '\0';
    return count != text.size();
}
// Retain a valid UTF-8 prefix. JSON replacement could otherwise expand a
// malformed 4096-byte IR buffer beyond the reader's own bounded text limit.
// Embedded NUL/binary content is similarly disclosed as a truncated artifact.
template<std::size_t N>
bool copy_artifact(std::array<char, N>& out, std::string_view text) noexcept {
    std::size_t count = 0;
    while (count < text.size()) {
        const auto first = static_cast<unsigned char>(text[count]);
        const std::size_t width = first > 0 && first < 0x80 ? 1 :
            first >= 0xc2 && first <= 0xdf ? 2 : first >= 0xe0 && first <= 0xef ? 3 :
            first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (width == 0 || width > text.size() - count || width >= N - count) break;
        bool valid = true;
        for (std::size_t i = 1; i < width; ++i) {
            const auto next = static_cast<unsigned char>(text[count + i]);
            valid &= next >= 0x80 && next <= 0xbf;
        }
        if (width > 1) {
            const auto second = static_cast<unsigned char>(text[count + 1]);
            valid &= !(first == 0xe0 && second < 0xa0) && !(first == 0xed && second >= 0xa0) &&
                !(first == 0xf0 && second < 0x90) && !(first == 0xf4 && second >= 0x90);
        }
        if (!valid) break;
        std::copy_n(text.begin() + static_cast<std::ptrdiff_t>(count), width, out.begin() + static_cast<std::ptrdiff_t>(count));
        count += width;
    }
    out[count] = '\0';
    return count != text.size();
}
std::uint64_t relative_time(std::uint64_t time, std::uint64_t origin) noexcept {
    return time > origin ? time - origin : 0;
}
}

struct Recorder::Impl {
    explicit Impl(Limits requested)
        : limits(requested), generation(g_generation.fetch_add(1)),
          session(g_generation.fetch_add(1)), origin(Recorder::steady_now_ns()) {
        limits.max_threads = std::clamp(limits.max_threads, std::size_t{1}, std::size_t{128});
        if (limits.memory_bytes < 4096 || limits.history_ns == 0)
            throw std::invalid_argument("Microprofiler needs positive history and at least 4096 bytes");
        rings = std::make_unique<Ring[]>(limits.max_threads + 1);
    }
    Limits limits;
    const std::uint64_t generation;
    std::atomic<std::uint64_t> session;
    std::atomic<std::uint64_t> origin;
    std::atomic<std::uint64_t> stop_time{0};
    std::atomic<bool> enabled{false};
    std::atomic<bool> storage_ready{false};
    std::atomic<bool> gpu_observed{false};
    std::atomic<std::size_t> next_thread{0};
    std::atomic<std::uint64_t> next_id{1}, dropped{0}, overwritten{0}, truncated{0}, stale{0};
    std::unique_ptr<Ring[]> rings;
    std::unique_ptr<Record[]> storage;
    std::unique_ptr<PipelineRecord[]> pipeline_storage;
    mutable std::mutex pipeline_mutex;
    std::size_t pipeline_capacity = 0, pipeline_count = 0, pipeline_next = 0;
    std::size_t pipeline_header_capacity = 0, pipeline_header_count = 0;
    std::unique_ptr<PipelineString[]> pipeline_strings;
    std::unique_ptr<char[]> pipeline_string_bytes;
    std::size_t pipeline_string_capacity = 0, pipeline_string_bytes_capacity = 0;
    std::size_t pipeline_string_bytes_used = 1;
    std::unique_ptr<PipelineArtifact[]> pipeline_artifacts;
    std::size_t pipeline_artifact_capacity = 0, pipeline_artifact_next = 0;
    std::uint64_t pipeline_artifact_generation = 0;
    std::atomic<std::uint64_t> pipeline_dropped{0}, pipeline_overwritten{0}, pipeline_truncated{0};
    std::atomic<std::uint64_t> pipeline_headers_dropped{0}, pipeline_statistics_dropped{0};
    std::atomic<std::uint64_t> pipeline_statistics_overwritten{0};
    std::atomic<std::uint64_t> pipeline_artifacts_dropped{0}, pipeline_artifacts_overwritten{0};
    std::size_t storage_bytes = 0;
    mutable std::mutex control;
    std::map<std::string, std::string> metadata;

    // Called only under pipeline_mutex. References remain valid until reset;
    // full/colliding pools fail explicitly rather than returning another label.
    std::optional<std::uint32_t> intern_pipeline_string(std::string_view value) noexcept {
        if (value.empty()) return 0;
        if (value.size() > 4096 || value.find('\0') != std::string_view::npos ||
            pipeline_string_capacity == 0 || pipeline_string_bytes_capacity == 0) return {};
        std::uint64_t hash = 14695981039346656037ull;
        for (const unsigned char c : value) hash = (hash ^ c) * 1099511628211ull;
        if (hash == 0) hash = 1;
        const auto mask = pipeline_string_capacity - 1;
        auto index = static_cast<std::size_t>(hash) & mask;
        for (std::size_t probe = 0; probe < pipeline_string_capacity; ++probe) {
            auto& entry = pipeline_strings[index];
            if (entry.hash == 0) {
                if (value.size() + 1 > pipeline_string_bytes_capacity - pipeline_string_bytes_used) return {};
                const auto offset = pipeline_string_bytes_used;
                std::copy(value.begin(), value.end(), pipeline_string_bytes.get() + offset);
                pipeline_string_bytes[offset + value.size()] = '\0';
                pipeline_string_bytes_used += value.size() + 1;
                entry = {hash, static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(value.size())};
                return entry.offset;
            }
            if (entry.hash == hash && entry.length == value.size() &&
                std::string_view(pipeline_string_bytes.get() + entry.offset, entry.length) == value)
                return entry.offset;
            index = (index + 1) & mask;
        }
        return {};
    }
    void lose_pipeline_event(PipelineEventKind kind) noexcept {
        pipeline_dropped.fetch_add(1, std::memory_order_relaxed);
        if (kind == PipelineEventKind::Pipeline) pipeline_headers_dropped.fetch_add(1, std::memory_order_relaxed);
        if (kind == PipelineEventKind::Statistic) pipeline_statistics_dropped.fetch_add(1, std::memory_order_relaxed);
        if (kind == PipelineEventKind::InternalRepresentation) pipeline_artifacts_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    ThreadState& thread() noexcept {
        if (g_thread.owner != this || g_thread.generation != generation) {
            g_thread = {};
            g_thread.owner = this;
            g_thread.generation = generation;
            g_thread.slot = next_thread.fetch_add(1, std::memory_order_relaxed);
            if (g_thread.slot < limits.max_threads) {
                auto& ring = rings[g_thread.slot];
                std::unique_lock lock(ring.mutex, std::try_to_lock);
                if (lock.owns_lock()) {
                    ring.thread_id = static_cast<std::uint64_t>(
                        std::hash<std::thread::id>{}(std::this_thread::get_id()));
                }
            }
        }
        return g_thread;
    }
    Context current() noexcept {
        auto context = thread().context;
        if (context.capture_id == 0) context.capture_id = session.load(std::memory_order_relaxed);
        return context;
    }
    void labels(Record& record, std::string_view name, std::string_view category = {},
                std::string_view capability = {}) noexcept {
        const auto loss = static_cast<unsigned>(copy_label(record.name, name)) +
            static_cast<unsigned>(copy_label(record.category, category)) +
            static_cast<unsigned>(copy_label(record.capability, capability));
        truncated.fetch_add(loss, std::memory_order_relaxed);
    }
    void emit(Record record) noexcept {
        if (record.context.capture_id != 0 && record.context.capture_id != session.load(std::memory_order_relaxed)) {
            stale.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const bool gpu = record.kind == Kind::Gpu || record.kind == Kind::PipelineUse;
        if (!enabled.load(std::memory_order_acquire) &&
            !(gpu && storage_ready.load(std::memory_order_acquire))) return;
        auto& state = thread();
        if (record.context.capture_id == 0) record.context.capture_id = session.load();
        if (!gpu && state.slot >= limits.max_threads) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& ring = rings[gpu ? limits.max_threads : state.slot];
        std::unique_lock lock(ring.mutex, std::try_to_lock);
        if (record.context.capture_id != session.load(std::memory_order_relaxed)) {
            stale.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!lock.owns_lock() || (!gpu && !enabled.load(std::memory_order_relaxed)) ||
            ring.capacity == 0) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (ring.thread_id == 0) ring.thread_id = static_cast<std::uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
        ring.records[ring.next] = record;
        ring.next = (ring.next + 1) % ring.capacity;
        if (ring.count == ring.capacity) overwritten.fetch_add(1, std::memory_order_relaxed);
        else ++ring.count;
        if (gpu && !gpu_observed.load(std::memory_order_relaxed))
            gpu_observed.store(true, std::memory_order_release);
    }
};

Recorder::Recorder(Limits limits) : impl_(std::make_unique<Impl>(limits)) {}
Recorder::~Recorder() {
    // compare_exchange requires Recorder*& rather than a pointer-to-const.
    Recorder* expected = this; // NOLINT(misc-const-correctness)
    g_recorder.compare_exchange_strong(expected, nullptr);
}
std::uint64_t Recorder::steady_now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void Recorder::set_active(bool enabled) {
    std::scoped_lock lock(impl_->control);
    if (enabled && !impl_->storage) {
        // Metadata is separately bounded below. Leave room for ring descriptors
        // and control data; exported snapshots are consumer-owned copies.
        const auto overhead = sizeof(Impl) + sizeof(Ring) * (impl_->limits.max_threads + 1);
        const auto reserve = std::min(std::size_t{65536}, impl_->limits.memory_bytes / 8);
        if (overhead + reserve + sizeof(Record) * impl_->limits.max_threads > impl_->limits.memory_bytes)
            throw std::invalid_argument("Microprofiler memory budget cannot hold its thread rings");
        // Every diagnostics allocation is charged against the same recorder
        // cap. Half of its quarter-budget reserve holds compact records, one
        // quarter immutable interned labels and one quarter generation-checked
        // IR slots. Headers have a protected eighth of the metadata records.
        const auto available = impl_->limits.memory_bytes - overhead - reserve;
        const auto pipeline_budget = std::min(impl_->limits.memory_bytes / 4,
            available - sizeof(Record) * impl_->limits.max_threads);
        const auto record_budget = pipeline_budget / 2;
        const auto string_budget = pipeline_budget / 4;
        const auto artifact_budget = pipeline_budget - record_budget - string_budget;
        impl_->pipeline_capacity = record_budget / sizeof(PipelineRecord);
        impl_->pipeline_header_capacity = impl_->pipeline_capacity / 8;
        if (impl_->pipeline_capacity != 0)
            impl_->pipeline_storage = std::make_unique<PipelineRecord[]>(impl_->pipeline_capacity);
        impl_->pipeline_string_capacity = std::bit_floor(string_budget / 2 / sizeof(PipelineString));
        const auto string_index_bytes = impl_->pipeline_string_capacity * sizeof(PipelineString);
        impl_->pipeline_string_bytes_capacity = string_budget - string_index_bytes;
        if (impl_->pipeline_string_capacity != 0)
            impl_->pipeline_strings = std::make_unique<PipelineString[]>(impl_->pipeline_string_capacity);
        if (impl_->pipeline_string_bytes_capacity != 0)
            impl_->pipeline_string_bytes = std::make_unique<char[]>(impl_->pipeline_string_bytes_capacity);
        impl_->pipeline_artifact_capacity = artifact_budget / sizeof(PipelineArtifact);
        if (impl_->pipeline_artifact_capacity != 0)
            impl_->pipeline_artifacts = std::make_unique<PipelineArtifact[]>(impl_->pipeline_artifact_capacity);
        const auto pipeline_bytes = impl_->pipeline_capacity * sizeof(PipelineRecord) +
            string_index_bytes + impl_->pipeline_string_bytes_capacity +
            impl_->pipeline_artifact_capacity * sizeof(PipelineArtifact);
        const auto count = (impl_->limits.memory_bytes - overhead - reserve - pipeline_bytes) / sizeof(Record);
        impl_->storage = std::make_unique<Record[]>(count);
        impl_->storage_bytes = count * sizeof(Record) + overhead + pipeline_bytes;
        const auto gpu_count = count / 4;
        const auto cpu_count = count - gpu_count;
        std::size_t offset = 0;
        for (std::size_t i = 0; i < impl_->limits.max_threads; ++i) {
            std::size_t capacity = cpu_count;
            if (impl_->limits.max_threads == 2) capacity = cpu_count / 2;
            else if (impl_->limits.max_threads > 2)
                capacity = i < 2 ? cpu_count * 2 / 5 :
                    (cpu_count - 2 * (cpu_count * 2 / 5)) / (impl_->limits.max_threads - 2);
            auto& ring = impl_->rings[i];
            ring.records = impl_->storage.get() + offset;
            ring.capacity = capacity;
            offset += capacity;
        }
        auto& gpu_ring = impl_->rings[impl_->limits.max_threads];
        gpu_ring.records = impl_->storage.get() + offset;
        gpu_ring.capacity = gpu_count;
        impl_->storage_ready.store(true, std::memory_order_release);
    }
    if (!enabled && impl_->enabled.load(std::memory_order_relaxed))
        impl_->stop_time.store(steady_now_ns(), std::memory_order_release);
    impl_->enabled.store(enabled, std::memory_order_release);
}
bool Recorder::active() const noexcept { return impl_->enabled.load(std::memory_order_acquire); }
bool Recorder::has_gpu_observations() const noexcept {
    return impl_->gpu_observed.load(std::memory_order_acquire);
}
std::uint64_t Recorder::session_id() const noexcept { return impl_->session.load(); }
void Recorder::reset() {
    std::scoped_lock lock(impl_->control);
    if (active()) throw std::logic_error("Stop microprofiler before resetting it");
    impl_->session.store(g_generation.fetch_add(1));
    impl_->origin.store(steady_now_ns());
    impl_->stop_time.store(0);
    for (std::size_t i = 0; i <= impl_->limits.max_threads; ++i) {
        auto& ring = impl_->rings[i];
        std::scoped_lock ring_lock(ring.mutex);
        ring.count = 0;
        ring.next = 0;
    }
    impl_->dropped.store(0);
    impl_->gpu_observed.store(false, std::memory_order_release);
    impl_->overwritten.store(0);
    impl_->truncated.store(0);
    impl_->stale.store(0);
    {
        std::scoped_lock pipeline_lock(impl_->pipeline_mutex);
        impl_->pipeline_count = 0;
        impl_->pipeline_next = 0;
        impl_->pipeline_header_count = 0;
        impl_->pipeline_string_bytes_used = 1;
        if (impl_->pipeline_string_capacity != 0)
            std::fill_n(impl_->pipeline_strings.get(), impl_->pipeline_string_capacity, PipelineString{});
        if (impl_->pipeline_string_bytes_capacity != 0) impl_->pipeline_string_bytes[0] = '\0';
        impl_->pipeline_artifact_next = 0;
        impl_->pipeline_artifact_generation = 0;
        if (impl_->pipeline_artifact_capacity != 0)
            std::fill_n(impl_->pipeline_artifacts.get(), impl_->pipeline_artifact_capacity, PipelineArtifact{});
    }
    impl_->pipeline_dropped.store(0);
    impl_->pipeline_overwritten.store(0);
    impl_->pipeline_truncated.store(0);
    impl_->pipeline_headers_dropped.store(0);
    impl_->pipeline_statistics_dropped.store(0);
    impl_->pipeline_statistics_overwritten.store(0);
    impl_->pipeline_artifacts_dropped.store(0);
    impl_->pipeline_artifacts_overwritten.store(0);
    auto& state = impl_->thread();
    state.context = {};
    state.context.capture_id = session_id();
    state.parent = 0;
}
void Recorder::begin_capture(std::uint64_t history_ns) {
    if (history_ns == 0) throw std::invalid_argument("Microprofiler history must be positive");
    set_active(false);
    reset();
    {
        std::scoped_lock lock(impl_->control);
        impl_->limits.history_ns = history_ns;
    }
    set_active(true);
}
void Recorder::set_thread_name(std::string_view name) noexcept {
    auto& state = impl_->thread();
    if (state.slot >= impl_->limits.max_threads) return;
    auto& ring = impl_->rings[state.slot];
    std::unique_lock lock(ring.mutex, std::try_to_lock);
    if (!lock.owns_lock()) { impl_->dropped.fetch_add(1); return; }
    if (copy_label(ring.name, name)) impl_->truncated.fetch_add(1);
}
void Recorder::set_frame_context(Context context) noexcept {
    if (context.capture_id == 0) context.capture_id = session_id();
    impl_->thread().context = context;
}
Context Recorder::frame_context() const noexcept { return impl_->current(); }
void Recorder::set_metadata(std::string key, std::string value) {
    std::scoped_lock lock(impl_->control);
    std::size_t total = key.size() + value.size() + 256;
    for (const auto& [existing_key, existing_value] : impl_->metadata)
        if (existing_key != key) total += existing_key.size() + existing_value.size() + 256;
    const auto reserve = std::min(std::size_t{65536}, impl_->limits.memory_bytes / 8);
    if (key.size() > 128 || value.size() > 32768 || total > reserve ||
        (impl_->metadata.size() >= 64 && !impl_->metadata.contains(key)))
        throw std::invalid_argument("Microprofiler metadata exceeds bounded limits");
    impl_->metadata.insert_or_assign(std::move(key), std::move(value));
}
std::uint64_t Recorder::begin_scope(std::string_view name, std::string_view category,
                                  std::optional<Context> context) noexcept {
    if (!active()) return 0;
    auto& state = impl_->thread();
    Record record;
    record.kind = Kind::Begin;
    record.context = context.value_or(impl_->current());
    record.timestamp = steady_now_ns();
    record.id = impl_->next_id.fetch_add(1, std::memory_order_relaxed);
    record.parent = state.parent;
    impl_->labels(record, name, category);
    impl_->emit(record);
    state.parent = record.id;
    return record.id;
}
void Recorder::end_scope(std::uint64_t id) noexcept {
    if (id == 0 || !active()) return;
    Record record;
    record.kind = Kind::End;
    record.context = impl_->current();
    record.timestamp = steady_now_ns();
    record.id = id;
    impl_->emit(record);
}
void Recorder::frame_begin(Context context, FrameKind kind) noexcept {
    if (!active()) return;
    Record record;
    record.kind = Kind::FrameBegin;
    record.context = context;
    record.timestamp = steady_now_ns();
    record.frame_kind = kind;
    impl_->emit(record);
}
void Recorder::frame_end(Context context, FrameOutcome outcome, FrameKind kind) noexcept {
    if (!active()) return;
    Record record;
    record.kind = Kind::FrameEnd;
    record.context = context;
    record.timestamp = steady_now_ns();
    record.frame_kind = kind;
    record.outcome = outcome;
    impl_->emit(record);
}
void Recorder::counter(std::string_view name, double value, std::string_view category,
                       std::optional<Context> context) noexcept {
    if (!active()) return;
    if (!std::isfinite(value)) { impl_->dropped.fetch_add(1); return; }
    Record record;
    record.kind = Kind::Counter;
    record.context = context.value_or(impl_->current());
    record.timestamp = steady_now_ns();
    record.value = value;
    impl_->labels(record, name, category);
    impl_->emit(record);
}
void Recorder::flow(std::uint64_t id, FlowPhase phase, std::string_view name,
                    std::optional<Context> context) noexcept {
    if (!active()) return;
    Record record;
    record.kind = Kind::Flow;
    record.context = context.value_or(impl_->current());
    record.timestamp = steady_now_ns();
    record.id = id;
    record.phase = phase;
    impl_->labels(record, name);
    impl_->emit(record);
}
void Recorder::external_span(std::string_view name, std::string_view category,
    std::uint64_t begin_steady_ns, std::uint64_t end_steady_ns, Context context,
    std::uint64_t id, std::uint64_t parent_id) noexcept {
    if (!active()) return;
    if (end_steady_ns < begin_steady_ns) { impl_->dropped.fetch_add(1); return; }
    Record record;
    record.kind = Kind::External;
    record.context = context;
    record.timestamp = begin_steady_ns;
    record.end = end_steady_ns;
    record.id = id != 0 ? id : impl_->next_id.fetch_add(1);
    record.parent = parent_id;
    impl_->labels(record, name, category);
    impl_->emit(record);
}
void Recorder::gpu_sample(const GpuSample& sample) noexcept {
    gpu_sample(sample.context, sample.pass_id, sample.parent_pass_id, sample.name,
        sample.begin_ns, sample.end_ns, sample.status, sample.source, sample.capability,
        sample.command_count, sample.primitive_count, sample.visible_renderables, sample.draw_instance_invocations,
        sample.native_draw_commands_full, sample.native_draw_commands_2x1, sample.native_draw_commands_2x2,
        sample.native_draw_commands_available,
        {sample.pipeline_statistics_status, sample.pipeline_statistics_reason,
         sample.input_assembly_vertices, sample.input_assembly_primitives,
         sample.vertex_shader_invocations, sample.clipping_invocations,
         sample.clipping_primitives, sample.fragment_shader_invocations});
}
void Recorder::gpu_sample(Context context, std::uint64_t pass_id, std::uint64_t parent_pass_id,
    std::string_view name, std::uint64_t begin_ns, std::uint64_t end_ns,
    SampleStatus status, TimerSource source, std::string_view capability,
    std::uint64_t command_count, std::uint64_t primitive_count, std::uint64_t visible_renderables,
    std::uint64_t draw_instance_invocations, std::uint64_t native_draw_commands_full,
    std::uint64_t native_draw_commands_2x1, std::uint64_t native_draw_commands_2x2,
    bool native_draw_commands_available, PipelineStatisticsView pipeline_statistics) noexcept {
    if (!impl_->storage_ready.load(std::memory_order_acquire)) return;
    if (!active() && context.capture_id == 0) { impl_->dropped.fetch_add(1); return; }
    Record record;
    record.kind = Kind::Gpu;
    record.context = context;
    record.timestamp = steady_now_ns();
    record.id = pass_id;
    record.parent = parent_pass_id;
    record.value = 0;
    // GPU times deliberately use separate fields and retain their own epoch.
    record.end = end_ns;
    record.status = status;
    if (status == SampleStatus::Available && end_ns < begin_ns) record.status = SampleStatus::Error;
    record.source = source;
    impl_->labels(record, name, {}, capability);
    // Separate payload for GPU begin; counter/frame fields are unused here.
    record.gpu_begin = begin_ns;
    record.command_count = command_count;
    record.primitive_count = primitive_count;
    record.visible_renderables = visible_renderables;
    record.draw_instance_invocations = draw_instance_invocations;
    record.native_draw_commands_full = native_draw_commands_full;
    record.native_draw_commands_2x1 = native_draw_commands_2x1;
    record.native_draw_commands_2x2 = native_draw_commands_2x2;
    record.native_draw_commands_available = native_draw_commands_available;
    record.pipeline_statistics_status = pipeline_statistics.status;
    if (copy_label(record.pipeline_statistics_reason, pipeline_statistics.reason)) impl_->truncated.fetch_add(1);
    record.input_assembly_vertices = pipeline_statistics.input_assembly_vertices;
    record.input_assembly_primitives = pipeline_statistics.input_assembly_primitives;
    record.vertex_shader_invocations = pipeline_statistics.vertex_shader_invocations;
    record.clipping_invocations = pipeline_statistics.clipping_invocations;
    record.clipping_primitives = pipeline_statistics.clipping_primitives;
    record.fragment_shader_invocations = pipeline_statistics.fragment_shader_invocations;
    impl_->emit(record);
}


void Recorder::pipeline_event(const PipelineEventView& event) noexcept {
    if (!impl_->storage_ready.load(std::memory_order_acquire)) return;
    if (event.context.capture_id != session_id()) {
        impl_->stale.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (event.kind == PipelineEventKind::Use) {
        Record record;
        record.kind = Kind::PipelineUse;
        record.context = event.context;
        record.timestamp = steady_now_ns();
        record.id = event.pipeline_id;
        record.parent = event.program_id;
        record.gpu_begin = event.pass_id;
        record.status = event.status;
        impl_->labels(record, event.program_name, {}, event.reason);
        impl_->emit(record);
        return;
    }
    if (event.format == PipelineStatisticFormat::Float64 && !std::isfinite(event.float_value)) {
        impl_->lose_pipeline_event(event.kind);
        return;
    }
    std::unique_lock lock(impl_->pipeline_mutex, std::try_to_lock);
    if (!lock.owns_lock() || impl_->pipeline_capacity == 0) {
        impl_->lose_pipeline_event(event.kind);
        return;
    }
    if (event.context.capture_id != session_id()) {
        impl_->stale.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto program = impl_->intern_pipeline_string(event.program_name);
    const auto name = impl_->intern_pipeline_string(event.name);
    const auto description = impl_->intern_pipeline_string(event.description);
    const auto reason = impl_->intern_pipeline_string(event.reason);
    if (!program || !name || !description || !reason) {
        impl_->lose_pipeline_event(event.kind);
        return;
    }
    PipelineRecord record;
    record.context = event.context;
    record.kind = event.kind;
    record.pipeline_id = event.pipeline_id;
    record.program_id = event.program_id;
    record.pass_id = event.pass_id;
    record.executable_index = event.executable_index;
    record.stages = event.stages;
    record.subgroup_size = event.subgroup_size;
    record.program_name = *program;
    record.name = *name;
    record.description = *description;
    record.reason = *reason;
    record.truncated = event.truncated;
    record.status = event.status;
    record.format = event.format;
    record.bool_value = event.bool_value;
    record.signed_value = event.signed_value;
    record.unsigned_value = event.unsigned_value;
    record.float_value = event.float_value;
    record.original_size = event.original_size;
    if (!event.text.empty()) {
        if (impl_->pipeline_artifact_capacity == 0 ||
            impl_->pipeline_artifact_generation == std::numeric_limits<std::uint64_t>::max()) {
            impl_->pipeline_artifacts_dropped.fetch_add(1, std::memory_order_relaxed);
            record.status = SampleStatus::Dropped;
            record.truncated = true;
        } else {
            const auto slot = impl_->pipeline_artifact_next;
            auto& artifact = impl_->pipeline_artifacts[slot];
            if (artifact.generation != 0)
                impl_->pipeline_artifacts_overwritten.fetch_add(1, std::memory_order_relaxed);
            artifact.generation = ++impl_->pipeline_artifact_generation;
            record.truncated = copy_artifact(artifact.text, event.text) || event.truncated;
            record.artifact_slot = static_cast<std::uint32_t>(slot);
            record.artifact_generation = artifact.generation;
            impl_->pipeline_artifact_next = (slot + 1) % impl_->pipeline_artifact_capacity;
        }
    }
    if (record.truncated && event.kind == PipelineEventKind::InternalRepresentation)
        impl_->pipeline_truncated.fetch_add(1, std::memory_order_relaxed);
    if (event.kind == PipelineEventKind::Pipeline) {
        // Header slots never yield to optional statistics or IR. Replayed
        // headers update their existing identity instead of consuming slots.
        for (std::size_t i = 0; i < impl_->pipeline_header_count; ++i) {
            const auto& previous = impl_->pipeline_storage[i];
            if (previous.pipeline_id == record.pipeline_id && previous.executable_index == record.executable_index) {
                impl_->pipeline_storage[i] = record;
                return;
            }
        }
        if (impl_->pipeline_header_count == impl_->pipeline_header_capacity) {
            impl_->lose_pipeline_event(event.kind);
            return;
        }
        impl_->pipeline_storage[impl_->pipeline_header_count++] = record;
        return;
    }
    const auto capacity = impl_->pipeline_capacity - impl_->pipeline_header_capacity;
    auto& destination = impl_->pipeline_storage[impl_->pipeline_header_capacity + impl_->pipeline_next];
    if (impl_->pipeline_count == capacity) {
        impl_->pipeline_overwritten.fetch_add(1, std::memory_order_relaxed);
        if (destination.kind == PipelineEventKind::Statistic)
            impl_->pipeline_statistics_overwritten.fetch_add(1, std::memory_order_relaxed);
        if (destination.kind == PipelineEventKind::InternalRepresentation && destination.artifact_generation != 0 &&
            destination.artifact_slot < impl_->pipeline_artifact_capacity) {
            auto& artifact = impl_->pipeline_artifacts[destination.artifact_slot];
            if (artifact.generation == destination.artifact_generation) {
                artifact.generation = 0;
                impl_->pipeline_artifacts_overwritten.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } else ++impl_->pipeline_count;
    destination = record;
    impl_->pipeline_next = (impl_->pipeline_next + 1) % capacity;

}

Snapshot Recorder::snapshot() const {
    std::scoped_lock control_lock(impl_->control);
    Snapshot out;
    out.session_id = session_id();
    out.origin_ns = impl_->origin.load();
    out.active = active();
    const auto stopped = impl_->stop_time.load();
    const auto now = out.active || stopped == 0 ? steady_now_ns() : stopped;
    out.end_ns = relative_time(now, out.origin_ns);
    out.begin_ns = out.end_ns > impl_->limits.history_ns ? out.end_ns - impl_->limits.history_ns : 0;
    out.dropped_events = impl_->dropped.load();
    out.overwritten_events = impl_->overwritten.load();
    out.truncated_labels = impl_->truncated.load();
    out.stale_epoch_events = impl_->stale.load();
    out.dropped_pipeline_events = impl_->pipeline_dropped.load();
    out.overwritten_pipeline_events = impl_->pipeline_overwritten.load();
    out.truncated_pipeline_artifacts = impl_->pipeline_truncated.load();
    out.pipeline_loss_breakdown_available = true;
    out.dropped_pipeline_headers = impl_->pipeline_headers_dropped.load();
    out.dropped_pipeline_statistics = impl_->pipeline_statistics_dropped.load();
    out.overwritten_pipeline_statistics = impl_->pipeline_statistics_overwritten.load();
    out.dropped_pipeline_artifacts = impl_->pipeline_artifacts_dropped.load();
    out.overwritten_pipeline_artifacts = impl_->pipeline_artifacts_overwritten.load();
    out.memory_bytes = impl_->storage_bytes;
    out.metadata = impl_->metadata;
    {
        std::scoped_lock pipeline_lock(impl_->pipeline_mutex);
        using PipelineKey = std::tuple<std::uint64_t, std::uint32_t, PipelineEventKind, std::string>;
        std::map<PipelineKey, std::size_t> latest;
        const auto label = [&](std::uint32_t offset) -> std::string {
            return offset == 0 ? std::string{} : std::string(impl_->pipeline_string_bytes.get() + offset);
        };
        const auto append = [&](const PipelineRecord& r) {
            if (r.context.capture_id != out.session_id) return;
            PipelineEvent event;
            event.context = r.context;
            event.kind = r.kind;
            event.pipeline_id = r.pipeline_id;
            event.program_id = r.program_id;
            event.pass_id = r.pass_id;
            event.executable_index = r.executable_index;
            event.stages = r.stages;
            event.subgroup_size = r.subgroup_size;
            event.program_name = label(r.program_name);
            event.name = label(r.name);
            event.description = label(r.description);
            event.reason = label(r.reason);
            event.status = r.status;
            event.format = r.format;
            event.bool_value = r.bool_value;
            event.signed_value = r.signed_value;
            event.unsigned_value = r.unsigned_value;
            event.float_value = r.float_value;
            event.original_size = r.original_size;
            event.truncated = r.truncated;
            if (r.artifact_generation != 0 && r.artifact_slot < impl_->pipeline_artifact_capacity) {
                const auto& artifact = impl_->pipeline_artifacts[r.artifact_slot];
                if (artifact.generation == r.artifact_generation) event.text = artifact.text.data();
                else {
                    event.status = SampleStatus::Dropped;
                    event.reason = "bounded IR slot was overwritten; compiler headers and statistics remain independent";
                    event.truncated = true;
                }
            } else if (r.kind == PipelineEventKind::InternalRepresentation && r.status == SampleStatus::Dropped) {
                event.reason = "bounded IR storage was unavailable; compiler headers and statistics remain independent";
            }
            // Driver loss records can contain deltas. Keep each retained delta;
            // replacing repeated Loss records would hide earlier metadata loss.
            if (r.kind == PipelineEventKind::Loss) out.pipeline_events.push_back(std::move(event));
            else {
                const auto [entry, inserted] = latest.try_emplace(
                    PipelineKey{r.pipeline_id, r.executable_index, r.kind, event.name}, out.pipeline_events.size());
                if (inserted) out.pipeline_events.push_back(std::move(event));
                else out.pipeline_events[entry->second] = std::move(event);
            }
        };
        for (std::size_t i = 0; i < impl_->pipeline_header_count; ++i) append(impl_->pipeline_storage[i]);
        const auto capacity = impl_->pipeline_capacity - impl_->pipeline_header_capacity;
        for (std::size_t i = 0; i < impl_->pipeline_count; ++i) {
            const auto index = (impl_->pipeline_next + capacity - impl_->pipeline_count + i) % capacity;
            append(impl_->pipeline_storage[impl_->pipeline_header_capacity + index]);
        }
    }
    struct Tagged { Record record; std::uint64_t thread; };
    std::vector<Tagged> records;
    records.reserve(impl_->storage_bytes / sizeof(Record));
    out.threads.reserve(impl_->limits.max_threads);
    for (std::size_t i = 0; i <= impl_->limits.max_threads; ++i) {
        auto& ring = impl_->rings[i];
        std::scoped_lock ring_lock(ring.mutex);
        if (i < std::min(impl_->next_thread.load(), impl_->limits.max_threads))
            out.threads.push_back({ring.thread_id, ring.name[0] != '\0' ? std::string(ring.name.data()) :
                "Thread " + std::to_string(i)});
        for (std::size_t j = 0; j < ring.count; ++j) {
            const auto index = (ring.next + ring.capacity - ring.count + j) % ring.capacity;
            const auto& record = ring.records[index];
            if (record.context.capture_id == out.session_id) records.push_back({record, ring.thread_id});
        }
    }
    std::stable_sort(records.begin(), records.end(), [](const Tagged& a, const Tagged& b) {
        return a.record.timestamp < b.record.timestamp;
    });
    std::unordered_map<std::uint64_t, std::size_t> spans;
    using FrameKey = std::tuple<std::uint64_t, std::uint64_t, FrameKind>;
    std::map<FrameKey, std::size_t> frames;
    using GpuKey = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    std::map<GpuKey, std::size_t> gpu_samples;
    for (const auto& tagged : records) {
        const auto& r = tagged.record;
        const auto time = relative_time(r.timestamp, out.origin_ns);
        switch (r.kind) {
        case Kind::Begin:
            spans[r.id] = out.spans.size();
            out.spans.push_back({r.id, r.parent, tagged.thread, time, out.end_ns,
                r.name.data(), r.category.data(), r.context, true});
            break;
        case Kind::End:
            if (const auto found = spans.find(r.id); found != spans.end()) {
                auto& span = out.spans[found->second];
                span.end_ns = std::max(span.begin_ns, time);
                span.incomplete = false;
            }
            break;
        case Kind::External:
            out.spans.push_back({r.id, r.parent, tagged.thread, time,
                relative_time(r.end, out.origin_ns), r.name.data(), r.category.data(), r.context, false});
            break;
        case Kind::FrameBegin:
            frames[{r.context.attempt_id, r.context.renderer_id, r.frame_kind}] = out.frames.size();
            out.frames.push_back({r.context, time, out.end_ns, r.frame_kind, FrameOutcome::Pending, true});
            break;
        case Kind::FrameEnd:
            if (const auto found = frames.find({r.context.attempt_id, r.context.renderer_id, r.frame_kind});
                found != frames.end()) {
                auto& frame = out.frames[found->second];
                frame.end_ns = std::max(frame.begin_ns, time);
                frame.context = r.context;
                frame.outcome = r.outcome;
                frame.incomplete = false;
            }
            break;
        case Kind::Counter:
            if (time >= out.begin_ns) out.counters.push_back({tagged.thread, time,
                r.name.data(), r.category.data(), r.value, r.context});
            break;
        case Kind::Flow:
            if (time >= out.begin_ns) out.flows.push_back({r.id, tagged.thread, time,
                r.name.data(), r.phase, r.context});
            break;
        case Kind::Gpu:
            if (time >= out.begin_ns) {
                const GpuKey key{r.context.renderer_id, r.context.filament_frame_id, r.id};
                GpuSample sample{r.context, r.id, r.parent, r.gpu_begin, r.end,
                    r.name.data(), r.status, r.source, r.capability.data(),
                    r.command_count, r.primitive_count, r.visible_renderables, r.draw_instance_invocations,
                    r.native_draw_commands_full, r.native_draw_commands_2x1, r.native_draw_commands_2x2,
                    r.native_draw_commands_available, r.pipeline_statistics_status,
                    r.pipeline_statistics_reason.data(), r.input_assembly_vertices,
                    r.input_assembly_primitives, r.vertex_shader_invocations,
                    r.clipping_invocations, r.clipping_primitives, r.fragment_shader_invocations};
                const auto [found, inserted] = gpu_samples.try_emplace(key, out.gpu_samples.size());
                if (inserted) out.gpu_samples.push_back(std::move(sample));
                else out.gpu_samples[found->second] = std::move(sample);
            }
            break;
        case Kind::PipelineUse:
            if (time >= out.begin_ns) {
                PipelineEvent event;
                event.context = r.context;
                event.kind = PipelineEventKind::Use;
                event.pipeline_id = r.id;
                event.program_id = r.parent;
                event.pass_id = r.gpu_begin;
                event.program_name = r.name.data();
                event.status = r.status;
                event.reason = r.capability.data();
                out.pipeline_events.push_back(std::move(event));
            }
            break;
        }
    }
    std::erase_if(out.spans, [&](const Span& span) { return span.end_ns < out.begin_ns; });
    for (auto& span : out.spans) {
        if (span.begin_ns < out.begin_ns) { span.begin_ns = out.begin_ns; span.incomplete = true; }
    }
    std::erase_if(out.frames, [&](const Frame& frame) { return frame.end_ns < out.begin_ns; });
    for (auto& frame : out.frames) {
        if (frame.begin_ns < out.begin_ns) { frame.begin_ns = out.begin_ns; frame.incomplete = true; }
    }
    // The application supplies a candidate submission before beginFrame can
    // reject it. Only retained acceptance evidence may confirm that candidate;
    // attempt IDs still identify rejected work and work whose outcome aged out.
    struct Acceptance {
        bool rejected = false, frame = false, ambiguous = false;
        std::uint64_t submission = 0;
    };
    using AttemptKey = std::pair<std::uint64_t, std::uint64_t>;
    std::map<AttemptKey, Acceptance> acceptance;
    for (const auto& frame : out.frames) {
        auto& proof = acceptance[{frame.context.capture_id, frame.context.attempt_id}];
        if (frame.outcome != FrameOutcome::Accepted) proof.rejected = true;
        else {
            if (proof.frame && proof.submission != frame.context.submission_id) proof.ambiguous = true;
            proof.frame = true;
            proof.submission = frame.context.submission_id;
        }
    }
    for (const auto& sample : out.gpu_samples) {
        const auto& context = sample.context;
        if (sample.status == SampleStatus::Dropped || context.renderer_id == 0 ||
            context.filament_frame_id == 0 || context.submission_id == 0) continue;
        auto& proof = acceptance[{context.capture_id, context.attempt_id}];
        if (proof.frame) continue;
        if (proof.submission != 0 && proof.submission != context.submission_id) proof.ambiguous = true;
        proof.submission = context.submission_id;
    }
    const auto normalize = [&](Context& context) {
        if (context.submission_id == 0) return;
        const auto proof = acceptance.find({context.capture_id, context.attempt_id});
        if (proof == acceptance.end() || proof->second.rejected || proof->second.ambiguous ||
            proof->second.submission != context.submission_id) context.submission_id = 0;
    };
    for (auto& span : out.spans) normalize(span.context);
    for (auto& counter : out.counters) normalize(counter.context);
    for (auto& flow : out.flows) normalize(flow.context);
    return out;
}
void set_recorder(Recorder* recorder) noexcept { g_recorder.store(recorder, std::memory_order_release); }
Recorder* recorder() noexcept { return g_recorder.load(std::memory_order_acquire); }
Scope::Scope(const char* name, const char* category) noexcept {
    if (auto* installed = profile::recorder(); installed && installed->active()) {
        recorder_ = installed;
        previous_parent_ = recorder_->impl_->thread().parent;
        id_ = recorder_->begin_scope(name != nullptr ? name : "", category != nullptr ? category : "CPU", {});
    }
}
Scope::Scope(Recorder& recorder, std::string_view name, std::string_view category,
             std::optional<Context> context) noexcept : recorder_(&recorder) {
    previous_parent_ = recorder_->impl_->thread().parent;
    id_ = recorder_->begin_scope(name, category, context);
}
Scope::~Scope() {
    if (recorder_ != nullptr) {
        recorder_->end_scope(id_);
        recorder_->impl_->thread().parent = previous_parent_;
    }
}
ContextScope::ContextScope(Context context) noexcept : recorder_(profile::recorder()) {
    if (recorder_ != nullptr) {
        previous_ = recorder_->frame_context();
        recorder_->set_frame_context(context);
    }
}
ContextScope::ContextScope(Recorder& recorder, Context context) noexcept : recorder_(&recorder) {
    previous_ = recorder_->frame_context();
    recorder_->set_frame_context(context);
}
ContextScope::~ContextScope() {
    if (recorder_ != nullptr) recorder_->set_frame_context(previous_);
}
} // namespace heaven::profile
