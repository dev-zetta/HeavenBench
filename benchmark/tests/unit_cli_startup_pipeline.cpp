#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/render_presets.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
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

struct ConfigFixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("heaven-cli-startup-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    ~ConfigFixture() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

bool unit_cli_startup_pipeline() {
    bool ok = true;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::printf("[unit_cli_startup_pipeline] FAILED: %s\n", message);
            ok = false;
        }
    };
    ConfigFixture file;
    {
        std::ofstream output(file.path);
        output << R"({"quality_preset":"visual-ultra","width":1400,"height":900,
            "time_of_day":21,"data_dir":"file assets","quality.msaa_samples":4,
            "texture_quality":"4k","tessellation.cache_file":"file-cache.zip",
            "music.enabled":false})";
        output.close();
        if (!output) return false;
    }

    // Match application startup: parse actions first, load the requested file
    // over defaults, then overlay CLI settings. Release argv before replay to
    // also exercise ownership of strings in the deferred operations.
    auto startup = [&](std::vector<std::string> flags) {
        heaven::ParseResult parsed;
        {
            std::vector<std::string> argv{"heaven_benchmark", "--config", file.path.string()};
            argv.insert(argv.end(), flags.begin(), flags.end());
            parsed = heaven::parse_cli(argv);
        }
        const auto loaded = heaven::load_config_file(*parsed.config_path, heaven::BenchConfig{});
        check(loaded.parsed_ok && loaded.diagnostics.empty(), "fixture file did not load cleanly");
        auto config = loaded.config;
        heaven::apply_cli_overlay(config, parsed);
        return config;
    };

    const auto advanced = startup({
        "--color-grading", "--physical-camera", "--dynamic-resolution",
        "--tone-mapper", "agx", "--grading-contrast", "1.5",
        "--grading-saturation", "0.25", "--white-balance", "0.5,-0.25",
        "--grading-slope", "0.5,1,1.5", "--grading-offset", "0.1,0.2,0.3",
        "--grading-power", "1.5,1,0.5", "--aperture", "4",
        "--shutter", "0.002", "--iso", "400",
        "--dynamic-resolution-quality", "0.75", "--specular-aa-quality", "0",
        "--width", "1280", "--data", "CLI assets with spaces/owned-string-storage"});
    check(advanced.features.color_grading && advanced.features.physical_camera &&
          advanced.features.dynamic_resolution, "CLI-only capability toggles were lost");
    check(advanced.tone_mapper == heaven::ToneMapperSelection::AgX,
          "CLI tone mapper was lost");
    check(advanced.grading.contrast == 1.5f && advanced.grading.saturation == 0.25f &&
          advanced.grading.temperature == 0.5f && advanced.grading.tint == -0.25f &&
          advanced.grading.cdl_slope == glm::vec3(0.5f, 1.0f, 1.5f) &&
          advanced.grading.cdl_offset == glm::vec3(0.1f, 0.2f, 0.3f) &&
          advanced.grading.cdl_power == glm::vec3(1.5f, 1.0f, 0.5f),
          "CLI grading parameters were lost");
    check(advanced.physical_camera.aperture == 4.0f &&
          advanced.physical_camera.shutter_speed == 0.002f &&
          advanced.physical_camera.sensitivity == 400.0f,
          "CLI physical-camera parameters were lost");
    check(advanced.quality.dynres_min_scale == 0.75f &&
          advanced.quality.specular_aa_level == heaven::QualityLevel::Low,
          "CLI-only quality parameters were lost");
    check(advanced.width == 1280 &&
          advanced.data_dir == "CLI assets with spaces/owned-string-storage" &&
          advanced.height == 900 && advanced.time_of_day == 21.0f &&
          !advanced.music.enabled && advanced.tessellation.cache_file == "file-cache.zip",
          "CLI precedence, deferred string ownership or unrelated file state changed");

    const auto rejected = startup({"--width", "invalid", "--msaa-quality", "3",
        "--texture-quality", "unknown", "--preset", "unknown", "--height"});
    check(rejected.width == 1400 && rejected.height == 900 &&
          rejected.quality.msaa_samples == 4 &&
          rejected.texture_quality == heaven::TextureQuality::Ultra4K &&
          rejected.render_preset == heaven::RenderPreset::VisualUltra &&
          rejected.quality_preset_label == "Visual Ultra",
          "rejected or missing CLI values replaced loaded settings with defaults");

    const auto override_after = startup({"--preset", "balanced", "--dynamic-resolution",
        "--specular-aa-quality", "0", "--no-bloom", "--msaa-quality", "1"});
    check(override_after.features.dynamic_resolution && !override_after.features.bloom &&
          override_after.quality.specular_aa_level == heaven::QualityLevel::Low &&
          override_after.quality.msaa_samples == 1 &&
          heaven::detect_render_preset(override_after) == heaven::RenderPreset::Custom,
          "settings after a CLI preset did not override it");

    const auto preset_after = startup({"--dynamic-resolution", "--specular-aa-quality", "0",
        "--no-bloom", "--msaa-quality", "1", "--preset", "balanced"});
    check(!preset_after.features.dynamic_resolution && preset_after.features.bloom &&
          preset_after.quality.specular_aa_level == heaven::QualityLevel::Medium &&
          preset_after.quality.msaa_samples == 2 &&
          heaven::detect_render_preset(preset_after) == heaven::RenderPreset::BalancedSystem &&
          preset_after.height == 900 && preset_after.tessellation.cache_file == "file-cache.zip",
          "later preset did not reset owned fields or changed unrelated file fields");

    const auto alias = startup({"--preset", "balanced", "--quality-preset", "visual-ultra",
        "--specular-aa-quality", "0", "--quality-preset", "custom"});
    check(alias.render_preset == heaven::RenderPreset::Custom &&
          alias.quality_preset_label == "Custom" && alias.quality.msaa_samples == 4 &&
          alias.quality.specular_aa_level == heaven::QualityLevel::Low,
          "deprecated preset alias/custom ordering lost configuration changes");

    const auto repeated = startup({"--tone-mapper", "gt7", "--tone-mapper", "pbr-neutral",
        "--no-grass", "--dynamic-resolution-quality", "0.5",
        "--dynamic-resolution-quality", "0.75"});
    check(repeated.tone_mapper == heaven::ToneMapperSelection::PBRNeutral &&
          repeated.quality.dynres_min_scale == 0.75f && !repeated.features.grass,
          "repeated flags did not preserve last occurrence");

    // The compatibility entry point must share the same complete overlay and
    // retain caller-owned nonpersistent controls when no flag changes them.
    heaven::AppConfig existing = advanced;
    const std::vector<std::string_view> flags{"heaven_benchmark", "--width", "1024"};
    heaven::apply_cli_arguments(existing, flags);
    check(existing.width == 1024 && existing.features.dynamic_resolution &&
          existing.grading.tint == -0.25f && existing.tone_mapper == heaven::ToneMapperSelection::AgX,
          "compatibility overlay reset an absent CLI-only setting");

    return ok;
}

const heaven::testing::Register reg_cli_startup_pipeline{unit_cli_startup_pipeline};

} // namespace
