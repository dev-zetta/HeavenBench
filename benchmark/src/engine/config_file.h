/*
 * HeavenBench — JSON configuration file (public surface)
 *
 * Loads a run from a saved JSON file (`--config FILE`) and serializes the
 * Effective_Configuration back out (`--write-config FILE`), with the precedence
 * CLI > file > default. The module mirrors engine/cli_parser: it is GPU-free
 * (no Filament/SDL/GPU header or symbol), host-testable, and never prints — it
 * collects every unknown / malformed / clamped / rejected / missing condition
 * into a returned diagnostics vector that main.cpp routes to stderr through the
 * same path it already uses for CLI diagnostics.
 *
 * The Config_File schema is derived from the SAME BenchConfig field set the
 * flag table writes, expressed once as a config_schema() table of typed field
 * descriptors so validation parity  and key coverage
 * are structural, not duplicated logic.
 *
 * This header depends only on engine/cli_parser.h (BenchConfig, Diagnostics),
 * engine/types.h, and the C++ standard library. It does NOT include the
 * Config_Library (lib/nlohmann/json.hpp); that header is visible only inside
 * config_file.cpp. The JsonValue / JsonObject wrappers below are opaque,
 * forward-declared types completed in the.cpp, so json.hpp never leaks into
 * the public surface.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_CONFIG_FILE_H_
#define HEAVENBENCH_SRC_ENGINE_CONFIG_FILE_H_

#include "engine/cli_parser.h"
#include "engine/types.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace heaven {

// Opaque wrappers over the Config_Library's value/object types. They are only
// forward-declared here (and used by reference in the FieldSpec callbacks), so
// the vendored json.hpp stays confined to config_file.cpp. The definitions live
// in config_file.cpp where the Config_Library is visible.
struct JsonValue;
struct JsonObject;

// FieldType — the JSON value category a Recognized_Key expects. Drives the
// type-mismatch check. QualityLevelEnum is represented in
// JSON as its integer level (0/1/2), matching the CLI's integer-level input so
// clamp_quality_level applies identically.
enum class FieldType {
    Bool,
    Int,
    Float,
    String,
    QualityLevelEnum,
};

// FieldSpec — one mapping between a Config_File key and a BenchConfig field.

//   load(json_value, cfg, diag):
//     reads the JSON value, applies the SAME shared clamp/reject validator the
//     CLI uses, writes the field, and may push a clamp/reject diagnostic

//   store(cfg, out):
//     writes the field's current value under `key` into the output JSON object
//     for the writer.

// Both callbacks are pure with respect to console/stream I/O.
struct FieldSpec {
    std::string_view key;   // canonical dotted Config_File key, e.g. "features.grass"
    FieldType        type;  // expected JSON type (mismatch detection)
    std::function<void(const JsonValue&, BenchConfig&, Diagnostics&)> load;
    std::function<void(const BenchConfig&, JsonObject&)>              store;
};

// ConfigLoadResult — the full output of loading a Config_File. A plain value
// struct holding no GPU state.
struct ConfigLoadResult {
    BenchConfig              config;             // base with file values applied
    std::vector<std::string> diagnostics;        // collected, never printed
    bool                     parsed_ok = false;  // parse success vs failure
};

// The single source of truth bridging JSON keys to BenchConfig fields. Defined
// once in config_file.cpp where the Config_Library is visible; exposed for the
// loader, the writer, the CLI overlay, and the host tests.
[[nodiscard]] const std::vector<FieldSpec>& config_schema();

// Load Config_File `path` onto a copy of `base`. Reads the file, parses it with
// the Config_Library, maps Recognized_Keys via config_schema() with full
// Requirement-5 validation, and collects diagnostics. Never prints. A file that
// cannot be opened  or does not parse  yields parsed_ok=false and
// config == base (no field mutated).
[[nodiscard]] ConfigLoadResult
load_config_file(const std::string& path, const BenchConfig& base);

// Serialize `cfg` to `path` as JSON, emitting one key for every Setting in
// config_schema(). Returns diagnostics (empty on success). On
// open/write failure, returns a single diagnostic naming the path and the
// failure and performs no other side effect.
[[nodiscard]] std::vector<std::string>
write_config_file(const std::string& path, const BenchConfig& cfg);

// The same effective configuration used by file exports, without file I/O.
[[nodiscard]] std::string serialize_config(const BenchConfig& cfg);

// Re-apply the command-line flags that were actually present on the command
// line on top of `cfg` (typically a config already carrying file/default
// values), so the command line wins where it overlaps the file: CLI > file >
// default. Replay `parse_result.config_operations` through the registered
// handlers in command-line order, including nonpersistent settings and presets.
// Absent flags and values rejected by their handler retain the file/default
// state. Startup actions are not replayed. GPU-free and never prints; duplicate
// validation diagnostics are discarded because parse_cli already returned them.
void apply_cli_overlay(BenchConfig& cfg, const ParseResult& parse_result);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_CONFIG_FILE_H_
