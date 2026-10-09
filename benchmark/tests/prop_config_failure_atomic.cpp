// Feature: benchmark-config-file,: A well-formed file is a parse
// success regardless of semantic validity (Validates:)
// Feature: benchmark-config-file,: A load failure leaves the base
// configuration unchanged (Validates:)
/*
 * HeavenBench — Property tests for the Config_Loader's parse-success vs.
 * parse-failure boundary and its failure atomicity.
 *
 * Feature: benchmark-config-file
 *   •  (well-formed ⇒ parse success):
 *       For ANY well-formed JSON OBJECT — including ones whose Settings are
 *       out-of-range, fixed-set-rejected, unknown, or type-incompatible —
 *       load_config_file reports parsed_ok == true. The semantic problems
 *       produce diagnostics but never demote the result to a parse failure,
 *       which is reserved for content that is not well-formed JSON.
 *
 *   •  (load failure ⇒ base unchanged):
 *       For ANY base BenchConfig and ANY of the four load-failure modes
 *       (unopenable path, malformed JSON, empty/whitespace-only, and a valid
 *       prefix followed by trailing garbage) load_config_file returns
 *       parsed_ok == false, a config field-equal to the base (NO Setting
 *       applied — including any recognized before the failure point), and at
 *       least one diagnostic.
 *
 *
 * load_config_file is pure with respect to the GPU (it performs only file I/O
 * and never prints), so this runs on the host. The file self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register pattern,
 * so the file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the
 * entry point. RapidCheck's default of 100 successful iterations meets the
 * "≥ 100 iterations" bar (raise via RC_PARAMS=max_success=…).
 *
 * The test target carries lib/nlohmann on its include path (config_file.cpp is
 * compiled into it), so we include "json.hpp" here purely to BUILD well-formed
 * documents to dump to disk — the loader under test still owns all parsing.
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "json.hpp"

#include "engine/cli_parser.h"
#include "engine/config_file.h"

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
using heaven::QualityLevel;

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
           ("heaven_cfg_failatomic_" + std::to_string(stamp) + "_" +
            std::to_string(n) + ".json");
}

// Write `content` to a fresh temp file and return its path (caller removes it).
std::filesystem::path write_temp(const std::string& content) {
    const std::filesystem::path p = unique_temp_path();
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    os << content;
    os.close();
    return p;
}

// ── Schema-mapped field equality ────────────────────────────────

// Thorough field-by-field comparison over EVERY Setting config_schema() maps,
// so a single stray applied field (e.g. the "width":100 prefix in the
// valid-prefix-then-garbage case) is caught. On a load failure the loader
// returns config as an untouched COPY of base, so exact == is correct here; the
// base generator below produces only finite floats so float == never trips on
// NaN.
bool configs_equal(const BenchConfig& a, const BenchConfig& b) {
    // Strings
    if (a.data_dir != b.data_dir) return false;
    if (a.node_file != b.node_file) return false;
    if (a.quality_preset_label != b.quality_preset_label) return false;
    if (a.texture_quality != b.texture_quality) return false;

    // Ints
    if (a.width != b.width) return false;
    if (a.height != b.height) return false;
    if (a.quality.shadow_light_budget != b.quality.shadow_light_budget) return false;
    if (a.quality.msaa_samples != b.quality.msaa_samples) return false;

    // Bools (top level)
    if (a.fullscreen != b.fullscreen) return false;
    if (a.hdr != b.hdr) return false;
    if (a.wind_debug != b.wind_debug) return false;
    if (a.benchmark_mode != b.benchmark_mode) return false;
    if (a.heat_haze_enabled != b.heat_haze_enabled) return false;

    // Floats (top level)
    if (a.hdr_peak_nits != b.hdr_peak_nits) return false;
    if (a.hdr_paper_white_nits != b.hdr_paper_white_nits) return false;
    if (a.cloth_sim_hz != b.cloth_sim_hz) return false;
    if (a.time_of_day != b.time_of_day) return false;

    // Tessellation
    if (a.tessellation.enabled != b.tessellation.enabled) return false;
    if (a.tessellation.scale != b.tessellation.scale) return false;
    if (a.tessellation.factor != b.tessellation.factor) return false;
    if (a.tessellation.distance != b.tessellation.distance) return false;

    // Feature toggles
    if (a.features.grass != b.features.grass) return false;
    if (a.features.particles != b.features.particles) return false;
    if (a.features.god_rays != b.features.god_rays) return false;
    if (a.features.volumetric_fog != b.features.volumetric_fog) return false;
    if (a.features.clouds != b.features.clouds) return false;
    if (a.features.sky != b.features.sky) return false;
    if (a.features.lens_flares != b.features.lens_flares) return false;
    if (a.features.heat_haze != b.features.heat_haze) return false;
    if (a.features.rain != b.features.rain) return false;
    if (a.features.wet_reflections != b.features.wet_reflections) return false;
    if (a.features.soft_shadows != b.features.soft_shadows) return false;
    if (a.features.contact_shadows != b.features.contact_shadows) return false;
    if (a.features.point_light_shadows != b.features.point_light_shadows) return false;
    if (a.features.msaa != b.features.msaa) return false;
    if (a.features.refraction != b.features.refraction) return false;
    if (a.features.reflections != b.features.reflections) return false;
    if (a.features.volumetric_clouds != b.features.volumetric_clouds) return false;
    if (a.features.ssgi != b.features.ssgi) return false;
    if (a.features.massive_foliage != b.features.massive_foliage) return false;
    if (a.features.ground_relief != b.features.ground_relief) return false;

    // Weather (validated)
    if (a.weather.rain_intensity != b.weather.rain_intensity) return false;
    if (a.weather.dry_down_seconds != b.weather.dry_down_seconds) return false;

    // Quality (validated)
    if (a.quality.soft_shadow_level != b.quality.soft_shadow_level) return false;
    if (a.quality.contact_shadow_level != b.quality.contact_shadow_level) return false;
    if (a.quality.cloud_quality != b.quality.cloud_quality) return false;
    if (a.quality.ssgi_quality != b.quality.ssgi_quality) return false;
    if (a.quality.foliage_density != b.quality.foliage_density) return false;

    return true;
}

// ── Random base BenchConfig ─────────────────────────────────────

// Randomises a representative slice of every category so config == base is a
// meaningful assertion. Floats are derived from integer draws so they are
// always finite (no NaN) and exact == is safe. The chosen `width` range
// [200,6000] never collides with the 12345 prefix value injected by the
// valid-prefix-then-garbage case, so that case's atomicity check is meaningful.
// Must be called inside an rc::check lambda (uses the `*` generator operator).
BenchConfig gen_base() {
    BenchConfig c;

    c.width  = *rc::gen::inRange(200, 6000);
    c.height = *rc::gen::inRange(200, 4000);
    c.fullscreen     = *rc::gen::arbitrary<bool>();
    c.hdr            = *rc::gen::arbitrary<bool>();
    c.wind_debug     = *rc::gen::arbitrary<bool>();
    c.benchmark_mode = *rc::gen::arbitrary<bool>();

    c.time_of_day  = static_cast<float>(*rc::gen::inRange(0, 240)) / 10.0f;   // [0,24)
    c.cloth_sim_hz = static_cast<float>(*rc::gen::inRange(1, 240));

    c.tessellation.enabled = *rc::gen::arbitrary<bool>();
    c.tessellation.scale   = static_cast<float>(*rc::gen::inRange(1, 200)) / 100.0f;

    c.features.grass = *rc::gen::arbitrary<bool>();
    c.features.ssgi  = *rc::gen::arbitrary<bool>();
    c.features.sky   = *rc::gen::arbitrary<bool>();

    // heat_haze_enabled is kept in sync with features.heat_haze by the schema;
    // mirror that here so the base is internally consistent.
    const bool hh = *rc::gen::arbitrary<bool>();
    c.heat_haze_enabled  = hh;
    c.features.heat_haze = hh;

    c.weather.rain_intensity   = static_cast<float>(*rc::gen::inRange(0, 100)) / 100.0f;
    c.weather.dry_down_seconds = static_cast<float>(*rc::gen::inRange(1, 60)) / 2.0f; // [0.5,30]

    c.quality.msaa_samples       = *rc::gen::elementOf(std::vector<int>{1, 2, 4});
    c.quality.soft_shadow_level  = static_cast<QualityLevel>(*rc::gen::inRange(0, 3));
    c.quality.shadow_light_budget = *rc::gen::inRange(0, 16);

    c.data_dir = *rc::gen::elementOf(
        std::vector<std::string>{"extracted/heaven", "assets", "data/run", "x"});

    return c;
}

// ── JSON document building  ─────────────────────────

// A pool of canonical Recognized_Keys spanning every value category, plus a
// pool of keys that name no Setting (Unknown_Keys). All ASCII so nlohmann's
// dump() never rejects invalid UTF-8.
const std::vector<std::string>& recognized_keys() {
    static const std::vector<std::string> k = {
        "width", "height", "fullscreen", "hdr", "hdr_peak_nits",
        "time_of_day", "cloth_sim_hz", "wind_debug", "data_dir",
        "quality_preset_label", "texture_quality", "heat_haze_enabled",
        "tessellation.enabled", "tessellation.scale",
        "features.grass", "features.ssgi",
        "weather.rain_intensity", "weather.dry_down_seconds",
        "quality.soft_shadow_level", "quality.msaa_samples",
        "quality.shadow_light_budget", "quality.foliage_density",
    };
    return k;
}

const std::vector<std::string>& unknown_keys() {
    static const std::vector<std::string> k = {
        "bogus", "not_a_setting", "features.unknown", "quality.mystery",
        "xyzzy", "width2", "extra",
    };
    return k;
}

const std::vector<std::string>& safe_strings() {
    static const std::vector<std::string> s = {
        "", "Custom", "High", "some/path", "a b c", "12ab",
    };
    return s;
}

// A random well-formed JSON value of an arbitrary category. Deliberately spans
// int / double / bool / string / null / array so that a Recognized_Key may
// receive an out-of-range numeric, a fixed-set-rejected numeric, OR a
// type-incompatible value (null/array/wrong scalar) — all of which
// requires to still count as a parse SUCCESS. All strings come from an ASCII
// pool, so dumping the document never throws on invalid UTF-8. Must be called
// inside an rc::check lambda.
nlohmann::json gen_json_value() {
    switch (*rc::gen::inRange(0, 6)) {
        case 0:
            // Integer, deliberately able to range well outside any clamp / set.
            return *rc::gen::inRange(-100000, 100000);
        case 1:
            // Double in a wide range (finite — derived from an int draw).
            return static_cast<double>(*rc::gen::inRange(-100000, 100000)) / 1000.0;
        case 2:
            return *rc::gen::arbitrary<bool>();
        case 3:
            return *rc::gen::elementOf(safe_strings());
        case 4:
            return nullptr; // JSON null — type-incompatible with every field
        default:
            return nlohmann::json::array({1, 2, 3}); // array — type-incompatible with every field
    }
}

// ──: well-formed object ⇒ parse success ──────────────
bool run_p3_wellformed_is_success() {
    return rc::check(
        "Feature: benchmark-config-file, Property 3: a well-formed JSON object "
        "is a parse success regardless of semantic validity (Validates: "
        "Requirements 2.4, 2.5)",
        [] {
            // Build a random well-formed top-level object: a mix of recognized
            // keys (with arbitrary-typed, possibly out-of-range values),
            // recognized keys carrying type-incompatible values, and unknown
            // keys. n == 0 yields "{}" which exercises  (a
            // well-formed file with no Recognized_Keys is still a success).
            nlohmann::json doc = nlohmann::json::object();
            const int n = *rc::gen::inRange(0, 15);
            for (int i = 0; i < n; ++i) {
                const int kind = *rc::gen::inRange(0, 3);
                std::string key;
                if (kind == 0 || kind == 1) {
                    key = *rc::gen::elementOf(recognized_keys());
                } else {
                    key = *rc::gen::elementOf(unknown_keys());
                }
                doc[key] = gen_json_value();
            }

            const std::string content = doc.dump();
            const std::filesystem::path path = write_temp(content);

            const BenchConfig base;
            const ConfigLoadResult r = heaven::load_config_file(path.string(), base);

            std::error_code ec;
            std::filesystem::remove(path, ec);

            // The document is well-formed, so parsing SUCCEEDS — distinct from
            // the parse-failure result of malformed content — even though
            // individual Settings may have been clamped, rejected, unknown, or
            // type-incompatible.
            RC_ASSERT(r.parsed_ok == true);
        });
}

// ──: load failure ⇒ base unchanged ───────────────────
bool run_p8_failure_leaves_base_unchanged() {
    return rc::check(
        "Feature: benchmark-config-file, Property 8: a load failure leaves the "
        "base configuration unchanged (Validates: Requirements 6.2, 7.2)",
        [] {
            const BenchConfig base = gen_base();

            // Pick one of the four failure modes.
            const int mode = *rc::gen::inRange(0, 4);

            std::filesystem::path path;
            bool created = false;

            switch (mode) {
                case 0: {
                    // (1) Unopenable path: a name in the temp dir we never
                    // create. Ensure it is absent.
                    path = unique_temp_path();
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                    break;
                }
                case 1: {
                    // (2) Malformed JSON: not well-formed in the Config_Format.
                    const std::string bad = *rc::gen::elementOf(std::vector<std::string>{
                        "{ not json", "{", "}", "[1, 2", "{\"a\":}",
                        "{\"a\" 1}", "{\"a\": 1,}", "tru", "{\"x\": [1,2}",
                        "\"unterminated", R"({"k": 1 "j": 2})",
                    });
                    path = write_temp(bad);
                    created = true;
                    break;
                }
                case 2: {
                    // (3) Empty / whitespace-only content (treated as malformed,

                    const std::string ws = *rc::gen::elementOf(std::vector<std::string>{
                        "", " ", "\n", "\t", "   \n\t  ", "\r\n  ",
                    });
                    path = write_temp(ws);
                    created = true;
                    break;
                }
                default: {
                    // (4) Valid prefix then garbage: a complete, well-formed
                    // object followed by trailing junk. nlohmann rejects trailing
                    // tokens, so the whole document is malformed and NOTHING is
                    // applied — in particular the "width":12345 that appeared
                    // before the garbage must NOT leak into the result. The base
                    // width is in [200,6000], so it always differs from 12345 and
                    // the atomicity check is meaningful.
                    const std::string prefixed = "{\"width\": 12345} GARBAGE";
                    path = write_temp(prefixed);
                    created = true;
                    break;
                }
            }

            const ConfigLoadResult r = heaven::load_config_file(path.string(), base);

            if (created) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }

            // A load failure: parsed_ok is false, the returned config is
            // field-equal to the base (no Setting applied — including any
            // recognized before the failure), and at least one diagnostic was
            // produced.
            RC_ASSERT(r.parsed_ok == false);
            RC_ASSERT(configs_equal(r.config, base));
            RC_ASSERT(!r.diagnostics.empty());
        });
}

bool run_config_failure_atomic_properties() {
    bool ok = true;
    ok = run_p3_wellformed_is_success() && ok;
    ok = run_p8_failure_leaves_base_unchanged() && ok;
    return ok;
}

const heaven::testing::Register g_register_config_failure_atomic{
    &run_config_failure_atomic_properties};

} // namespace
