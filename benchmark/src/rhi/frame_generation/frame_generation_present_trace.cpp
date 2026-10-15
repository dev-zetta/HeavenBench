#include "frame_generation_present_trace.h"

#include "json.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace heaven {
namespace {
constexpr size_t maximum_samples = 32768;
constexpr size_t maximum_pending = 256;
constexpr uint64_t wait_timeout_ns = 1'000'000;
constexpr uint64_t observation_limit_ns = 2'000'000'000;
constexpr int32_t success = 0;
constexpr int32_t timeout = 2;
constexpr int32_t suboptimal = 1000001003;
enum class State : uint8_t {
    Pending, Complete, Unsupported, QueueFailed, WaitFailed,
    PendingOverflow, Expired, Retired
};
constexpr std::array state_names{"pending", "khr_complete", "unsupported", "queue_failed",
    "wait_failed", "pending_overflow", "unresolved_timeout", "unresolved_retired"};
constexpr std::array kind_names{"real", "generated", "unattributed"};
const char* mode_name(int32_t mode) noexcept {
    switch (mode) {
        case 0: return "immediate";
        case 1: return "mailbox";
        case 2: return "fifo";
        case 3: return "fifo_relaxed";
        case 1000111000: return "shared_demand_refresh";
        case 1000111001: return "shared_continuous_refresh";
        default: return "unknown";
    }
}

std::filesystem::path environment_output() {
    const char* path = std::getenv("HEAVEN_FG_PRESENT_TRACE");
    return path && *path ? std::filesystem::path(path) : std::filesystem::path{};
}
void replace_atomic(const std::filesystem::path& temporary, const std::filesystem::path& output) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Presentation trace atomic replacement failed: " +
            std::to_string(GetLastError()));
#else
    std::filesystem::rename(temporary, output);
#endif
}
} // namespace

struct FrameGenerationPresentTrace::Impl {
    struct Sample {
        PresentTraceSubmission submission;
        // A producer owns only this immutable submission until publication.
        // The observer owns every result field after the acquire load.
        std::atomic<bool> ready{false};
        uint64_t lower_ns = 0;
        uint64_t upper_ns = 0;
        uint64_t wait_calls = 0;
        uint64_t wait_total_ns = 0;
        int32_t wait_result = 0;
        State state = State::Pending;
    };
    std::filesystem::path output;
    uint64_t origin_ns = FrameGenerationPresentTrace::now_ns();
    std::array<Sample, maximum_samples> samples{};
    std::array<size_t, maximum_pending> pending{};
    std::atomic<uint64_t> tickets{0};
    size_t imported_count = 0;
    size_t pending_head = 0;
    size_t pending_count = 0;
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> wake_epoch{0};
    std::thread worker;
    Wait wait = nullptr;
    void* userdata = nullptr;
    bool supported = false;
    std::atomic<bool> stopping{false};
    bool worker_unavailable = false;
    std::string reason = "Awaiting Vulkan presentation timing capability";
    std::string lifecycle_reason;
    bool lifecycle_reason_unavailable = false;

    explicit Impl(std::filesystem::path path) : output(std::move(path)) {}
    ~Impl() { stop("Observer destroyed"); }

    void wake() noexcept {
        wake_epoch.fetch_add(1, std::memory_order_release);
        wake_epoch.notify_one();
    }

    // Registration happens at serialized lifecycle boundaries, never while a
    // presentation is publishing a sample. A failed start leaves submissions
    // in their bounded slots for explicit WaitFailed classification at finish.
    void start() noexcept {
        stopping.store(false, std::memory_order_release);
        worker_unavailable = false;
        try { worker = std::thread([this] { observe(); }); }
        catch (...) { worker_unavailable = true; }
    }

    size_t retained_count() const noexcept {
        const auto count = tickets.load(std::memory_order_relaxed);
        return static_cast<size_t>(count < maximum_samples ? count : maximum_samples);
    }

    void import_submissions(bool retiring) noexcept {
        const auto count = retained_count();
        while (imported_count < count) {
            auto& sample = samples[imported_count];
            // Multiple producers may publish reserved tickets out of order.
            // A later ready ticket cannot expose an earlier producer's writes.
            if (!sample.ready.load(std::memory_order_acquire)) break;
            const auto index = imported_count++;
            const auto& submission = sample.submission;
            sample.lower_ns = submission.submit_begin_ns;
            if (submission.queue_result != success && submission.queue_result != suboptimal) {
                sample.state = State::QueueFailed;
                sample.lower_ns = 0;
            } else if (!supported || !submission.native_chain || !submission.present_id) {
                sample.state = State::Unsupported;
                sample.lower_ns = 0;
            } else if (worker_unavailable) {
                sample.state = State::WaitFailed;
                sample.wait_result = -1;
            } else if (pending_count == maximum_pending) sample.state = State::PendingOverflow;
            else {
                sample.state = retiring ? State::Retired : State::Pending;
                pending[(pending_head + pending_count) % maximum_pending] = index;
                ++pending_count;
            }
        }
    }

    void observe() noexcept {
        while (!stopping.load(std::memory_order_acquire)) {
            // Read the wake generation BEFORE inspecting the ready slots. A
            // publication between inspection and wait changes the value and
            // cannot become a lost condition-variable notification.
            const auto epoch = wake_epoch.load(std::memory_order_acquire);
            import_submissions(false);
            if (stopping.load(std::memory_order_acquire)) break;
            if (!pending_count) {
                wake_epoch.wait(epoch, std::memory_order_acquire);
                continue;
            }
            const auto index = pending[pending_head];
            pending_head = (pending_head + 1) % maximum_pending;
            --pending_count;
            auto& sample = samples[index];
            const auto native_chain = sample.submission.native_chain;
            const auto present_id = sample.submission.present_id;
            const auto begin = FrameGenerationPresentTrace::now_ns();
            int32_t result = -13; // VK_ERROR_UNKNOWN, including a throwing diagnostic adapter.
            try { result = wait(userdata, native_chain, present_id, wait_timeout_ns); }
            catch (...) { result = -13; }
            const auto end = FrameGenerationPresentTrace::now_ns();
            if (result == timeout && end - begin < wait_timeout_ns)
                std::this_thread::sleep_for(std::chrono::nanoseconds(wait_timeout_ns - (end - begin)));
            ++sample.wait_calls;
            sample.wait_total_ns += end - begin;
            sample.wait_result = result;
            if (result == success || result == suboptimal) {
                sample.upper_ns = end;
                sample.state = State::Complete;
            } else if (result != timeout) {
                sample.state = State::WaitFailed;
            } else {
                // TIMEOUT establishes only a conservative bound at call entry;
                // scheduling and implementation timeout accuracy are unbounded.
                sample.lower_ns = begin;
                if (stopping.load(std::memory_order_acquire)) sample.state = State::Retired;
                else if (end - sample.submission.submit_begin_ns >= observation_limit_ns)
                    sample.state = State::Expired;
                else if (pending_count == maximum_pending) sample.state = State::PendingOverflow;
                else {
                    pending[(pending_head + pending_count) % maximum_pending] = index;
                    ++pending_count;
                }
            }
        }
        // Lifecycle callers have completed all producers before stop. Import
        // even records the observer had not yet visited; never lose a ready
        // source identity merely because its handle is about to retire.
        import_submissions(true);
        while (pending_count) {
            samples[pending[pending_head]].state = State::Retired;
            pending_head = (pending_head + 1) % maximum_pending;
            --pending_count;
        }
    }

    void stop(const char* label) noexcept {
        stopping.store(true, std::memory_order_release);
        try { lifecycle_reason = label; lifecycle_reason_unavailable = false; }
        catch (...) { lifecycle_reason_unavailable = true; }
        wake();
        if (worker.joinable()) worker.join();
        else import_submissions(true);
    }

    void export_json() {
        using Json = nlohmann::json;
        Json document{{"format", "heaven-present-trace"}, {"schema", 1},
            {"clock", "host steady_clock; nanoseconds relative to origin"},
            {"origin_ns", std::to_string(origin_ns)},
            {"available", supported && !worker_unavailable},
            {"reason", worker_unavailable ? reason + "; observer worker unavailable" : reason},
            {"lifecycle_reason", lifecycle_reason}, {"lifecycle_reason_unavailable", lifecycle_reason_unavailable},
            {"dropped_samples", dropped.load()},
            {"limits", {{"samples", maximum_samples}, {"pending", maximum_pending},
                {"requested_wait_timeout_ns", wait_timeout_ns}, {"observation_limit_ns", observation_limit_ns}}},
            {"semantics", "KHR present-ID completion bounds; no precise scanout clock or physical input latency. "
                "MAILBOX/replacing modes may signal replaced images without displaying each image. "
                "Timeout accuracy and worker scheduling widen bounds. Opt-in observer overhead is included."}};
        if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
        auto temporary = output;
        static std::atomic<uint64_t> serial{0};
        temporary += ".tmp-" + std::to_string(FrameGenerationPresentTrace::now_ns()) +
            "-" + std::to_string(serial.fetch_add(1));
        try {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.exceptions(std::ios::failbit | std::ios::badbit);
            const auto metadata = document.dump();
            stream.write(metadata.data(), static_cast<std::streamsize>(metadata.size() - 1));
            stream << ",\"samples\":[\n";
            for (size_t index = 0; index < imported_count; ++index) {
                const auto& sample = samples[index];
                const auto& source = sample.submission;
                const Json record{{"chain", source.chain}, {"present_id", source.present_id},
                    {"source_frame", source.source_frame},
                    {"kind", kind_names.at(static_cast<size_t>(source.kind))},
                    {"present_mode", source.present_mode}, {"present_mode_label", mode_name(source.present_mode)},
                    {"queue_result", source.queue_result},
                    {"submit_begin_ns", source.submit_begin_ns - origin_ns},
                    {"submit_end_ns", source.submit_end_ns - origin_ns},
                    {"status", state_names.at(static_cast<size_t>(sample.state))},
                    {"khr_signal_lower_ns", sample.lower_ns ? Json(sample.lower_ns - origin_ns) : Json(nullptr)},
                    {"khr_signal_upper_ns", sample.upper_ns ? Json(sample.upper_ns - origin_ns) : Json(nullptr)},
                    {"wait_result", sample.wait_calls ? Json(sample.wait_result) : Json(nullptr)},
                    {"wait_calls", sample.wait_calls}, {"wait_total_ns", sample.wait_total_ns}};
                if (index) stream << ",\n";
                stream << record.dump();
            }
            stream << "\n]}\n";
            stream.close();
            replace_atomic(temporary, output);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }

    void finish(const char* label, bool restart) noexcept {
        stop(label);
        try { export_json(); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "Presentation trace export failed: %s\n", error.what());
        } catch (...) {
            std::fputs("Presentation trace export failed\n", stderr);
        }
        // A replacement is registered at this serialized lifecycle boundary.
        // imported_count already covers every old published ticket, so this
        // idle worker cannot access a retired handle. Only a later submission
        // can provide work after its caller has completed native recreation.
        if (restart) start();
    }
};

FrameGenerationPresentTrace::FrameGenerationPresentTrace()
    : FrameGenerationPresentTrace(environment_output()) {}
FrameGenerationPresentTrace::FrameGenerationPresentTrace(std::filesystem::path output) {
    if (!output.empty()) {
        impl_ = std::make_unique<Impl>(std::move(output));
        impl_->start();
    }
}
FrameGenerationPresentTrace::~FrameGenerationPresentTrace() {
    if (impl_) impl_->finish("Trace destroyed", false);
}
bool FrameGenerationPresentTrace::requested() const noexcept { return impl_ != nullptr; }
bool FrameGenerationPresentTrace::available() const noexcept {
    return impl_ && impl_->supported && !impl_->worker_unavailable;
}

void FrameGenerationPresentTrace::configure(bool available, std::string reason, Wait wait, void* userdata) {
    if (!impl_) return;
    impl_->stop("Capability changed");
    impl_->supported = available && wait;
    impl_->wait = wait;
    impl_->userdata = userdata;
    impl_->reason = std::move(reason);
    impl_->start();
}

void FrameGenerationPresentTrace::submitted(const PresentTraceSubmission& submission) noexcept {
    if (!impl_) return;
    // No observer mutex, GPU wait, allocation or thread creation occurs here.
    // Each producer reserves a distinct slot and publishes only its immutable
    // submission. Completion and pending-queue state belongs to the observer.
    if (impl_->tickets.load(std::memory_order_relaxed) >= maximum_samples) {
        impl_->dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto index = impl_->tickets.fetch_add(1, std::memory_order_relaxed);
    if (index >= maximum_samples) {
        impl_->dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& sample = impl_->samples[static_cast<size_t>(index)];
    sample.submission = submission;
    sample.ready.store(true, std::memory_order_release);
    impl_->wake();
}

void FrameGenerationPresentTrace::finish(const char* reason) noexcept {
    if (!impl_) return;
    impl_->finish(reason, true);
}

uint64_t FrameGenerationPresentTrace::now_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace heaven
