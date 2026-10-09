// Feature: benchmark-config-file,: Configuration round trip
//                                              (Validates:,

/*
 * HeavenBench — Property test for the Config_Writer / Config_Loader round
 * trip.
 *
 * Feature: benchmark-config-file
 *   •  (configuration round trip):
 *       For ANY BenchConfig whose Settings hold IN-RANGE, valid values,
 *       write_config_file(temp, cfg) followed by load_config_file(temp)
 *       produces a BenchConfig EQUAL to the original on EVERY Setting
 *       (floating-point Settings equal with NO rounding difference) and yields
 *       ZERO diagnostics of any kind (the writer returns an empty vector and
 *       the loader's diagnostics are empty with parsed_ok == true).
 *
 *
 * Why the round trip is exact. The writer stores each float field into a JSON
 * node (promoted to double) and serializes it with nlohmann's shortest-decimal
 * formatter, which round-trips an IEEE-754 double bit-for-bit. The loader reads
 * get<double>() and casts back to float. Since the value serialized is exactly
 * the float→double promotion of cfg's field, the reloaded double casts back to
 * the IDENTICAL float — so an exact `==` comparison is correct, not merely
 * approximate. The generator below therefore only needs to avoid values the
 * loader would TRANSFORM (out-of-range clamps, fixed-set rejects, time_of_day
 * wrap) and values JSON cannot represent (NaN/Inf): keep every validated field
 * in range, keep time_of_day in [0,24) so wrap_24 is a no-op, and derive floats
 * from finite integer draws.
 *
 * load_config_file / write_config_file are pure with respect to the GPU (they
 * perform file I/O only, no Filament/SDL/GPU, and never print), so this runs on
 * the host. The file self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" bar (raise via RC_PARAMS=max_success=…).
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
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
using heaven::QualityLevel;

// ── Temp-file plumbing ──────────────────────────────────────────

// Unique path under the system temp dir. A static counter plus a steady-clock
// stamp keeps paths distinct within this process run and across re-runs without
// pulling in platform headers. Each case writes via write_config_file, reloads
// via load_config_file, then removes the file.
std::filesystem::path unique_temp_path() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1);
    const std::uint64_t stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("heaven_cfg_roundtrip_" + std::to_string(stamp) + "_" +
            std::to_string(n) + ".json");
}

// ── Schema-mapped field equality ────────────────────────────────

// Thorough field-by-field comparison over EVERY Setting config_schema() maps,
// so a single field that fails to round-trip is caught. Floats are compared
// with exact `==`: the writer/loader path round-trips each float bit-for-bit
// (see the file header), and the generator produces only finite floats, so `==`
// never trips on NaN.
bool configs_equal(const BenchConfig& a, const BenchConfig& b) {
    // Strings
    if (a.data_dir != b.data_dir) return false;
    if (a.node_file != b.node_file) return false;
    if (a.texture_quality != b.texture_quality) return false;
    // quality_preset_label is a deprecated read alias. Canonical writes emit
    // only quality_preset, so the free-form legacy label is not round-tripped.

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
    if (a.bloom_strength != b.bloom_strength) return false;

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

// ── ASCII string generator (no escaping needed) ─────────────────

// Strings are drawn from a charset that excludes '"' and '\\' so the writer's
// JSON serialization never needs escaping and the reload is byte-identical.
const std::string& string_charset() {
    static const std::string charset =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./ ";
    return charset;
}

std::string gen_ascii_string() {
    return *rc::gen::container<std::string>(rc::gen::elementOf(string_charset()));
}

// ── In-range / valid BenchConfig generator ──────────────────────

// Randomises EVERY schema-mapped Setting within valid bounds so the writer
// emits only Recognized_Keys with in-range values and the loader applies each
// without clamping, rejecting, or wrapping — the precondition for an exact
// round trip. Floats are derived from finite integer draws
// (always finite, never NaN/Inf which JSON cannot represent) and round-trip
// bit-for-bit through the writer/loader. Must be called inside an rc::check
// lambda (uses the `*` generator operator).
BenchConfig gen_valid_config() {
    BenchConfig c; // defaults provide the QualityKnobs bounds (foliage_density_max, …)

    // Strings (no validation).
    c.data_dir             = gen_ascii_string();
    c.node_file            = gen_ascii_string();
    c.quality_preset_label = gen_ascii_string();
    c.texture_quality = static_cast<heaven::TextureQuality>(
        *rc::gen::inRange(0, 3));

    // Ints (no validation) — kept to a sane positive resolution range.
    c.width  = *rc::gen::inRange(1, 16384);
    c.height = *rc::gen::inRange(1, 16384);

    // Bools (no validation).
    c.fullscreen     = *rc::gen::arbitrary<bool>();
    c.hdr            = *rc::gen::arbitrary<bool>();
    c.wind_debug     = *rc::gen::arbitrary<bool>();
    c.benchmark_mode = *rc::gen::arbitrary<bool>();

    // Unvalidated floats — finite, derived from integer draws so the round trip
    // is exact. Spans negatives and a wide magnitude range.
    c.hdr_peak_nits        = static_cast<float>(*rc::gen::inRange(0, 100000)) / 10.0f;
    c.hdr_paper_white_nits = static_cast<float>(*rc::gen::inRange(0, 100000)) / 10.0f;
    c.cloth_sim_hz         = static_cast<float>(*rc::gen::inRange(1, 100000)) / 100.0f;

    c.tessellation.enabled  = *rc::gen::arbitrary<bool>();
    c.tessellation.scale    = static_cast<float>(*rc::gen::inRange(-100000, 100000)) / 1000.0f;
    c.tessellation.factor   = static_cast<float>(*rc::gen::inRange(-100000, 100000)) / 1000.0f;
    c.tessellation.distance = static_cast<float>(*rc::gen::inRange(-100000, 100000)) / 1000.0f;

    // time_of_day — kept in [0,24) so wrap_24 is a no-op and the value is
    // preserved exactly (a value outside the range would be wrapped by the
    // loader, breaking the identity round trip).
    c.time_of_day = static_cast<float>(*rc::gen::inRange(0, 24000)) / 1000.0f;
    c.bloom_strength = static_cast<float>(*rc::gen::inRange(0, 1001)) / 1000.0f;

    // heat_haze_enabled and features.heat_haze MUST be generated in sync: the
    // schema's heat_haze_enabled load sets both, and the writer emits the value
    // from cfg.heat_haze_enabled, so an unequal pair could not be reproduced.
    const bool hh = *rc::gen::arbitrary<bool>();
    c.heat_haze_enabled  = hh;
    c.features.heat_haze = hh;

    // Feature toggles (no validation).
    c.features.grass               = *rc::gen::arbitrary<bool>();
    c.features.particles           = *rc::gen::arbitrary<bool>();
    c.features.god_rays            = *rc::gen::arbitrary<bool>();
    c.features.volumetric_fog      = *rc::gen::arbitrary<bool>();
    c.features.clouds              = *rc::gen::arbitrary<bool>();
    c.features.sky                 = *rc::gen::arbitrary<bool>();
    c.features.lens_flares         = *rc::gen::arbitrary<bool>();
    c.features.rain                = *rc::gen::arbitrary<bool>();
    c.features.wet_reflections     = *rc::gen::arbitrary<bool>();
    c.features.soft_shadows        = *rc::gen::arbitrary<bool>();
    c.features.contact_shadows     = *rc::gen::arbitrary<bool>();
    c.features.point_light_shadows = *rc::gen::arbitrary<bool>();
    c.features.msaa                = *rc::gen::arbitrary<bool>();
    c.features.refraction          = *rc::gen::arbitrary<bool>();
    c.features.reflections         = *rc::gen::arbitrary<bool>();
    c.features.volumetric_clouds   = *rc::gen::arbitrary<bool>();
    c.features.ssgi                = *rc::gen::arbitrary<bool>();
    c.features.massive_foliage     = *rc::gen::arbitrary<bool>();
    c.features.ground_relief       = *rc::gen::arbitrary<bool>();

    // Weather (validated) — kept strictly in range so no clamp fires.
    //   rain_intensity   ∈ [0,1]
    //   dry_down_seconds ∈ [0.5,30]
    c.weather.rain_intensity   = static_cast<float>(*rc::gen::inRange(0, 10000)) / 10000.0f;
    c.weather.dry_down_seconds = static_cast<float>(*rc::gen::inRange(5, 300)) / 10.0f;

    // Quality (validated) — levels in {0,1,2}, budget in [0,16], foliage in
    // [1.0, foliage_density_max], msaa in {1,2,4}; all in range so the loader
    // applies them verbatim.
    c.quality.soft_shadow_level    = static_cast<QualityLevel>(*rc::gen::inRange(0, 3));
    c.quality.contact_shadow_level = static_cast<QualityLevel>(*rc::gen::inRange(0, 3));
    c.quality.cloud_quality        = static_cast<QualityLevel>(*rc::gen::inRange(0, 3));
    c.quality.ssgi_quality         = static_cast<QualityLevel>(*rc::gen::inRange(0, 3));
    c.quality.shadow_light_budget  = *rc::gen::inRange(0, c.quality.shadow_light_budget_max + 1);
    c.quality.msaa_samples         = *rc::gen::elementOf(std::vector<int>{1, 2, 4});

    // foliage_density ∈ [1.0, foliage_density_max] (≈ 2.146). Draw across the
    // valid range from an integer so the value is finite and round-trips.
    {
        const float lo = 1.0f;
        const float hi = c.quality.foliage_density_max;
        const int steps = 10000;
        const int n = *rc::gen::inRange(0, steps + 1);
        c.quality.foliage_density =
            lo + (hi - lo) * (static_cast<float>(n) / static_cast<float>(steps));
    }

    return c;
}

// ──: configuration round trip ───────────────────────
bool run_p10_config_round_trip() {
    return rc::check(
        "Feature: benchmark-config-file, Property 10: a write_config_file then "
        "load_config_file round trip preserves every Setting exactly and yields "
        "zero diagnostics (Validates: Requirements 9.1, 9.2, 10.1, 10.2, 10.3)",
        [] {
            const BenchConfig cfg = gen_valid_config();
            const std::filesystem::path path = unique_temp_path();

            // Write the configuration. The writer emits one key for every
            // schema Setting  and returns NO diagnostics on a
            // successful write.
            const std::vector<std::string> write_diags =
                heaven::write_config_file(path.string(), cfg);

            // Reload onto a default base. Because the file contains every key,
            // the base is irrelevant — every field is overwritten from the file.
            const ConfigLoadResult r =
                heaven::load_config_file(path.string(), BenchConfig{});

            std::error_code ec;
            std::filesystem::remove(path, ec); // best-effort cleanup

            // Zero diagnostics of ANY kind: the writer wrote cleanly and the
            // reload — of an all-in-range document — clamps/rejects nothing

            RC_ASSERT(write_diags.empty());
            RC_ASSERT(r.parsed_ok);
            RC_ASSERT(r.diagnostics.empty());

            // Every Setting equals the original, floats with no rounding
            // difference.
            RC_ASSERT(configs_equal(r.config, cfg));
        });
}

bool run_config_roundtrip_properties() {
    return run_p10_config_round_trip();
}

const heaven::testing::Register g_register_config_roundtrip{
    &run_config_roundtrip_properties};

} // namespace
