#include "frame_generation_present_trace.h"

#include "json.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
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
    size_t sample_count = 0;
    size_t pending_head = 0;
    size_t pending_count = 0;
    std::atomic<uint64_t> dropped{0};
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    Wait wait = nullptr;
    void* userdata = nullptr;
    bool supported = false;
    bool stopping = false;
    std::string reason = "Awaiting Vulkan presentation timing capability";
    std::string lifecycle_reason;
    bool lifecycle_reason_unavailable = false;

    explicit Impl(std::filesystem::path path) : output(std::move(path)) {}
    ~Impl() { stop("Observer destroyed"); }

    void observe() noexcept {
        std::unique_lock lock(mutex);
        while (!stopping) {
            changed.wait(lock, [&] { return stopping || pending_count != 0; });
            if (stopping) break;
            const auto index = pending[pending_head];
            pending_head = (pending_head + 1) % maximum_pending;
            --pending_count;
            auto& sample = samples[index];
            const auto native_chain = sample.submission.native_chain;
            const auto present_id = sample.submission.present_id;
            lock.unlock();
            const auto begin = FrameGenerationPresentTrace::now_ns();
            int32_t result = -13; // VK_ERROR_UNKNOWN, including a throwing diagnostic adapter.
            try { result = wait(userdata, native_chain, present_id, wait_timeout_ns); }
            catch (...) { result = -13; }
            const auto end = FrameGenerationPresentTrace::now_ns();
            if (result == timeout && end - begin < wait_timeout_ns)
                std::this_thread::sleep_for(std::chrono::nanoseconds(wait_timeout_ns - (end - begin)));
            lock.lock();
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
                if (stopping) sample.state = State::Retired;
                else if (end - sample.submission.submit_begin_ns >= observation_limit_ns)
                    sample.state = State::Expired;
                else if (pending_count == maximum_pending) sample.state = State::PendingOverflow;
                else {
                    pending[(pending_head + pending_count) % maximum_pending] = index;
                    ++pending_count;
                }
            }
        }
        while (pending_count) {
            samples[pending[pending_head]].state = State::Retired;
            pending_head = (pending_head + 1) % maximum_pending;
            --pending_count;
        }
    }

    void stop(const char* label) noexcept {
        {
            std::scoped_lock lock(mutex);
            stopping = true;
            try { lifecycle_reason = label; lifecycle_reason_unavailable = false; }
            catch (...) { lifecycle_reason_unavailable = true; }
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();
    }

    void export_json() {
        using Json = nlohmann::json;
        Json document{{"format", "heaven-present-trace"}, {"schema", 1},
            {"clock", "host steady_clock; nanoseconds relative to origin"},
            {"origin_ns", std::to_string(origin_ns)}, {"available", supported}, {"reason", reason},
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
            for (size_t index = 0; index < sample_count; ++index) {
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
};

FrameGenerationPresentTrace::FrameGenerationPresentTrace()
    : FrameGenerationPresentTrace(environment_output()) {}
FrameGenerationPresentTrace::FrameGenerationPresentTrace(std::filesystem::path output) {
    if (!output.empty()) impl_ = std::make_unique<Impl>(std::move(output));
}
FrameGenerationPresentTrace::~FrameGenerationPresentTrace() { finish("Trace destroyed"); }
bool FrameGenerationPresentTrace::requested() const noexcept { return impl_ != nullptr; }
bool FrameGenerationPresentTrace::available() const noexcept { return impl_ && impl_->supported; }

void FrameGenerationPresentTrace::configure(bool available, std::string reason, Wait wait, void* userdata) {
    if (!impl_) return;
    impl_->stop("Capability changed");
    impl_->supported = available && wait;
    impl_->wait = wait;
    impl_->userdata = userdata;
    impl_->reason = std::move(reason);
}

void FrameGenerationPresentTrace::submitted(const PresentTraceSubmission& submission) noexcept {
    if (!impl_) return;
    std::unique_lock lock(impl_->mutex, std::try_to_lock);
    if (!lock.owns_lock() || impl_->sample_count == maximum_samples) {
        ++impl_->dropped;
        return;
    }
    const auto index = impl_->sample_count++;
    auto& sample = impl_->samples[index];
    sample.submission = submission;
    sample.lower_ns = submission.submit_begin_ns;
    if (submission.queue_result != success && submission.queue_result != suboptimal) {
        sample.state = State::QueueFailed;
        sample.lower_ns = 0;
    } else if (!impl_->supported || !submission.native_chain || !submission.present_id) {
        sample.state = State::Unsupported;
        sample.lower_ns = 0;
    } else if (impl_->pending_count == maximum_pending)
        sample.state = State::PendingOverflow;
    else {
        try {
            if (!impl_->worker.joinable()) {
                impl_->stopping = false;
                impl_->worker = std::thread([state = impl_.get()] { state->observe(); });
            }
            impl_->pending[(impl_->pending_head + impl_->pending_count) % maximum_pending] = index;
            ++impl_->pending_count;
            impl_->changed.notify_one();
        } catch (...) {
            sample.state = State::WaitFailed;
            sample.wait_result = -1;
        }
    }
}

void FrameGenerationPresentTrace::finish(const char* reason) noexcept {
    if (!impl_) return;
    impl_->stop(reason);
    try { impl_->export_json(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Presentation trace export failed: %s\n", error.what());
    } catch (...) {
        std::fputs("Presentation trace export failed\n", stderr);
    }
}

uint64_t FrameGenerationPresentTrace::now_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace heaven
