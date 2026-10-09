// Feature: benchmark-config-file,: A recognized, type-compatible key
//                                              applies its validated value
// Feature: benchmark-config-file,: Keys absent from the file leave
//                                              their fields unchanged
/*
 * HeavenBench — Property tests for the Config_Loader's per-key behaviour.
 *
 * Feature: benchmark-config-file
 *   •: A recognized, type-compatible key applies its validated value
 *
 *   •: Keys absent from the file leave their fields unchanged
 *
 *
 * load_config_file reads a file PATH (not a string), so each case writes a
 * one-key (or empty) JSON object to a unique temp file, loads it onto a base
 * BenchConfig, asserts on the resulting field, then removes the temp file.
 *
 * (recognized key applies its validated value):
 *   For a representative Setting covering each FieldType category — plus the
 *   validated fields — a random, type-compatible value is written as
 *   {key: value} to a temp file and loaded onto a default BenchConfig. The
 *   target field must equal the EXPECTED value, computed INDEPENDENTLY here:
 *     • unvalidated fields (bool/int/float/string) → the raw value verbatim;
 *     • quality level enum   → clamp_quality_level(raw).value;
 *     • weather.rain_intensity → parse_rain_intensity(<value text>).value
 *         (the loader formats the JSON number to a token and routes it through
 *          the same shared helper, so the expectation mirrors that exactly);
 *     • quality.msaa_samples → apply_msaa_samples(raw, prior).value
 *         (fixed-set reject: an out-of-set value retains the prior sample count).
 *   These expectations are the SAME shared engine/types.h helpers the CLI flag
 *   uses, so the test pins validation parity structurally rather than copying
 *   the loader's logic.
 *
 * (absent keys are preserved):
 *   For the same representative Settings, a random value is written into the
 *   BASE config and a well-formed file that OMITS that Setting's key is loaded
 *   onto it (an empty object, or an object carrying an unrelated key). The
 *   loaded field must still equal the base field — the loader touches only keys
 *   the file names.
 *
 * load_config_file is pure with respect to the GPU (it does file I/O only, no
 * Filament/SDL/GPU), so this runs on the host. The file self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point. RapidCheck's default of 100 successful iterations
 * meets the "≥ 100 iterations" bar (raise via RC_PARAMS=max_success=…).
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidcheck.h>

#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/types.h"

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

// ── Temp-file plumbing ──────────────────────────────────────────

// Write `content` to a unique path under the system temp directory and return
// it. Uniqueness comes from a process-wide atomic counter combined with the
// steady-clock tick, so concurrent/iterated cases never collide. Callers remove
// the file once the load completes.
std::string write_temp_json(const std::string& content) {
    namespace fs = std::filesystem;
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long id = counter.fetch_add(1);
    const long long tick =
        std::chrono::steady_clock::now().time_since_epoch().count();

    const fs::path path =
        fs::temp_directory_path() /
        std::format("heaven_cfg_apply_{}_{}.json", tick, id);

    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    os << content;
    os.close();
    return path.string();
}

void remove_temp(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec); // best-effort cleanup; ignore failure
}

// Load a JSON document (already serialized) onto `base` via a temp file.
ConfigLoadResult load_json(const std::string& json, const BenchConfig& base) {
    const std::string path = write_temp_json(json);
    ConfigLoadResult r = heaven::load_config_file(path, base);
    remove_temp(path);
    return r;
}

// ── JSON value text builders (no escaping needed) ───────────────

// Each builder renders a scalar into the textual JSON form the loader parses.
// Strings are drawn from a charset that excludes '"' and '\\' so no escaping is
// required; numbers use std::format's shortest round-trip representation so a
// reparsed double casts back to the identical float.
std::string json_bool(bool b)        { return b ? "true" : "false"; }
std::string json_int(int v)          { return std::format("{}", v); }
std::string json_double(double v)    { return std::format("{}", v); }
std::string json_string(const std::string& s) { return std::format("\"{}\"", s); }

// A safe, escaping-free character pool for generated string Settings.
const std::string& string_charset() {
    static const std::string charset =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./ ";
    return charset;
}

// Representative Setting keys, one per FieldType category plus the validated
// fields. These cover each branch of the loader's type/validation handling
// without enumerating every key.
constexpr std::string_view k_bool_key   = "fullscreen";              // Bool, no validation
constexpr std::string_view k_int_key    = "width";                   // Int, no validation
constexpr std::string_view k_float_key  = "tessellation.scale";      // Float, no validation
constexpr std::string_view k_string_key = "data_dir";                // String, no validation
constexpr std::string_view k_qlevel_key = "quality.soft_shadow_level"; // QualityLevelEnum, clamp
constexpr std::string_view k_rain_key   = "weather.rain_intensity";  // Float, clamp [0,1]
constexpr std::string_view k_msaa_key   = "quality.msaa_samples";    // Int, fixed-set reject

// ──  ──────────────────────────────────────────────────
bool run_p4_recognized_key_applies_validated_value() {
    return rc::check(
        "Feature: benchmark-config-file, Property 4: a recognized, "
        "type-compatible key applies its validated value (Validates: "
        "Requirements 2.3, 3.2)",
        [] {
            const int kind = *rc::gen::inRange(0, 7);
            const BenchConfig base; // defaults; msaa prior == base.quality.msaa_samples

            switch (kind) {
                case 0: { // Bool (no validation) → verbatim
                    const bool v = *rc::gen::arbitrary<bool>();
                    const std::string json =
                        std::format("{{{}: {}}}", json_string(std::string(k_bool_key)),
                                    json_bool(v));
                    const ConfigLoadResult r = load_json(json, base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.fullscreen == v);
                    break;
                }
                case 1: { // Int (no validation) → verbatim
                    const int v = *rc::gen::inRange(-100000, 100000);
                    const std::string json =
                        std::format("{{{}: {}}}", json_string(std::string(k_int_key)),
                                    json_int(v));
                    const ConfigLoadResult r = load_json(json, base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.width == v);
                    break;
                }
                case 2: { // Float (no validation) → verbatim (round-trip exact)
                    const int n = *rc::gen::inRange(-1000000, 1000000);
                    const float v = static_cast<float>(n) / 10000.0f;
                    const std::string json =
                        std::format("{{{}: {}}}", json_string(std::string(k_float_key)),
                                    json_double(static_cast<double>(v)));
                    const ConfigLoadResult r = load_json(json, base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.tessellation.scale == v);
                    break;
                }
                case 3: { // String (no validation) → verbatim
                    const std::string v = *rc::gen::container<std::string>(
                        rc::gen::elementOf(string_charset()));
                    const std::string json =
                        std::format("{{{}: {}}}", json_string(std::string(k_string_key)),
                                    json_string(v));
                    const ConfigLoadResult r = load_json(json, base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.data_dir == v);
                    break;
                }
                case 4: { // QualityLevelEnum → clamp_quality_level(raw).value
                    const int raw = *rc::gen::inRange(-3, 6);
                    const std::string json =
                        std::format("{{{}: {}}}", json_string(std::string(k_qlevel_key)),
                                    json_int(raw));
                    const ConfigLoadResult r = load_json(json, base);
                    RC_ASSERT(r.parsed_ok);
                    const heaven::QualityLevel expected =
                        heaven::clamp_quality_level(raw).value;
                    RC_ASSERT(r.config.quality.soft_shadow_level == expected);
                    break;
                }
                case 5: { // weather.rain_intensity → parse_rain_intensity(text).value
                    // Range spans below 0, inside [0,1], and above 1 so the
                    // clamp branches are all exercised.
                    const int n = *rc::gen::inRange(-30000, 30000);
                    const double v = static_cast<double>(n) / 10000.0;
                    const std::string num = json_double(v);
                    const std::string json = std::format(
                        "{{{}: {}}}", json_string(std::string(k_rain_key)), num);
                    const ConfigLoadResult r = load_json(json, base);
                    RC_ASSERT(r.parsed_ok);
                    // The loader formats the parsed double to a token and routes
                    // it through parse_rain_intensity; mirror that exactly.
                    const float expected =
                        heaven::parse_rain_intensity(num.c_str()).value;
                    RC_ASSERT(r.config.weather.rain_intensity == expected);
                    break;
                }
                default: { // quality.msaa_samples → apply_msaa_samples(raw, prior).value
                    const int raw = *rc::gen::inRange(-1, 9);
                    const std::string json =
                        std::format("{{{}: {}}}", json_string(std::string(k_msaa_key)),
                                    json_int(raw));
                    const ConfigLoadResult r = load_json(json, base);
                    RC_ASSERT(r.parsed_ok);
                    const int expected =
                        heaven::apply_msaa_samples(raw, base.quality.msaa_samples).value;
                    RC_ASSERT(r.config.quality.msaa_samples == expected);
                    break;
                }
            }
        });
}

// ──  ──────────────────────────────────────────────────

// For each representative Setting: randomize the field in the BASE config, load
// a well-formed file that OMITS that Setting's key (sometimes an empty object,
// sometimes an unrelated key), and assert the field is preserved. The unrelated
// key, when used, is chosen distinct from the target so the file genuinely
// omits the target key.
bool run_p5_absent_key_preserved() {
    return rc::check(
        "Feature: benchmark-config-file, Property 5: keys absent from the file "
        "leave their fields unchanged (Validates: Requirements 3.3)",
        [] {
            const int kind = *rc::gen::inRange(0, 7);

            // The body of the omitting document: either an empty object or one
            // that sets an unrelated, type-compatible key. Both are well-formed
            // and omit every target key tested below.
            const bool use_unrelated = *rc::gen::arbitrary<bool>();

            BenchConfig base; // start from defaults, randomize one field below

            switch (kind) {
                case 0: {
                    base.fullscreen = *rc::gen::arbitrary<bool>();
                    const std::string body =
                        use_unrelated ? R"("width": 800)" : "";
                    const ConfigLoadResult r =
                        load_json(std::format("{{{}}}", body), base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.fullscreen == base.fullscreen);
                    break;
                }
                case 1: {
                    base.width = *rc::gen::inRange(-100000, 100000);
                    const std::string body =
                        use_unrelated ? R"("fullscreen": true)" : "";
                    const ConfigLoadResult r =
                        load_json(std::format("{{{}}}", body), base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.width == base.width);
                    break;
                }
                case 2: {
                    const int n = *rc::gen::inRange(-1000000, 1000000);
                    base.tessellation.scale = static_cast<float>(n) / 10000.0f;
                    const std::string body =
                        use_unrelated ? R"("width": 640)" : "";
                    const ConfigLoadResult r =
                        load_json(std::format("{{{}}}", body), base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.tessellation.scale == base.tessellation.scale);
                    break;
                }
                case 3: {
                    base.data_dir = *rc::gen::container<std::string>(
                        rc::gen::elementOf(string_charset()));
                    const std::string body =
                        use_unrelated ? R"("height": 480)" : "";
                    const ConfigLoadResult r =
                        load_json(std::format("{{{}}}", body), base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.data_dir == base.data_dir);
                    break;
                }
                case 4: {
                    const int lvl = *rc::gen::inRange(0, 3);
                    base.quality.soft_shadow_level =
                        static_cast<heaven::QualityLevel>(lvl);
                    const std::string body =
                        use_unrelated ? R"("fullscreen": false)" : "";
                    const ConfigLoadResult r =
                        load_json(std::format("{{{}}}", body), base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.quality.soft_shadow_level ==
                              base.quality.soft_shadow_level);
                    break;
                }
                case 5: {
                    const int n = *rc::gen::inRange(0, 10000);
                    base.weather.rain_intensity = static_cast<float>(n) / 10000.0f;
                    const std::string body =
                        use_unrelated ? R"("width": 1024)" : "";
                    const ConfigLoadResult r =
                        load_json(std::format("{{{}}}", body), base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.weather.rain_intensity ==
                              base.weather.rain_intensity);
                    break;
                }
                default: {
                    const int samples = *rc::gen::elementOf(
                        std::vector<int>{1, 2, 4});
                    base.quality.msaa_samples = samples;
                    const std::string body =
                        use_unrelated ? R"("fullscreen": true)" : "";
                    const ConfigLoadResult r =
                        load_json(std::format("{{{}}}", body), base);
                    RC_ASSERT(r.parsed_ok);
                    RC_ASSERT(r.config.quality.msaa_samples ==
                              base.quality.msaa_samples);
                    break;
                }
            }
        });
}

bool run_config_apply_properties() {
    bool ok = true;
    ok = run_p4_recognized_key_applies_validated_value() && ok;
    ok = run_p5_absent_key_preserved() && ok;
    return ok;
}

const heaven::testing::Register g_register_config_apply{
    &run_config_apply_properties};

} // namespace
