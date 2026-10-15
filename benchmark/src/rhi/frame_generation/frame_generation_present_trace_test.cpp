#include "frame_generation_present_trace.h"
#include "json.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using namespace heaven;
using Json = nlohmann::json;
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct WaitState {
    std::atomic<uint32_t> calls{0};
    std::atomic<bool> active{false};
    std::atomic<bool> retired{false};
    std::atomic<bool> invalid{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool hold = false;
    bool release = false;
    bool first_timeout = false;
    int32_t result = 0;
    static int32_t wait(void* userdata, uint64_t native_chain, uint64_t present_id, uint64_t timeout_ns) {
        auto& state = *static_cast<WaitState*>(userdata);
        state.active = true;
        const auto count = ++state.calls;
        if (state.retired || !native_chain || !present_id || timeout_ns != 1'000'000) state.invalid = true;
        {
            std::unique_lock lock(state.mutex);
            if (state.hold) state.changed.wait(lock, [&] { return state.release; });
        }
        if (state.retired) state.invalid = true;
        state.active = false;
        return state.first_timeout && count == 1 ? 2 : state.result;
    }
};
void await_calls(WaitState& state, uint32_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (state.calls < count && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(state.calls >= count, "Observer callback did not run");
}
PresentTraceSubmission submission(uint64_t id = 1, uint64_t chain = 1) {
    PresentTraceSubmission result;
    result.chain = chain;
    result.native_chain = chain;
    result.present_id = id;
    result.source_frame = (id + 1) / 2;
    result.kind = id % 2 ? PresentTraceKind::Generated : PresentTraceKind::Real;
    result.present_mode = 1; // MAILBOX: completion may cover a replaced image.
    result.submit_begin_ns = result.submit_end_ns = FrameGenerationPresentTrace::now_ns();
    return result;
}
Json read(const std::filesystem::path& path) {
    std::ifstream stream(path);
    return Json::parse(stream);
}
void exercise(const std::filesystem::path& directory) {
    FrameGenerationPresentTrace disabled(std::filesystem::path{});
    require(!disabled.requested() && !disabled.available(), "Empty trace path must remain disabled");
    disabled.submitted(submission());
    disabled.finish("No output");

    const auto path = directory / "trace.json";
    {
        FrameGenerationPresentTrace trace(path);
        trace.configure(false, "Unsupported fixture device");
        auto unknown_mode = submission();
        unknown_mode.present_mode = PresentTraceSubmission{}.present_mode;
        trace.submitted(unknown_mode);
        trace.finish("Unsupported device");
        const auto document = read(path);
        require(!document.at("available").get<bool>(), "Unsupported device invented capability");
        const auto& sample = document.at("samples").at(0);
        require(sample.at("status") == "unsupported" && sample.at("khr_signal_lower_ns").is_null() &&
            sample.at("khr_signal_upper_ns").is_null(), "Unsupported timing invented completion bounds");
        require(sample.at("present_mode") == INT32_MAX && sample.at("present_mode_label") == "unknown",
            "Uninitialized/headless presentation mode invented IMMEDIATE mode");
    }
    {
        WaitState state;
        state.first_timeout = true;
        FrameGenerationPresentTrace trace(path);
        trace.configure(true, "Fake KHR completion", WaitState::wait, &state);
        trace.submitted(submission());
        await_calls(state, 2);
        trace.finish("Complete");
        auto document = read(path);
        const auto& sample = document.at("samples").at(0);
        require(sample.at("status") == "khr_complete" && sample.at("wait_calls") == 2,
            "Timeout followed by completion was lost");
        require(sample.at("submit_begin_ns") <= sample.at("khr_signal_lower_ns") &&
            sample.at("khr_signal_lower_ns") <= sample.at("khr_signal_upper_ns"), "Completion bounds reversed");
        require(sample.at("kind") == "generated" && sample.at("source_frame") == 1 &&
            sample.at("present_mode") == 1, "Submission identity or actual mode lost");
        require(!state.invalid, "Wait received invalid identity/timeout or retired handle");
        auto failed = submission(2);
        failed.queue_result = -1000001004;
        trace.submitted(failed);
        trace.finish("Queue failure");
        document = read(path);
        require(document.at("samples").at(1).at("status") == "queue_failed" && state.calls == 2,
            "Failed native queue presentation reached a wait");
    }
    {
        WaitState state;
        state.result = -4;
        FrameGenerationPresentTrace trace(path);
        trace.configure(true, "Fake device loss", WaitState::wait, &state);
        trace.submitted(submission());
        await_calls(state, 1);
        trace.finish("Error");
        const auto document = read(path);
        const auto& sample = document.at("samples").at(0);
        require(sample.at("status") == "wait_failed" && sample.at("wait_result") == -4,
            "Device-loss feedback was treated as completed");
    }
    {
        WaitState state;
        state.hold = true;
        state.result = 2;
        FrameGenerationPresentTrace trace(path);
        trace.configure(true, "Fake retained swapchain", WaitState::wait, &state);
        trace.submitted(submission());
        await_calls(state, 1);
        for (uint64_t id = 2; id <= 300; ++id) trace.submitted(submission(id));
        std::atomic<bool> joined{false};
        std::jthread retirement([&] { trace.finish("Recreate before completion"); joined = true; });
        std::this_thread::sleep_for(2ms);
        const bool retained_until_release = !joined && state.active;
        {
            std::scoped_lock lock(state.mutex);
            state.release = true;
        }
        state.changed.notify_one();
        retirement.join();
        state.retired = true;
        require(retained_until_release, "Lifecycle join returned while a wait still owned the handle");
        require(!state.active && !state.invalid, "Swapchain retired concurrently with wait");
        auto document = read(path);
        uint32_t overflow = 0;
        uint32_t unresolved = 0;
        for (const auto& sample : document.at("samples")) {
            overflow += sample.at("status") == "pending_overflow";
            unresolved += sample.at("status") == "unresolved_retired";
        }
        require(overflow != 0 && unresolved != 0, "Overflow/retirement coverage was silently discarded");
        WaitState replacement;
        trace.configure(true, "Replacement native swapchain", WaitState::wait, &replacement);
        trace.submitted(submission(301, 2));
        await_calls(replacement, 1);
        trace.finish("Replacement completed");
        document = read(path);
        require(document.at("samples").back().at("chain") == 2 &&
            document.at("samples").back().at("status") == "khr_complete", "Observer did not restart after recreation");
    }
    {
        FrameGenerationPresentTrace trace(path);
        trace.configure(false, "Concurrent unsupported submissions");
        std::atomic<bool> release{false};
        std::array<std::jthread, 4> producers;
        for (size_t index = 0; index < producers.size(); ++index) {
            producers[index] = std::jthread([&, index] {
                release.wait(false, std::memory_order_acquire);
                for (uint64_t id = 1; id <= 32; ++id)
                    trace.submitted(submission(static_cast<uint64_t>(index) * 32 + id));
            });
        }
        release.store(true, std::memory_order_release);
        release.notify_all();
        for (auto& producer : producers) producer.join();
        // Finish immediately: even published slots the observer had not yet
        // imported must retain complete identities without querying a GPU.
        trace.finish("Concurrent producers drained");
        const auto document = read(path);
        require(document.at("samples").size() == 128 && document.at("dropped_samples") == 0,
            "Concurrent producer admission lost submissions");
        std::set<uint64_t> ids;
        for (const auto& sample : document.at("samples")) {
            const auto id = sample.at("present_id").get<uint64_t>();
            require(ids.insert(id).second && sample.at("source_frame") == (id + 1) / 2 &&
                sample.at("status") == "unsupported", "Concurrent publication corrupted identity/status");
        }
        require(*ids.begin() == 1 && *ids.rbegin() == 128,
            "Concurrent publication omitted a reserved identity");
    }
    {
        FrameGenerationPresentTrace trace(path);
        trace.configure(false, "Sample capacity");
        for (uint64_t id = 1; id <= 32770; ++id) trace.submitted(submission(id));
        trace.finish("Capacity complete");
        const auto document = read(path);
        require(document.at("samples").size() == 32768 && document.at("dropped_samples") == 2,
            "Bounded sample storage overflow was not disclosed");
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            require(!entry.path().filename().string().starts_with("trace.json.tmp-"),
                "Atomic export left a temporary file");
    }
    {
        const auto obstruction = directory / "not-a-directory";
        { std::ofstream stream(obstruction); stream << "keep existing user data"; }
        FrameGenerationPresentTrace trace(obstruction / "trace.json");
        trace.configure(false, "Export failure fixture");
        trace.submitted(submission());
        trace.finish("Expected export failure");
        std::ifstream stream(obstruction);
        std::string bytes;
        std::getline(stream, bytes);
        require(bytes == "keep existing user data", "Export failure overwrote an existing user path");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Pass a writable fixture directory");
        const std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        exercise(directory);
        std::puts("Presentation trace CPU contracts passed: bounds, errors, overflow, retirement/join and restart");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Presentation trace contract failed: %s\n", error.what());
        return 1;
    }
}
