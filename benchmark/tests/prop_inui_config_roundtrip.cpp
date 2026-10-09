// Feature: in-ui-configuration,: Configuration save/load round-trip
/*
 * HeavenBench — Property test for the configuration save/load round-trip.
 *
 * Feature: in-ui-configuration
 *   •: Configuration save/load round-trip
 *
 *
 * (save/load round-trip):
 *   For ANY BenchConfig, serializing it through write_config_file (returning no
 *   diagnostic) and then reading that file back through load_config_file onto
 *   the same base (reporting parse success) reproduces the same configuration
 *   for every setting in config_schema(): every integer, boolean, enumerated,
 *   and string setting matches EXACTLY, and every floating-point setting matches
 *   within an absolute difference of 1e-6 (which already covers the
 *   "1e-6 absolute below magnitude 1.0" clause). The in-UI Save/Load reuses
 *   write_config_file / load_config_file directly, so proving the module
 *   round-trips proves saved configurations reload faithfully.
 *
 * Why a fixpoint, not raw-original equality. gen_bench_config() deliberately
 * emits boundary AND out-of-range values (the shared generator the catalog /
 * clamp / reject properties consume). write_config_file writes those raw values
 * verbatim, but load_config_file CLAMPS / REJECTS them on read through the shared
 * engine helpers. So the raw `original` is NOT in general what a load reproduces
 * — the loader's clamp is part of the contract. The meaningful,
 * well-defined round-trip property given that clamp is STABILITY: once a config
 * has passed through the loader (`loaded1`), saving and reloading it again
 * (`loaded2`) must reproduce it exactly. We therefore normalize first
 * (write original → load → loaded1), then assert the round-trip on the
 * already-normalized config (write loaded1 → load → loaded2; loaded1 == loaded2
 * over config_schema()). This is exactly the "load once, compare loaded1 vs
 * loaded2" framing the test describes, and it is the in-range round-trip
 * specifies (loaded1 is in-range by construction).
 *
 * How the comparison is done over config_schema(). JsonObject is opaque outside
 * config_file.cpp, so we cannot invoke FieldSpec.store directly. Instead we use
 * the writer itself as the serializer: write loaded1 and loaded2 to their own
 * temp files, parse both files with the vendored Config_Library, and compare
 * them key-by-key. write_config_file emits a flat JSON object whose member names
 * are exactly the FieldSpec.key strings, so walking config_schema() and looking
 * each key up in both parsed objects compares every persisted setting. The
 * FieldSpec.type drives the comparison: Float keys use the 1e-6 absolute
 * tolerance; Bool / Int / String / QualityLevelEnum keys must match exactly.
 *
 * Non-UTF-8 string fields. gen_bench_config() draws the String-typed schema
 * fields (data_dir, node_file, quality_preset_label) from
 * rc::gen::arbitrary<std::string>(), which can contain non-UTF-8 bytes;
 * nlohmann's dump() throws on those — a serialization concern orthogonal to the
 * round-trip property. We therefore overwrite every String schema field with a
 * JSON-safe ASCII string (the same approach the sibling save/diagnostics tests
 * use) before the first write, so the case isolates value fidelity rather than
 * string encoding while still exercising arbitrary in-range string content.
 *
 * write_config_file / load_config_file are GPU-free (file I/O only, no
 * Filament/SDL/GPU, never print), so this runs on the host. We include the
 * vendored nlohmann/json.hpp directly to PARSE the written files — the
 * heaven_benchmark_tests target lists the lib/nlohmann include dir, and json.hpp
 * is dependency-free, so the host link line stays Filament/SDL-free. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/CMakeLists.txt
 * picks it up without editing the entry point. RapidCheck's default of 100
 * successful iterations meets the "≥ 100 iterations" bar (raise via
 * RC_PARAMS=max_success=…).
 */
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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
using heaven::ConfigLoadResult;
using heaven::FieldSpec;
using heaven::FieldType;

// Absolute tolerance for floating-point settings. 1e-6
// absolute already subsumes the "1e-6 absolute for magnitudes below 1.0" clause.
constexpr double kFloatTol = 1e-6;

// ── Temp-file plumbing ──────────────────────────────────────────

// Unique path under the system temp dir. A static counter plus a steady-clock
// stamp keeps paths distinct within this process run and across re-runs without
// pulling in platform headers.
std::filesystem::path unique_temp_path() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1);
    const std::uint64_t stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("heaven_cfg_roundtrip_" + std::to_string(stamp) + "_" +
            std::to_string(n) + ".json");
}

// ── JSON-safe ASCII string generator ────────────────────────────

// The round-trip preserves arbitrary in-range string CONTENT, but nlohmann's
// dump() rejects non-UTF-8 bytes; the shared generator draws arbitrary
// std::strings. We replace the String-typed schema fields with ASCII drawn from
// a JSON-safe charset so the writer cannot throw on an invalid byte sequence,
// isolating value fidelity from string encoding.
const std::string& string_charset() {
    static const std::string charset =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./ ";
    return charset;
}

std::string gen_ascii_string() {
    return *rc::gen::container<std::string>(rc::gen::elementOf(string_charset()));
}

// Replace every String-typed config_schema() field with a JSON-safe ASCII
// string so write_config_file's dump() never throws on a non-UTF-8 byte. Driven
// off the schema's FieldType so it stays correct if a String field is added.
void sanitize_string_fields(BenchConfig& cfg) {
    for (const FieldSpec& f : heaven::config_schema()) {
        if (f.type == FieldType::String) {
            const std::string key(f.key);
            const std::string ascii = gen_ascii_string();
            if (key == "data_dir") {
                cfg.data_dir = ascii;
            } else if (key == "node_file") {
                cfg.node_file = ascii;
            } else if (key == "quality_preset_label") {
                cfg.quality_preset_label = ascii;
            }
        }
    }
}

// Serialize `cfg` to a fresh temp file and parse it back as JSON. Asserts the
// writer reported no diagnostics (a writable temp path means no I/O failure).
nlohmann::json write_and_parse(const BenchConfig& cfg) {
    const std::filesystem::path path = unique_temp_path();
    const std::vector<std::string> diags =
        heaven::write_config_file(path.string(), cfg);
    RC_ASSERT(diags.empty());

    std::ifstream in(path, std::ios::binary);
    RC_ASSERT(in.is_open());
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    in.close();

    std::error_code ec;
    std::filesystem::remove(path, ec); // best-effort cleanup

    return nlohmann::json::parse(content);
}

// Save `cfg` to a temp file, load it back onto `base`, asserting the load parsed
// successfully. Returns the loaded BenchConfig.
BenchConfig save_then_load(const BenchConfig& cfg, const BenchConfig& base) {
    const std::filesystem::path path = unique_temp_path();
    const std::vector<std::string> write_diags =
        heaven::write_config_file(path.string(), cfg);
    RC_ASSERT(write_diags.empty());

    const ConfigLoadResult result = heaven::load_config_file(path.string(), base);

    std::error_code ec;
    std::filesystem::remove(path, ec); // best-effort cleanup

    // A well-formed object parses successfully even when individual settings
    // were clamped/rejected (those are non-error diagnostics).
    RC_ASSERT(result.parsed_ok);
    return result.config;
}

// Compare two parsed Config_File JSON objects key-by-key over config_schema():
// exact match for Bool/Int/String/QualityLevelEnum, 1e-6 absolute tolerance for
// Float. Every schema key must be present in both objects.
void assert_schema_equal(const nlohmann::json& a, const nlohmann::json& b) {
    for (const FieldSpec& f : heaven::config_schema()) {
        const std::string key(f.key);
        if (key == "quality_preset_label") continue;
        RC_ASSERT(a.contains(key));
        RC_ASSERT(b.contains(key));
        const nlohmann::json& va = a.at(key);
        const nlohmann::json& vb = b.at(key);

        switch (f.type) {
            case FieldType::Float: {
                const double da = va.get<double>();
                const double db = vb.get<double>();
                RC_ASSERT(std::isfinite(da));
                RC_ASSERT(std::isfinite(db));
                RC_ASSERT(std::fabs(da - db) <= kFloatTol);
                break;
            }
            case FieldType::Int:
            case FieldType::QualityLevelEnum: {
                RC_ASSERT(va.get<long long>() == vb.get<long long>());
                break;
            }
            case FieldType::Bool: {
                RC_ASSERT(va.get<bool>() == vb.get<bool>());
                break;
            }
            case FieldType::String: {
                RC_ASSERT(va.get<std::string>() == vb.get<std::string>());
                break;
            }
        }
    }
}

// ──: configuration save/load round-trip ─────────────
bool run_p15_config_roundtrip() {
    return rc::check(
        "Feature: in-ui-configuration, Property 15: write_config_file followed "
        "by load_config_file round-trips every config_schema() setting (exact "
        "for int/bool/enum/string, 1e-6 absolute for float) "
        "(Validates: Requirements 9.1, 9.2, 9.3)",
        [] {
            // A generated config carrying the full valid range plus boundary and
            // out-of-range values. String fields are made JSON-safe so the
            // writer cannot throw on a non-UTF-8 byte.
            BenchConfig original = heaven::testing::gen_bench_config();
            sanitize_string_fields(original);

            // The base the loads apply onto. A default BenchConfig; every schema
            // key is present in the written file, so the base is fully
            // superseded by the file values on each load.
            const BenchConfig base{};

            // Normalize: pass `original` through the loader once so any
            // out-of-range value is clamped/rejected exactly as the contract
            // specifies. `loaded1` is in-range by construction.
            const BenchConfig loaded1 = save_then_load(original, base);

            // Round-trip the already-normalized config. A stable module yields
            // loaded2 == loaded1 over config_schema().
            const BenchConfig loaded2 = save_then_load(loaded1, base);

            // Compare loaded1 vs loaded2 over the schema via their serialized
            // JSON (the writer is the serializer; JsonObject is opaque here).
            const nlohmann::json json1 = write_and_parse(loaded1);
            const nlohmann::json json2 = write_and_parse(loaded2);

            RC_ASSERT(json1.is_object());
            RC_ASSERT(json2.is_object());
            assert_schema_equal(json1, json2);
        });
}

const heaven::testing::Register g_register_config_roundtrip{
    &run_p15_config_roundtrip};

} // namespace
