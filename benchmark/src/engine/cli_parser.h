/*
 * HeavenBench — CLI argument parser (public surface)
 *
 * A type-driven, table-backed replacement for the legacy if/else strcmp chain
 * in main.cpp::parse_args(). Every flag is described once in a single ordered
 * registration table (flag_table()) of FlagSpec entries; that table is the
 * single source of truth used for both argument parsing (via a hash-based
 * lookup keyed by flag name) and --help generation.
 *
 * The module is GPU-free and I/O-free: it depends only on engine/types.h and
 * the C++ standard library, performs no printing and no exit, and returns its
 * full result in a ParseResult struct. This keeps it host-testable without
 * Filament or SDL.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_CLI_PARSER_H_
#define HEAVENBENCH_SRC_ENGINE_CLI_PARSER_H_

#include "engine/number_parse.h"
#include "engine/types.h"

#include <array>
#include <functional>
#include <expected>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heaven {

// BenchConfig — the configuration the parser produces. AppConfig extended with
// the two BenchConfig-only fields, relocated here from main.cpp so it is
// constructible without Filament/SDL.
struct BenchConfig : AppConfig {
    GraphicsApi graphics_api   = GraphicsApi::Filament;
    bool        benchmark_mode = false;
};

// Arity — whether a flag consumes a following value token.
enum class Arity { None, Value };

// Diagnostics — collected, never printed by the parser.
// The wrapper in main.cpp decides how/whether to print them, keeping the module
// host-testable and I/O-free.
struct Diagnostics {
    std::vector<std::string> messages;
    void warn(std::string msg) { messages.push_back(std::move(msg)); }
};

// FlagSpec — one registration-table entry: the single place a flag's name,
// value arity, help text, and applied action are defined.

//   apply(cfg, value, diag):
//     • Boolean_Toggle / Alias member: `value` is empty; the handler sets its
//       declared target field(s) to a fixed value.
//     • Value_Flag: `value` is the consumed following token; the handler parses
//       it (reusing the shared pure helpers for weather/quality) and may push a
//       clamp/reject/malformed diagnostic.
struct FlagSpec {
    std::string_view name;        // e.g. "--width", "--no-grass"
    Arity            arity;       // None = toggle, Value = consumes one token
    std::string_view value_label; // "W", "DIR", "L", … ("" for toggles) — help only
    std::string_view help;        // help text
    std::function<void(BenchConfig&, std::string_view value, Diagnostics&)> apply;
};

// Transient visual-audit actions. They never become saved configuration or
// benchmark settings. Captures default to a fixed 1/60-second simulation step.
struct VisualCaptureOptions {
    std::optional<std::array<float, 5>> camera_pose;
    std::optional<float> tour_time;
    bool tour_motion = false;
    float fov = 60.0f;
    bool fov_explicit = false;
    std::optional<std::string> output;
    int frames = 90;
    int count = 1;
    bool floating_motion = false;
    float fps = 60.0f;
    bool realtime = false;
    std::optional<std::string> profile_output;
    float profile_seconds = 65.0f;
    bool invalid = false;
};

// A resolved renderer-setting flag in command-line order. Own both tokens:
// callers may release their argv storage before loading the configuration file.
// Startup actions are excluded. The registered handler retains its normal
// validation/clamping behavior when replayed against the loaded configuration.
struct CliConfigOperation {
    std::string flag;
    std::string value;
};

// Diagnostic trace controls are transient startup actions, never saved settings.
struct MicroprofileOptions {
    std::optional<std::string> output_base;
    float seconds = 10.0f;
    bool visible = false;
    bool invalid = false;
};

// ParseResult — the full output of parsing.
struct ParseResult {
    BenchConfig              config;          // the parsed configuration
    std::vector<std::string> diagnostics;     // unknown/malformed/clamp/reject reports
    bool                     help_requested = false;
    bool                     list_test_scenes_requested = false;
    bool                     test_scene_invalid = false;
    VisualCaptureOptions     capture;
    MicroprofileOptions      microprofile;

    // Configuration-file orchestration outputs (empty when the flag was
    // absent). Stored on the result rather than in BenchConfig because they are
    // startup actions, not renderer settings. Last occurrence wins for
    // --config; see the parse driver's special-casing.
    std::optional<std::string> config_path;        // last --config value
    std::optional<std::string> write_config_path;  // last --write-config value

    // Used-asset packer orchestration outputs. Like config_path above, these
    // target a startup ACTION (run the packer instead of the renderer) rather
    // than a renderer setting, so they live on the result, not BenchConfig.
    //   • pack_requested is set whenever the --pack-assets TOKEN appears,
    //     independent of whether its value was consumed, so "flag present" is
    //     distinguishable from "flag absent". --pack-assets
    //     takes an OPTIONAL value: when it is the final token, or the next
    //     token begins with '-', pack_target stays unset and NO diagnostic is
    //     emitted — main then defaults the Pack_Target to "assets".
    //   • pack_target is the Pack_Target directory; unset
    //     means "use the default", applied by main as value_or("assets").
    //   • pack_move opts into Move_Mode; default is Copy_Mode.
    //   • pack_overwrite allows writing into a non-empty existing Pack_Target

    bool                       pack_requested = false;  // --pack-assets token seen
    std::optional<std::string> pack_target;             // --pack-assets value
    bool                       pack_move      = false;   // --pack-move
    bool                       pack_overwrite = false;   // --pack-overwrite
    std::optional<std::string> pack_texture_set;        // --pack-texture-set

    bool build_tessellation_cache_requested = false;
    std::optional<std::string> tessellation_cache_target;
    bool benchmark_loading_requested = false;
    std::optional<std::string> loading_benchmark_output;

    // Ordered operations drive CLI-over-file precedence, including CLI-only
    // settings that deliberately have no persistent JSON schema entry.
    std::vector<CliConfigOperation> config_operations;

    // The set of persistent flags actually seen on the command line,
    // recorded by their canonical Config_File schema key (e.g. --no-grass →
    // "features.grass", --width → "width"). Retained for schema introspection;
    // it is not the complete set of renderer settings accepted by the CLI.
    std::set<std::string> seen_keys;
};

// The single source of truth. Defined once in cli_parser.cpp; exposed for help
// generation and for the host tests.
[[nodiscard]] const std::vector<FlagSpec>& flag_table();

// Parse a command line into a BenchConfig + diagnostics + help flag. Pure: no
// printing, no exit, no rendering init.
[[nodiscard]] ParseResult parse_cli(std::span<const std::string_view> args);

template <std::ranges::input_range Arguments>
requires std::convertible_to<std::ranges::range_reference_t<Arguments>,
                             std::string_view>
[[nodiscard]] ParseResult parse_cli(const Arguments& arguments) {
    std::vector<std::string_view> views;
    if constexpr (std::ranges::sized_range<Arguments>) {
        views.reserve(std::ranges::size(arguments));
    }
    for (const auto& argument : arguments) {
        views.emplace_back(argument);
    }
    return parse_cli(std::span<const std::string_view>{views});
}

// Apply only the explicitly present CLI settings to an existing configuration.
// This is the canonical compatibility path for tests, smoke checks, and callers
// that need CLI-over-config precedence without duplicating flag parsing.
void apply_cli_arguments(AppConfig& config,
                         std::span<const std::string_view> args);

template <std::ranges::input_range Arguments>
requires std::convertible_to<std::ranges::range_reference_t<Arguments>,
                             std::string_view>
void apply_cli_arguments(AppConfig& config, const Arguments& arguments) {
    std::vector<std::string_view> views;
    if constexpr (std::ranges::sized_range<Arguments>) {
        views.reserve(std::ranges::size(arguments));
    }
    for (const auto& argument : arguments) {
        views.emplace_back(argument);
    }
    apply_cli_arguments(config, std::span<const std::string_view>{views});
}

// Generate the help listing entirely from flag_table(). One
// line per table entry, in table order; no entry exists for any flag absent
// from the table.
[[nodiscard]] std::string generate_help();

// ── Pure parsing primitives (host-testable) ─────────────────────

// Strict numeric parsers for the non-weather/non-quality value flags. Unlike
// std::atoi/std::atof (which silently yield 0 on junk), these require the whole
// token to be a valid number — they return nullopt on empty input, on a token
// std::strtol/std::strtod does not advance past, or on trailing garbage. This
// mirrors parse_rain_intensity's end-pointer validation style and lets the
// value handlers report-and-leave-unchanged on malformed input (Requirement

[[nodiscard]] inline std::expected<int, NumberParseError>
parse_int_strict(std::string_view token) {
    return parse_number_strict<int>(token);
}
[[nodiscard]] inline std::expected<float, NumberParseError>
parse_float_strict(std::string_view token) {
    return parse_number_strict<float>(token);
}

// Authored Unigine text historically used atoi/atof-style prefix parsing.
// These helpers retain that explicit compatibility contract while returning
// an error instead of silently manufacturing zero for malformed input.
[[nodiscard]] inline std::expected<int, NumberParseError>
parse_int_prefix(std::string_view token) {
    return parse_number_prefix<int>(token);
}
[[nodiscard]] inline std::expected<float, NumberParseError>
parse_float_prefix(std::string_view token) {
    return parse_number_prefix<float>(token);
}

// Normalize an hour value into [0.0, 24.0) by adding/subtracting 24.0 as
// needed, matching the legacy --time-of-day wrap loop exactly (Requirement

[[nodiscard]] float wrap_24(float t);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_CLI_PARSER_H_
