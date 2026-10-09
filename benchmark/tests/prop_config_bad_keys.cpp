// Feature: benchmark-config-file,: Unknown and type-incompatible
// keys are reported individually and never block valid keys.
/*
 * HeavenBench — Property test for the Config_Loader's handling of a
 * Config_File that mixes valid Recognized_Keys, Unknown_Keys, and
 * type-incompatible Recognized_Keys.
 *
 * Feature: benchmark-config-file
 *   •: Unknown and type-incompatible keys are reported individually
 *     and never block valid keys
 *
 *
 * (design.md):
 *   For any well-formed JSON object containing a MIX of valid Recognized_Keys,
 *   Unknown_Keys, and type-incompatible Recognized_Keys, load_config_file
 *     (a) emits at least one diagnostic for EACH unknown or type-incompatible
 *         key,
 *     (b) leaves the field of every such key unchanged (relative to base), and
 *     (c) still applies every valid Recognized_Key to its field.
 *
 * STRATEGY
 *   The schema (config_schema()) exposes every Recognized_Key and its expected
 *   FieldType, so the generator partitions the recognized keys into three
 *   DISJOINT roles per draw — valid / type-incompatible / skipped — and adds a
 *   handful of Unknown_Keys (prefixed so they can never collide with a real
 *   schema key). Two JSON documents are built from the same draws:
 *     • `mixed`      — valid + incompatible + unknown keys, written to a temp
 *                      file and loaded.
 *     • `valid_only` — ONLY the valid keys with the IDENTICAL values, written
 *                      and loaded against the same base.
 *
 *   Because load_config_file is deterministic, the valid-only load is the
 *   ground truth for "every valid key applied, every other field left at base":
 *   in it the incompatible-role and skipped keys are simply absent, so their
 *   fields stay at base, while the valid keys are applied exactly as they would
 *   be in isolation. (b)+(c) therefore reduce to: the configuration
 *   produced by the mixed load equals the configuration produced by the
 *   valid-only load. Equality is checked structurally over EVERY Setting by
 *   serializing each result with write_config_file (which emits the whole
 *   schema) and comparing the text — this avoids reaching into individual
 *   BenchConfig fields (the JsonObject/JsonValue wrappers are opaque outside
 *   config_file.cpp) while still covering the full field set.
 *
 *   Valid values are drawn strictly IN RANGE for every validated Setting, so
 *   the valid-only load emits zero diagnostics. Consequently the mixed load's
 *   diagnostics are exactly one per Unknown_Key plus one per type-incompatible
 *   key, which pins (a) AND the "reported individually" guarantee
 * the diagnostic count equals (#unknown + #incompatible)
 *   and each such key is named in its own diagnostic.
 *
 *   Type-incompatible values use categories that NO FieldType accepts (JSON
 *   null, array, object) plus a type-specific scalar mismatch (a string for a
 *   non-string field, a bool for a string field), exercising
 *   including the "null is incompatible for all types" rule.
 *
 * load_config_file / write_config_file / config_schema are pure (no GPU; only
 * the file I/O they are defined to do), so this runs on the host. The file
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/
 * CMakeLists.txt picks it up without editing the entry point. RapidCheck's
 * default of 100 successful iterations meets the "≥ 100 iterations" bar.
 *
 * The test target carries lib/nlohmann on its include path (config_file.cpp
 * links into heaven_benchmark_tests), so json.hpp is used here to build the
 * input documents rather than hand-assembling JSON strings.
 */
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "json.hpp"

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
using heaven::FieldSpec;
using heaven::FieldType;

// Unique scratch path under the system temp dir. A static atomic counter makes
// every path distinct within the process (the harness is single-threaded, but
// the atomic is cheap insurance); the caller removes the file when done.
std::string unique_temp_path() {
    static std::atomic<std::uint64_t> ctr{0};
    const std::uint64_t n = ctr.fetch_add(1);
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("heaven_prop9_" + std::to_string(n) + ".json");
    return p.string();
}

// Write `text` to `path`, returning whether the write succeeded.
bool write_text(const std::string& path, const std::string& text) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os.is_open()) return false;
    os << text;
    os.flush();
    return os.good();
}

// Read the entire file at `path` (empty string if it cannot be opened).
std::string read_text(const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(is)),
                       std::istreambuf_iterator<char>()};
}

// Best-effort removal of a scratch file (ignore errors).
void remove_quietly(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

bool run_p9_bad_keys_property() {
    return rc::check(
        "Feature: benchmark-config-file, Property 9: unknown / "
        "type-incompatible keys are reported individually and never block valid "
        "keys (Validates: Requirements 8.1, 8.2, 8.3, 8.4)",
        [] {
            const std::vector<FieldSpec>& schema = heaven::config_schema();

            // ── Generator helpers (draw inside the property body) ──

            // A finite double in [lo, hi]. Drawn from an integer grid so it can
            // never be NaN/Inf (which JSON cannot represent and nlohmann would
            // serialize as null, breaking the round trip).
            auto draw_double_in = [](double lo, double hi) -> double {
                constexpr int kGrid = 100000;
                const int k = *rc::gen::inRange(0, kGrid + 1);
                return lo + (hi - lo) * (static_cast<double>(k) /
                                         static_cast<double>(kGrid));
            };

            // A short ASCII string (safe UTF-8 so write_config_file's dump()
            // never throws on the value).
            auto draw_ascii = []() -> std::string {
                static const std::string charset =
                    "abcdefghijklmnopqrstuvwxyz0123456789_";
                return *rc::gen::container<std::string>(rc::gen::map(
                    rc::gen::inRange<std::size_t>(0, charset.size()),
                    [](std::size_t i) { return charset[i]; }));
            };

            // A type-correct, IN-RANGE value for a Recognized_Key, so the
            // valid-only load applies it with zero diagnostics. Validated keys
            // are special-cased to their accepted range / set; everything else
            // is keyed off the declared FieldType.
            auto draw_valid = [&](const std::string& key,
                                  FieldType t) -> nlohmann::json {
                if (key == "weather.rain_intensity")
                    return draw_double_in(0.0, 1.0);
                if (key == "music.volume" || key == "bloom_strength")
                    return draw_double_in(0.0, 1.0);
                if (key == "android_preview_fps")
                    return *rc::gen::inRange(0, 241);
                if (key == "android_render_scale")
                    return draw_double_in(0.125, 1.0);
                if (key == "weather.dry_down_seconds")
                    return draw_double_in(0.5, 30.0);
                if (key == "quality.shadow_light_budget")
                    return *rc::gen::inRange(0, 17); // [0, 16]
                if (key == "quality.foliage_density")
                    return draw_double_in(1.0, 2.0); // ≤ foliage_density_max ≈ 2.15
                if (key == "quality.msaa_samples") {
                    const int idx = *rc::gen::inRange(0, 3);
                    return std::vector<int>{1, 2, 4}[idx];
                }
                if (key == "quality.sun_shadow_map_size") {
                    const int idx = *rc::gen::inRange(0, 3);
                    return std::vector<int>{1024, 2048, 4096}[idx];
                }
                if (key == "quality.point_shadow_map_size") {
                    const int idx = *rc::gen::inRange(0, 3);
                    return std::vector<int>{256, 512, 1024}[idx];
                }
                if (key == "cpu_stress.workers")
                    return *rc::gen::inRange(0, 257);
                if (key == "cpu_stress.duration_seconds")
                    return draw_double_in(0.01, 60.0);
                if (key == "render_test_scene")
                    return "contact-shadow";
                if (key == "quality_preset")
                    return "balanced";
                if (key == "quality_preset_label")
                    return "Balanced System";
                if (key == "material_workflow")
                    return "metallic-roughness";
                if (key == "texture_quality") {
                    const int idx = *rc::gen::inRange(0, 3);
                    return std::vector<std::string>{"original", "2k", "4k"}[idx];
                }
                switch (t) {
                    case FieldType::Bool:
                        return *rc::gen::arbitrary<bool>();
                    case FieldType::Int:
                        return *rc::gen::inRange(-10000, 10000);
                    case FieldType::Float:
                        return draw_double_in(-1000.0, 1000.0);
                    case FieldType::String:
                        return draw_ascii();
                    case FieldType::QualityLevelEnum:
                        return *rc::gen::inRange(0, 3); // {0,1,2}
                }
                return {};
            };

            // A value whose JSON type is INCOMPATIBLE with the field. null,
            // array, and object are rejected by every FieldType; the scalar
            // fallback is a string for non-string fields and a bool for string
            // fields (including null-for-all-types).
            auto draw_incompatible = [&](FieldType t) -> nlohmann::json {
                switch (*rc::gen::inRange(0, 4)) {
                    case 0: return {}; // null
                    case 1: return nlohmann::json::array({1, 2, 3});
                    case 2: return nlohmann::json::object({{"k", 1}});
                    default:
                        if (t == FieldType::String)
                            return true; // bool ≠ string; braces would make an array.
                        return "not_a_value";
                }
            };

            // ── Build the two documents from one set of draws ──
            nlohmann::json mixed      = nlohmann::json::object();
            nlohmann::json valid_only = nlohmann::json::object();
            std::vector<std::string> incompatible_keys;
            std::vector<std::string> unknown_keys;

            for (const FieldSpec& f : schema) {
                const std::string key(f.key);
                // 0 = valid, 1 = type-incompatible, 2 = skipped.
                switch (*rc::gen::inRange(0, 3)) {
                    case 0: {
                        const nlohmann::json v = draw_valid(key, f.type);
                        mixed[key]      = v;
                        valid_only[key] = v;
                        break;
                    }
                    case 1: {
                        mixed[key] = draw_incompatible(f.type);
                        incompatible_keys.push_back(key);
                        break;
                    }
                    default:
                        break; // skip
                }
            }

            // Unknown keys: prefixed + indexed so they are guaranteed distinct
            // and absent from the schema; their value is arbitrary (ignored).
            const int n_unknown = *rc::gen::inRange(0, 5);
            for (int i = 0; i < n_unknown; ++i) {
                const std::string uk =
                    "unk_" + std::to_string(i) + "_" + draw_ascii();
                mixed[uk] = 123;
                unknown_keys.push_back(uk);
            }

            // ── Write inputs, load both, then clean up the input files ──
            const std::string mixed_in = unique_temp_path();
            const std::string valid_in = unique_temp_path();
            RC_ASSERT(write_text(mixed_in, mixed.dump(2)));
            RC_ASSERT(write_text(valid_in, valid_only.dump(2)));

            const BenchConfig base{};
            const ConfigLoadResult mixed_res = heaven::load_config_file(mixed_in, base);
            const ConfigLoadResult valid_res = heaven::load_config_file(valid_in, base);

            remove_quietly(mixed_in);
            remove_quietly(valid_in);

            // Both documents are well-formed objects → parse success.
            RC_ASSERT(mixed_res.parsed_ok);
            RC_ASSERT(valid_res.parsed_ok);

            // The valid-only load uses strictly in-range values, so it is clean.
            RC_ASSERT(valid_res.diagnostics.empty());

            // (b)+(c): the mixed load applies every valid key and leaves every
            // unknown / incompatible / skipped field at base — i.e. it produces
            // the SAME configuration as the valid-only load. Compare structurally
            // over the whole schema via write_config_file.
            const std::string mixed_out = unique_temp_path();
            const std::string valid_out = unique_temp_path();
            RC_ASSERT(heaven::write_config_file(mixed_out, mixed_res.config).empty());
            RC_ASSERT(heaven::write_config_file(valid_out, valid_res.config).empty());
            const std::string mixed_cfg_text = read_text(mixed_out);
            const std::string valid_cfg_text = read_text(valid_out);
            remove_quietly(mixed_out);
            remove_quietly(valid_out);
            RC_ASSERT(mixed_cfg_text == valid_cfg_text);

            // (a) +: each unknown and each type-incompatible key
            // is named in its own diagnostic. The key is quoted in the message
            // ("'<key>'"), so a quoted-substring search is collision-free across
            // the distinct keys.
            auto names_key = [&](const std::string& key) {
                const std::string needle = "'" + key + "'";
                return std::ranges::any_of(mixed_res.diagnostics, [&](const std::string& d) {
                    return d.contains(needle);
                });
            };
            for (const std::string& uk : unknown_keys) {
                RC_ASSERT(names_key(uk));
            }
            for (const std::string& ik : incompatible_keys) {
                RC_ASSERT(names_key(ik));
            }

            // Reported INDIVIDUALLY: exactly one diagnostic per unknown key plus
            // one per type-incompatible key (valid keys contributed none), so the
            // count equals their sum.
            RC_ASSERT(mixed_res.diagnostics.size() ==
                      unknown_keys.size() + incompatible_keys.size());
        });
}

const heaven::testing::Register g_register_config_bad_keys{
    &run_p9_bad_keys_property};

} // namespace
