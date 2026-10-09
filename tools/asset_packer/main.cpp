/* Standalone, GPU-free entry point for the renderer's used-asset packer. */
#include "asset_packer/asset_packer.h"
#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/runtime_paths.h"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {
void print_diagnostics(const std::vector<std::string>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::fprintf(stderr, "%s\n", diagnostic.c_str());
    }
}
}

int main(int argc, char* argv[]) try {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i]);
    const heaven::ParseResult parsed = heaven::parse_cli(arguments);
    if (parsed.help_requested) {
        std::puts(
            "Usage: heaven_asset_packer --data PATH [options]\n"
            "\n"
            "  --pack-assets [PATH]    Output ZIP (default: assets/assets.zip)\n"
            "  --pack-overwrite        Replace an existing output archive\n"
            "  --pack-move             Remove sources after successful packing\n"
            "  --pack-texture-set FILE Package a generated high-resolution texture set\n"
            "  --config FILE          Read renderer settings before CLI overrides\n"
            "  --write-config FILE    Save resolved settings and exit without packing\n"
            "  --help                 Show this help\n"
            "\n"
            "Scene and feature flags match heaven_benchmark. No display is required.");
        return 0;
    }
    print_diagnostics(parsed.diagnostics);
    if (!parsed.diagnostics.empty()) return heaven::EXIT_USAGE;
    if (parsed.build_tessellation_cache_requested || parsed.benchmark_loading_requested) {
        std::fputs("This tool packs assets; use heaven_benchmark for benchmark actions.\n", stderr);
        return heaven::EXIT_USAGE;
    }

    heaven::BenchConfig config;
    if (const auto data = heaven::runtime_path_from_environment("HEAVEN_DEFAULT_DATA")) {
        config.data_dir = data->string();
    }
    if (parsed.config_path) {
        const auto loaded = heaven::load_config_file(*parsed.config_path, config);
        print_diagnostics(loaded.diagnostics);
        if (!loaded.parsed_ok || !loaded.diagnostics.empty()) return heaven::EXIT_USAGE;
        config = loaded.config;
    }
    heaven::apply_cli_overlay(config, parsed);
    if (parsed.write_config_path) {
        const auto diagnostics = heaven::write_config_file(*parsed.write_config_path, config);
        print_diagnostics(diagnostics);
        return diagnostics.empty() ? 0 : 1;
    }

    const heaven::PackOptions options{
        parsed.pack_target.value_or("assets/assets.zip"),
        parsed.pack_move,
        parsed.pack_overwrite,
        parsed.pack_texture_set};
    return heaven::run_asset_packer(config, options);
} catch (const std::exception& error) {
    std::fprintf(stderr, "Asset packing failed: %s\n", error.what());
    return heaven::EXIT_WRITE;
} catch (...) {
    std::fputs("Asset packing failed with an unknown exception\n", stderr);
    return heaven::EXIT_WRITE;
}
