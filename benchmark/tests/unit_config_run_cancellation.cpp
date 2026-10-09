/*
 * An accessible Settings window must not produce a score with mixed settings.
 */
#include "rhi/overlay/config_controller.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

struct RunEffects : heaven::RendererEffects {
    explicit RunEffects(const heaven::BenchConfig& config) : active(config) {}

    const heaven::BenchConfig& active;
    bool running = true;
    bool mutation_while_running = false;
    float time_at_cancel = -1.0f;
    int cancels = 0;
    int rebuilds = 0;
    int live_calls = 0;

    void cancel_active_run() override {
        time_at_cancel = active.time_of_day;
        running = false;
        ++cancels;
    }
    void set_feature_enabled(const char* /*name*/, bool /*on*/) override {
        mutation_while_running |= running;
        ++live_calls;
    }
    void apply_live(heaven::SettingId /*id*/, const heaven::BenchConfig& /*config*/) override {
        mutation_while_running |= running;
        ++live_calls;
    }
    heaven::Result<void> rebuild() override {
        mutation_while_running |= running;
        ++rebuilds;
        return {};
    }
};

bool require(bool condition, const char* description) {
    if (!condition) {
        std::printf("[unit_config_run_cancellation] FAILED: %s\n", description);
    }
    return condition;
}

struct TempConfig {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("heaven_run_configuration_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    ~TempConfig() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    void write(const char* value) const { std::ofstream(path) << value; }
};

bool configuration_cancels_before_mutation() {
    using namespace heaven;
    BenchConfig active;
    active.time_of_day = 12.0f;
    BenchConfig working = active;
    ConfigUiState ui;
    RunEffects fx(active);
    OverlayIntents intents;
    intents.edits = {{SettingId::TimeOfDay, 8.0f},
                     {SettingId::FeatureSsgi, false}};
    intents.reload = true;
    apply_config_intents(working, active, ui, intents, fx);
    if (!require(!fx.running && !fx.mutation_while_running &&
                 fx.time_at_cancel == 12.0f && active.time_of_day == 8.0f &&
                 fx.cancels == 1 && fx.rebuilds == 1,
                 "live edits/reload changed the workload before one cancellation")) {
        return false;
    }

    // An unchanged Live value does not invalidate the current workload.
    RunEffects unchanged(active);
    intents = {};
    intents.edits = {{SettingId::TimeOfDay, 8.0f}};
    apply_config_intents(working, active, ui, intents, unchanged);
    if (!require(unchanged.running && unchanged.cancels == 0 &&
                 active.time_of_day == 8.0f,
                 "a no-op live edit unexpectedly cancelled playback")) return false;

    // Viewing Settings or editing a pending reload field keeps the run intact.
    RunEffects pending(active);
    intents = {};
    intents.edits = {{SettingId::TessScale, 0.4f}};
    apply_config_intents(working, active, ui, intents, pending);
    if (!require(pending.running && pending.cancels == 0 &&
                 pending.live_calls == 0 && pending.rebuilds == 0 && ui.dirty,
                 "pending reload settings unexpectedly cancelled playback")) {
        return false;
    }
    intents = {};
    intents.open_panel = OverlayIntents::Panel::Quality;
    apply_config_intents(working, active, ui, intents, pending);
    if (!require(pending.running && pending.cancels == 0,
                 "opening Settings unexpectedly cancelled playback")) return false;

    // A successful file load changes Live fields before its scene rebuild.
    TempConfig file;
    file.write(R"({"time_of_day":16.0,"features.ssgi":true})");
    RunEffects loaded(active);
    intents = {};
    intents.load = true;
    intents.load_path = file.path.string();
    apply_config_intents(working, active, ui, intents, loaded);
    if (!require(!loaded.running && !loaded.mutation_while_running &&
                 loaded.time_at_cancel == 8.0f && active.time_of_day == 16.0f &&
                 loaded.cancels == 1 && loaded.rebuilds == 1,
                 "successful load changed live settings before cancellation")) {
        return false;
    }
    // Parsing failure does not change any workload or retire a valid run.
    file.write("{invalid JSON");
    RunEffects failed(active);
    apply_config_intents(working, active, ui, intents, failed);
    return require(failed.running && failed.cancels == 0 &&
                   failed.live_calls == 0 && failed.rebuilds == 0 &&
                   active.time_of_day == 16.0f,
                   "failed configuration load interrupted or mutated the run");
}

heaven::testing::Register register_config_run_cancellation(
    configuration_cancels_before_mutation);

} // namespace
