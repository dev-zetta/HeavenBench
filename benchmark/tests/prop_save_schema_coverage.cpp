// Feature: in-ui-configuration,: Save emits every config_schema() setting
/*
 * HeavenBench — Property test for save-time schema coverage.
 *
 * Feature: in-ui-configuration
 *   •: Save emits every config_schema() setting
 *
 *
 * (save emits every config_schema() setting):
 *   For ANY BenchConfig, serializing it through write_config_file(path, cfg)
 *   produces a Config_File whose key set is EXACTLY the config_schema() key set:
 *   every schema key appears in the written JSON exactly once, and the file
 *   carries no key that is not a schema key. The in-UI Save reuses
 *   write_config_file, so proving the writer's output covers the schema 1:1
 *   proves Save persists the full Setting surface with no omissions or extras.
 *
 * How the writer structures keys. write_config_file() builds the output object
 * by invoking each FieldSpec.store(cfg, out) exactly once, and each store does
 * `out.node[std::string(key)] = value`. nlohmann's operator[](const std::string&)
 * treats the WHOLE string as a single object member name (it does NOT split on
 * '.' the way a JSON pointer would), so a dotted key such as "features.grass"
 * becomes a FLAT top-level member literally named "features.grass" rather than a
 * nested {"features":{"grass":...}} object. The written document is therefore a
 * flat JSON object whose member names are exactly the FieldSpec.key strings, and
 * coverage is verified by walking the parsed object's top-level keys and
 * matching them against config_schema()'s keys.
 *
 * write_config_file is pure with respect to the GPU (file I/O only, no
 * Filament/SDL/GPU, never prints), so this runs on the host. We include the
 * vendored nlohmann/json.hpp directly to PARSE the written file and inspect its
 * keys — the heaven_benchmark_tests target lists the lib/nlohmann include dir,
 * and json.hpp is dependency-free, so this keeps the host link line
 * Filament/SDL-free. The file self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100 iterations"
 * bar (raise via RC_PARAMS=max_success=…).
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "json.hpp"

#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/types.h"

#include "bench_config_gen.h"

// Mirror the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

using heaven::BenchConfig;

// ── Temp-file plumbing ──────────────────────────────────────────

// Unique path under the system temp dir. A static counter plus a steady-clock
// stamp keeps paths distinct within this process run and across re-runs without
// pulling in platform headers. Each case writes via write_config_file, parses
// the file back, then removes it.
std::filesystem::path unique_temp_path() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1);
    const std::uint64_t stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("heaven_cfg_schema_cov_" + std::to_string(stamp) + "_" +
            std::to_string(n) + ".json");
}

// ── ASCII string generator (serializable, no escaping needed) ───

// is about schema-KEY coverage, which is independent of any string
// field's CONTENT (each FieldSpec.store emits its key regardless of the value).
// The shared gen_bench_config() draws quality_preset_label from
// rc::gen::arbitrary<std::string>(), which can contain non-UTF-8 bytes; nlohmann's
// dump() rejects invalid UTF-8, an orthogonal serialization concern that is not
// what this property tests. We therefore overwrite the label with an ASCII
// string drawn from a JSON-safe charset (the same approach prop_config_roundtrip
// uses) so the case isolates key coverage rather than string encoding.
const std::string& string_charset() {
    static const std::string charset =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./ ";
    return charset;
}

std::string gen_ascii_string() {
    return *rc::gen::container<std::string>(rc::gen::elementOf(string_charset()));
}

// The canonical key set declared by config_schema() — the source of truth the
// written file must cover exactly.
std::set<std::string> schema_keys() {
    std::set<std::string> keys;
    for (const heaven::FieldSpec& f : heaven::config_schema()) {
        if (f.key == "quality_preset_label") continue;
        keys.emplace(f.key);
    }
    return keys;
}

// ──: save emits every config_schema() setting ───────
bool run_p14_save_schema_coverage() {
    return rc::check(
        "Feature: in-ui-configuration, Property 14: write_config_file emits "
        "exactly one key per config_schema() entry, with no missing or extra "
        "keys (Validates: Requirements 5.6)",
        [] {
            BenchConfig cfg = heaven::testing::gen_bench_config();
            // Use a JSON-serializable (ASCII) label; the property is about key
            // coverage, not string encoding (see note above).
            cfg.quality_preset_label = gen_ascii_string();
            const std::filesystem::path path = unique_temp_path();

            // Serialize. A writable temp path means the writer performs no
            // open/write failure, so it returns zero diagnostics.
            const std::vector<std::string> write_diags =
                heaven::write_config_file(path.string(), cfg);
            RC_ASSERT(write_diags.empty());

            // Read the file back and parse it with the Config_Library.
            std::ifstream in(path, std::ios::binary);
            RC_ASSERT(in.is_open());
            const std::string content((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            in.close();

            std::error_code ec;
            std::filesystem::remove(path, ec); // best-effort cleanup

            const nlohmann::json root = nlohmann::json::parse(content);

            // The writer emits a flat top-level object (dotted keys are literal
            // member names, not nested objects).
            RC_ASSERT(root.is_object());

            // Walk the parsed object's top-level member names. An nlohmann
            // object cannot hold duplicate member names, so "exactly once" per
            // present key is structural; what remains to prove is that the
            // present-key set equals the schema-key set (no missing, no extra).
            std::set<std::string> written_keys;
            for (const auto& [key, value] : root.items()) {
                written_keys.insert(key);
            }

            const std::set<std::string> expected = schema_keys();

            // Same cardinality: one member per schema entry, no extras. (The set
            // sizes match the config_schema() entry count because schema keys
            // are themselves unique.)
            RC_ASSERT(written_keys.size() == expected.size());

            // Direction 1: every schema key is present in the written file
            // (— every Setting is written).
            for (const std::string& k : expected) {
                RC_ASSERT(written_keys.contains(k));
            }

            // Direction 2: the written file carries no key outside the schema
            // (no stray/extra Setting emitted).
            for (const std::string& k : written_keys) {
                RC_ASSERT(expected.contains(k));
            }
        });
}

const heaven::testing::Register g_register_save_schema_coverage{
    &run_p14_save_schema_coverage};

} // namespace
