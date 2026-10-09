#include "engine/tessellation_levels.h"
#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/render_presets.h"
#include "rhi/overlay/config_controller.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

heaven::ParseResult parse(std::initializer_list<const char*> arguments) {
    std::vector<const char*> argv{"heaven_benchmark"};
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    return heaven::parse_cli(argv);
}

bool same_controls(const heaven::TessellationConfig& a, const heaven::TessellationConfig& b) {
    return a.enabled == b.enabled && a.scale == b.scale && a.factor == b.factor &&
        a.distance == b.distance && a.cache_file == b.cache_file;
}

bool unit_tessellation_levels() {
    using heaven::TessellationLevel;
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::printf("[unit_tessellation_levels] FAILED: %s\n", message);
            ok = false;
        }
    };
    // Independent expected controls from original Heaven's three setters.
    const std::pair<TessellationLevel, float> cases[] = {
        {TessellationLevel::Moderate, 0.75f}, {TessellationLevel::Normal, 1.0f},
        {TessellationLevel::Extreme, 1.5f}};
    heaven::TessellationConfig custom{false, 0.9f, 1.125f, 2.25f, "custom-cache.zip"};
    for (const auto& [level, multiplier] : cases) {
        auto config = custom;
        heaven::apply_tessellation_level(config, level);
        check(config.enabled && config.scale == multiplier && config.factor == multiplier &&
                  config.distance == multiplier && config.cache_file == custom.cache_file,
              "named level did not set the original three controls or preserve cache storage");
        check(heaven::detect_tessellation_level(config) == level,
              "effective named controls did not derive the selected level");
        config.factor = std::nextafter(config.factor, 2.0f);
        check(heaven::detect_tessellation_level(config) == TessellationLevel::Custom,
              "a nearby manual value was incorrectly rounded into a named level");
    }
    custom.enabled = true;
    check(heaven::detect_tessellation_level(custom) == TessellationLevel::Custom,
          "independent manual values did not derive Custom");
    const auto original = custom;
    heaven::apply_tessellation_level(custom, TessellationLevel::Disabled);
    auto disabled = original;
    disabled.enabled = false;
    check(same_controls(custom, disabled) &&
              heaven::detect_tessellation_level(custom) == TessellationLevel::Disabled,
          "Disabled discarded the saved custom controls");
    custom.enabled = true;
    check(same_controls(custom, original), "re-enabling lost the previous custom controls");
    heaven::apply_tessellation_level(custom, TessellationLevel::Custom);
    check(same_controls(custom, original), "Custom reset existing manual values");
    custom.scale = std::numeric_limits<float>::quiet_NaN();
    check(heaven::detect_tessellation_level(custom) == TessellationLevel::Custom,
          "nonfinite controls were reported as a canonical level");
    check(heaven::parse_tessellation_level("Moderate") == TessellationLevel::Moderate &&
              heaven::parse_tessellation_level("EXTREME") == TessellationLevel::Extreme &&
              !heaven::parse_tessellation_level("custom") &&
              !heaven::parse_tessellation_level("cinematic"),
          "only the selectable named levels should parse");
    check(heaven::detect_tessellation_level(heaven::TessellationConfig{}) == TessellationLevel::Normal,
          "default controls did not derive Normal");
    return ok;
}

bool unit_tessellation_level_cli() {
    using heaven::TessellationLevel;
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::printf("[unit_tessellation_level_cli] FAILED: %s\n", message);
            ok = false;
        }
    };
    const std::pair<const char*, TessellationLevel> cases[] = {
        {"disabled", TessellationLevel::Disabled}, {"moderate", TessellationLevel::Moderate},
        {"normal", TessellationLevel::Normal}, {"extreme", TessellationLevel::Extreme}};
    for (const auto& [name, expected] : cases) {
        const auto result = parse({"--tess-level", name});
        check(result.diagnostics.empty() &&
                  heaven::detect_tessellation_level(result.config.tessellation) == expected,
              "selectable CLI level did not apply without diagnostics");
        check(result.seen_keys == std::set<std::string>{"tessellation.enabled", "tessellation.scale",
                  "tessellation.factor", "tessellation.distance"},
              "grouped CLI setting did not record the existing persistent controls");
    }
    const auto override_after = parse({"--preset", "gpu-torture", "--tess-level", "moderate"});
    check(override_after.config.render_preset == heaven::RenderPreset::GpuTorture &&
              override_after.config.quality.msaa_samples == 4 &&
              override_after.config.tessellation.scale == 0.75f &&
              heaven::detect_tessellation_level(override_after.config.tessellation) == TessellationLevel::Moderate,
          "later level did not override only the rendering preset's geometry controls");
    const auto preset_after = parse({"--tess-level", "moderate", "--preset", "gpu-torture"});
    check(heaven::detect_tessellation_level(preset_after.config.tessellation) == TessellationLevel::Extreme,
          "later rendering preset did not retain its existing geometry base semantics");
    const auto manual_after = parse({"--tess-level", "normal", "--tess-scale", "0.9"});
    check(manual_after.config.tessellation.scale == 0.9f &&
              manual_after.config.tessellation.factor == 1.0f &&
              heaven::detect_tessellation_level(manual_after.config.tessellation) == TessellationLevel::Custom,
          "manual override did not retain exact values and derive Custom");
    const auto level_after = parse({"--tess-scale", "0.9", "--tess-level", "normal"});
    check(heaven::detect_tessellation_level(level_after.config.tessellation) == TessellationLevel::Normal,
          "later level did not supersede a prior manual override");
    const auto disabled = parse({"--tess-scale", "0.9", "--tess-factor", "1.125",
                                "--tess-distance", "2.25", "--tess-level", "disabled"});
    check(!disabled.config.tessellation.enabled && disabled.config.tessellation.scale == 0.9f &&
              disabled.config.tessellation.factor == 1.125f && disabled.config.tessellation.distance == 2.25f,
          "CLI Disabled changed manual controls");
    const auto invalid = parse({"--tess-scale", "0.9", "--tess-level", "custom"});
    check(invalid.diagnostics.size() == 1 && invalid.config.tessellation.scale == 0.9f &&
              invalid.config.tessellation.enabled,
          "invalid or derived-only CLI level altered the current settings");
    const auto missing = parse({"--tess-level"});
    check(missing.diagnostics.size() == 1 &&
              heaven::detect_tessellation_level(missing.config.tessellation) == TessellationLevel::Normal,
          "missing CLI value changed the defaults");

    heaven::BenchConfig saved;
    saved.tessellation = {false, 0.91f, 1.125f, 2.25f, "saved-cache.zip"};
    const auto saved_controls = saved.tessellation;
    heaven::apply_cli_overlay(saved, parse({"--tess-level", "disabled"}));
    check(same_controls(saved.tessellation, saved_controls),
          "CLI replay replaced the file's custom controls with parser defaults");
    heaven::apply_cli_overlay(saved, parse({"--tess-level", "moderate", "--tess-distance", "2.25"}));
    check(saved.tessellation.enabled && saved.tessellation.scale == 0.75f &&
              saved.tessellation.factor == 0.75f && saved.tessellation.distance == 2.25f &&
              saved.tessellation.cache_file == "saved-cache.zip" &&
              heaven::detect_tessellation_level(saved.tessellation) == TessellationLevel::Custom,
          "ordered CLI replay did not preserve manual overlays or file cache storage");
    check(heaven::is_dirty(saved, heaven::BenchConfig{}),
          "named level changes did not use existing pending-reload settings");
    return ok;
}

bool unit_tessellation_level_custom_roundtrip() {
    heaven::BenchConfig config;
    config.tessellation = {false, 0.91f, 1.125f, 2.25f, "saved-cache.zip"};
    const auto path = std::filesystem::temp_directory_path() /
        ("heaven-tessellation-levels-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    const auto diagnostics = heaven::write_config_file(path.string(), config);
    const auto loaded = heaven::load_config_file(path.string(), heaven::BenchConfig{});
    std::error_code error;
    std::filesystem::remove(path, error);
    if (!diagnostics.empty() || !loaded.parsed_ok || !loaded.diagnostics.empty() ||
            !same_controls(config.tessellation, loaded.config.tessellation)) {
        std::printf("[unit_tessellation_level_custom_roundtrip] FAILED: saved custom controls changed\n");
        return false;
    }
    auto enabled = loaded.config.tessellation;
    enabled.enabled = true;
    return heaven::detect_tessellation_level(enabled) == heaven::TessellationLevel::Custom;
}

const heaven::testing::Register levels(unit_tessellation_levels);
const heaven::testing::Register cli(unit_tessellation_level_cli);
const heaven::testing::Register saved(unit_tessellation_level_custom_roundtrip);

} // namespace
