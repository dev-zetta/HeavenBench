/*
 * HeavenBench — rendering preset examples
 */
#include "engine/cli_parser.h"
#include "engine/render_presets.h"

#include <cstdio>
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

heaven::ParseResult parse(std::initializer_list<const char*> args) {
    std::vector<const char*> argv{"heaven_benchmark"};
    argv.insert(argv.end(), args.begin(), args.end());
    return heaven::parse_cli(argv);
}

bool unit_render_presets() {
    bool ok = true;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::printf("[unit_render_presets] FAILED: %s\n", message);
            ok = false;
        }
    };

    check(heaven::parse_render_preset("visual-ultra") ==
              heaven::RenderPreset::VisualUltra,
          "canonical visual-ultra ID did not parse");
    check(heaven::parse_render_preset("Balanced System") ==
              heaven::RenderPreset::BalancedSystem,
          "legacy balanced label did not parse");
    check(!heaven::parse_render_preset("cinematic"),
          "unknown preset unexpectedly parsed");
    const auto legacy = parse({"--quality-preset", "Visual Ultra"});
    check(legacy.config.render_preset == heaven::RenderPreset::VisualUltra &&
              heaven::detect_render_preset(legacy.config) ==
                  heaven::RenderPreset::VisualUltra,
          "deprecated --quality-preset alias did not apply the preset");

    heaven::AppConfig config;
    config.width = 3440;
    config.weather.rain_intensity = 0.25f;
    heaven::apply_render_preset(config, heaven::RenderPreset::GpuTorture);
    check(config.width == 3440 && config.weather.rain_intensity == 0.25f,
          "preset changed display or weather state");
    check(config.quality.msaa_samples == 4 &&
              config.quality.point_shadow_map_size == 1024 &&
              config.quality.shadow_light_budget ==
                  config.quality.shadow_light_budget_max,
          "GPU Torture did not apply its owned settings atomically");
    check(heaven::detect_render_preset(config) ==
              heaven::RenderPreset::GpuTorture,
          "applied GPU Torture preset was not detected");
    config.width = 2560;
    check(heaven::detect_render_preset(config) ==
              heaven::RenderPreset::GpuTorture,
          "unrelated display edit changed detected preset");
    config.quality.msaa_samples = 2;
    check(heaven::detect_render_preset(config) == heaven::RenderPreset::Custom,
          "owned setting edit did not produce Custom");

    const auto override_after =
        parse({"--preset", "gpu-torture", "--msaa-quality", "2"});
    check(override_after.config.quality.msaa_samples == 2 &&
              heaven::detect_render_preset(override_after.config) ==
                  heaven::RenderPreset::Custom,
          "later CLI override did not customize preset");

    const auto preset_after =
        parse({"--msaa-quality", "2", "--preset", "gpu-torture"});
    check(preset_after.config.quality.msaa_samples == 4 &&
              preset_after.config.render_preset ==
                  heaven::RenderPreset::GpuTorture,
          "later --preset did not reset all preset-owned settings");

    const auto cpu =
        parse({"--preset", "cpu-torture", "--no-cpu-stress"});
    check(!cpu.config.cpu_stress.enabled &&
              cpu.config.render_preset == heaven::RenderPreset::CpuTorture &&
              heaven::detect_render_preset(cpu.config) ==
                  heaven::RenderPreset::Custom,
          "CPU stress override did not customize CPU Torture");

    for (const auto preset : {heaven::RenderPreset::BalancedSystem,
                              heaven::RenderPreset::VisualUltra,
                              heaven::RenderPreset::GpuTorture,
                              heaven::RenderPreset::CpuTorture}) {
        heaven::AppConfig cached;
        cached.tessellation.cache_file = "custom/tessellation.cache";
        heaven::apply_render_preset(cached, preset);
        check(cached.tessellation.cache_file == "custom/tessellation.cache",
              "preset discarded the tessellation cache path");
    }

    return ok;
}

const heaven::testing::Register reg(unit_render_presets);

} // namespace
