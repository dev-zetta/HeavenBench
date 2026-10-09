/*
 * HeavenBench — Android preview deadlines and persistent launcher settings
 */
#include "engine/config_file.h"
#include "engine/preview_frame_pacing.h"
#include "engine/types.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string_view>
#include <vector>

namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

bool require(bool condition, const char* message) {
    if (!condition) {
        std::printf("[unit_android_preview_pacing] FAILED: %s\n", message);
    }
    return condition;
}

bool deadline_tests() {
    using Clock = heaven::PreviewFramePacer::Clock;
    using namespace std::chrono_literals;
    heaven::PreviewFramePacer pacer;
    const Clock::time_point epoch{};
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / 30.0));
    if (!require(pacer.wait_before_frame(epoch, 30, true) == Clock::duration::zero(),
                 "the first preview frame was delayed") ||
        !require(pacer.wait_before_frame(epoch + 10ms, 30, true) == period - 10ms,
                 "an early preview frame escaped its deadline") ||
        !require(pacer.wait_before_frame(epoch + 20ms, 30, true) == period - 20ms,
                 "an early wake incorrectly advanced the deadline") ||
        !require(pacer.wait_before_frame(epoch + period, 30, true) == Clock::duration::zero(),
                 "the deadline did not permit submission")) return false;

    // A slow render cannot create a burst of overdue catch-up submissions.
    const auto late = epoch + 500ms;
    if (!require(pacer.wait_before_frame(late, 30, true) == Clock::duration::zero(),
                 "a late frame was delayed again") ||
        !require(pacer.wait_before_frame(late + 1ms, 30, true) == period - 1ms,
                 "missed deadlines permitted a catch-up burst")) return false;

    for (const auto state : std::array{
             std::array{true, false, false, false},
             std::array{false, true, false, false},
             std::array{false, false, true, false},
             std::array{false, false, false, true}}) {
        const bool allowed = heaven::preview_pacing_allowed(
            state[0], state[1], state[2], state[3]);
        if (!require(!allowed && pacer.wait_before_frame(late, 30, allowed) ==
                         Clock::duration::zero(),
                     "benchmark/capture/profile/pause was paced") ||
            !require(pacer.wait_before_frame(late, 30, true) == Clock::duration::zero(),
                     "leaving an unpaced state retained a stale deadline")) return false;
    }
    if (!require(pacer.wait_before_frame(late, 0, true) == Clock::duration::zero(),
                 "zero FPS did not disable preview pacing") ||
        !require(pacer.wait_before_frame(late, 60, true) == Clock::duration::zero(),
                 "changing the cap retained the previous deadline")) return false;
    pacer.reset();
    return require(pacer.wait_before_frame(epoch, 30, true) == Clock::duration::zero(),
                   "resize/reset retained an old preview deadline");
}

struct TempConfig {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("heaven_android_preview_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    ~TempConfig() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    void write(std::string_view value) const { std::ofstream(path) << value; }
};

bool configuration_tests() {
    heaven::BenchConfig base;
#ifdef __ANDROID__
    if (!require(base.android_preview_fps == 30,
                 "Android preview did not default to 30 FPS")) return false;
#else
    if (!require(base.android_preview_fps == 0,
                 "desktop preview default was changed")) return false;
#endif
    if (!require(base.android_render_scale == 1.0f,
                 "the default Android Surface scale changed")) return false;
    TempConfig file;
    file.write(R"({"android_preview_fps":30,"android_render_scale":0.375})");
    const auto loaded = heaven::load_config_file(file.path.string(), base);
    if (!require(loaded.parsed_ok && loaded.diagnostics.empty() &&
                     loaded.config.android_preview_fps == 30 &&
                     loaded.config.android_render_scale == 0.375f,
                 "valid preview/Surface settings did not load")) return false;
    const auto written = heaven::write_config_file(file.path.string(), loaded.config);
    const auto roundtrip = heaven::load_config_file(file.path.string(), base);
    if (!require(written.empty() && roundtrip.parsed_ok && roundtrip.diagnostics.empty() &&
                     roundtrip.config.android_preview_fps == 30 &&
                     roundtrip.config.android_render_scale == 0.375f,
                 "saved settings dropped the Android launcher fields")) return false;

    for (const auto invalid : std::array{
             R"({"android_preview_fps":-1,"android_render_scale":0.124})",
             R"({"android_preview_fps":241,"android_render_scale":1.01})",
             R"({"android_preview_fps":4294967326,"android_render_scale":1e300})",
             R"({"android_preview_fps":30.5,"android_render_scale":"nan"})"}) {
        file.write(invalid);
        const auto rejected = heaven::load_config_file(file.path.string(), loaded.config);
        if (!require(rejected.parsed_ok && rejected.diagnostics.size() == 2 &&
                         rejected.config.android_preview_fps == 30 &&
                         rejected.config.android_render_scale == 0.375f,
                     "invalid launcher setting overwrote the previous value")) return false;
    }
    const std::array boundaries{
             R"({"android_preview_fps":0,"android_render_scale":0.125})",
             R"({"android_preview_fps":240,"android_render_scale":1})"};
    return std::ranges::all_of(boundaries, [&](const auto boundary) {
        file.write(boundary);
        const auto accepted = heaven::load_config_file(file.path.string(), base);
        return require(accepted.parsed_ok && accepted.diagnostics.empty(),
                       "a supported launcher boundary was rejected");
    });
}

bool unit_android_preview_pacing() {
    return deadline_tests() && configuration_tests();
}

const heaven::testing::Register kRegister(unit_android_preview_pacing);

} // namespace
