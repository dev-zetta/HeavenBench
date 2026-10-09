/*
 * HeavenBench — JSON configuration file (implementation).
 *
 * This is the ONLY translation unit that includes the vendored Config_Library
 * (lib/nlohmann/json.hpp); the public header (engine/config_file.h) exposes the
 * JsonValue / JsonObject value types only as opaque, forward-declared wrappers,
 * so the library's compile-time cost and headers stay confined here. The module
 * is GPU-free: it references no Filament, SDL, or GPU-API symbol (Requirement
 * and never prints — every condition is collected into a returned
 * diagnostics vector.
 *
 * stands up the skeleton: the JsonValue / JsonObject wrapper
 * definitions plus stub config_schema() / load_config_file() / write_config_file()
 * bodies that compile and link. The schema table, loader,
 * and writer  fill these in.
 */
#include "engine/config_file.h"
#include "engine/render_presets.h"
#include "engine/render_test_scene_catalog.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

namespace heaven {

// Thin wrappers completing the opaque types declared in the public header. They
// hold a reference to the underlying Config_Library node so the FieldSpec
// callbacks can read/write JSON without json.hpp appearing in config_file.h.
struct JsonValue {
    const nlohmann::json& node;
};

struct JsonObject {
    nlohmann::json& node;
};

namespace {

// ── FieldSpec makers (single source of truth) ─────────

// Each maker takes a canonical dotted JSON key and a *generic* field accessor
// `acc` — a lambda `[](auto& c) -> auto& { return c.<field>; }`. Because it is
// generic, the same accessor yields a mutable reference when invoked on a
// `BenchConfig&` (the loader's `load` path) and a const reference when invoked
// on a `const BenchConfig&` (the writer's `store` path), so one accessor drives
// both directions and the JSON key ↔ field bridge cannot drift.

// The loaders here assume the JSON value has ALREADY been type-checked against
// FieldType by the loader  before `load` is invoked — they read the
// value directly. Fields with no validator are assigned verbatim; the validated
// fields below route through the SAME shared engine/types.h helpers the CLI flag
// uses, so a Setting validates identically from either source.

template <typename Acc>
FieldSpec bool_field(std::string_view key, Acc acc) {
    return FieldSpec{
        key, FieldType::Bool,
        [acc](const JsonValue& v, BenchConfig& c, Diagnostics&) {
            acc(c) = v.node.get<bool>();
        },
        [acc, key](const BenchConfig& c, JsonObject& o) {
            o.node[std::string(key)] = acc(c);
        }};
}

template <typename Acc>
FieldSpec int_field(std::string_view key, Acc acc) {
    return FieldSpec{
        key, FieldType::Int,
        [acc](const JsonValue& v, BenchConfig& c, Diagnostics&) {
            acc(c) = v.node.get<int>();
        },
        [acc, key](const BenchConfig& c, JsonObject& o) {
            o.node[std::string(key)] = acc(c);
        }};
}

template <typename Acc>
FieldSpec float_field(std::string_view key, Acc acc) {
    return FieldSpec{
        key, FieldType::Float,
        [acc](const JsonValue& v, BenchConfig& c, Diagnostics&) {
            acc(c) = static_cast<float>(v.node.get<double>());
        },
        [acc, key](const BenchConfig& c, JsonObject& o) {
            o.node[std::string(key)] = acc(c);
        }};
}

template <typename Acc>
FieldSpec string_field(std::string_view key, Acc acc) {
    return FieldSpec{
        key, FieldType::String,
        [acc](const JsonValue& v, BenchConfig& c, Diagnostics&) {
            acc(c) = v.node.get<std::string>();
        },
        [acc, key](const BenchConfig& c, JsonObject& o) {
            o.node[std::string(key)] = acc(c);
        }};
}

// QualityLevelEnum field: the JSON integer 0/1/2 maps to the ordered quality
// level, routed through the SAME clamp_quality_level helper the four CLI level
// flags use; an out-of-range level is pulled to the nearest valid level and a
// clamp diagnostic mirroring the CLI WARN text (key in place of the flag) is
// pushed. `store` writes the level back as its
// integer form for round-trip parity.
template <typename Acc>
FieldSpec quality_level_field(std::string_view key, Acc acc) {
    return FieldSpec{
        key, FieldType::QualityLevelEnum,
        [acc, key](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
            const int raw = v.node.get<int>();
            const QualityLevelClamp r = clamp_quality_level(raw);
            if (r.clamped) {
                d.warn(std::format("WARN: {} '{}' out of range [0,2]; clamped to level {}",
                                   key, raw, static_cast<int>(r.value)));
            }
            acc(c) = r.value;
        },
        [acc, key](const BenchConfig& c, JsonObject& o) {
            o.node[std::string(key)] = static_cast<int>(acc(c));
        }};
}

} // namespace

// config_schema() — single source of truth bridging JSON keys to BenchConfig
// fields. It defines exactly one FieldSpec per
// BenchConfig-mapping Setting that the CLI exposes, plus the two config-only
// Android launcher/preview fields, and NO key for `--help`
// (maps to no field) or `--api` (a documented no-op that leaves graphics_api at
// its default). `benchmark_mode` IS emitted so the writer's output covers every
// emitted key for the round trip. Validation parity with the
// CLI  is structural: every validated field routes through the
// identical shared helper the corresponding flag uses.
const std::vector<FieldSpec>& config_schema() {
    static const std::vector<FieldSpec> schema = [] {
        std::vector<FieldSpec> s;

        // ── Top-level scalars / strings (no validation) ────────
        s.push_back(string_field("data_dir", [](auto& c) -> auto& { return c.data_dir; }));
        s.push_back(FieldSpec{
            "texture_quality", FieldType::String,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const std::string value = v.node.get<std::string>();
                if (const auto quality = parse_texture_quality(value)) {
                    c.texture_quality = *quality;
                } else {
                    d.warn(std::format(
                        "WARN: texture_quality '{}' is unknown; retaining {}",
                        value, texture_quality_id(c.texture_quality)));
                }
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["texture_quality"] =
                    texture_quality_id(c.texture_quality);
            }});
        s.push_back(string_field("node_file", [](auto& c) -> auto& { return c.node_file; }));
        s.push_back(FieldSpec{
            "render_test_scene", FieldType::String,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const std::string value = v.node.get<std::string>();
                if (value.empty()) c.render_test_scene.clear();
                else if (!apply_render_test_scene_defaults(c, value)) {
                    d.warn(std::format("WARN: render_test_scene '{}' is unknown; retaining current scene", value));
                }
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["render_test_scene"] = c.render_test_scene;
            }});
        s.push_back(string_field("quality_preset_label",
                                 [](auto& c) -> auto& { return c.quality_preset_label; }));
        s.push_back(FieldSpec{
            "quality_preset", FieldType::String,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const std::string value = v.node.get<std::string>();
                if (const auto preset = parse_render_preset(value)) {
                    apply_render_preset(c, *preset);
                } else {
                    d.warn(std::format(
                        "WARN: quality_preset '{}' is unknown; retaining {}",
                        value, render_preset_id(c.render_preset)));
                }
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["quality_preset"] =
                    render_preset_id(c.render_preset);
            }});
        s.push_back(FieldSpec{
            "material_workflow", FieldType::String,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const std::string value = v.node.get<std::string>();
                if (const auto workflow = parse_material_workflow(value)) {
                    c.material_workflow = *workflow;
                } else {
                    d.warn(std::format(
                        "WARN: material_workflow '{}' is unknown; retaining {}",
                        value, material_workflow_id(c.material_workflow)));
                }
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["material_workflow"] =
                    material_workflow_id(c.material_workflow);
            }});
        s.push_back(int_field("width", [](auto& c) -> auto& { return c.width; }));
        s.push_back(int_field("height", [](auto& c) -> auto& { return c.height; }));
        s.push_back(bool_field("fullscreen", [](auto& c) -> auto& { return c.fullscreen; }));
        s.push_back(FieldSpec{
            "android_preview_fps", FieldType::Int,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const double raw = v.node.get<double>();
                if (std::isfinite(raw) && raw >= 0.0 && raw <= 240.0) {
                    c.android_preview_fps = static_cast<int>(raw);
                } else {
                    d.warn("WARN: android_preview_fps out of range [0,240]; retaining current value");
                }
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["android_preview_fps"] = c.android_preview_fps;
            }});
        s.push_back(FieldSpec{
            "android_render_scale", FieldType::Float,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const double raw = v.node.get<double>();
                if (std::isfinite(raw) && raw >= 0.125 && raw <= 1.0) {
                    c.android_render_scale = static_cast<float>(raw);
                } else {
                    d.warn("WARN: android_render_scale out of range [0.125,1]; retaining current value");
                }
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["android_render_scale"] = c.android_render_scale;
            }});
        s.push_back(bool_field("hdr", [](auto& c) -> auto& { return c.hdr; }));
        s.push_back(float_field("hdr_peak_nits", [](auto& c) -> auto& { return c.hdr_peak_nits; }));
        s.push_back(float_field("hdr_paper_white_nits",
                                [](auto& c) -> auto& { return c.hdr_paper_white_nits; }));
        s.push_back(float_field("cloth_sim_hz", [](auto& c) -> auto& { return c.cloth_sim_hz; }));
        s.push_back(FieldSpec{
            "bloom_strength", FieldType::Float,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const float raw = static_cast<float>(v.node.get<double>());
                const auto bounded = clamp_bloom_strength(raw);
                if (bounded.clamped) d.warn(std::format(
                    "WARN: bloom_strength '{}' out of range [0,1]; using {}",
                    raw, bounded.value));
                c.bloom_strength = bounded.value;
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["bloom_strength"] = c.bloom_strength;
            }});
        s.push_back(bool_field("wind_debug", [](auto& c) -> auto& { return c.wind_debug; }));
        s.push_back(bool_field("benchmark_mode", [](auto& c) -> auto& { return c.benchmark_mode; }));

        // time_of_day — wrapped into [0,24) via the shared wrap_24 helper the
        // CLI's --time-of-day uses (no diagnostic, matching the CLI which wraps
        // silently).
        s.push_back(FieldSpec{
            "time_of_day", FieldType::Float,
            [](const JsonValue& v, BenchConfig& c, Diagnostics&) {
                c.time_of_day = wrap_24(static_cast<float>(v.node.get<double>()));
            },
            [](const BenchConfig& c, JsonObject& o) { o.node["time_of_day"] = c.time_of_day; }});

        // heat_haze_enabled — keeps features.heat_haze in sync, exactly as
        // --no-heat-haze sets both. New code reads features.heat_haze; the
        // standalone field is retained for the pre-feature-framework call sites.
        s.push_back(FieldSpec{
            "heat_haze_enabled", FieldType::Bool,
            [](const JsonValue& v, BenchConfig& c, Diagnostics&) {
                const bool b = v.node.get<bool>();
                c.heat_haze_enabled  = b;
                c.features.heat_haze = b;
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["heat_haze_enabled"] = c.heat_haze_enabled;
            }});

        // ── Tessellation (no validation) ───────────────────────
        s.push_back(bool_field("tessellation.enabled",
                               [](auto& c) -> auto& { return c.tessellation.enabled; }));
        s.push_back(float_field("tessellation.scale",
                                [](auto& c) -> auto& { return c.tessellation.scale; }));
        s.push_back(float_field("tessellation.factor",
                                [](auto& c) -> auto& { return c.tessellation.factor; }));
        s.push_back(float_field("tessellation.distance",
                                [](auto& c) -> auto& { return c.tessellation.distance; }));
        s.push_back(string_field("tessellation.cache_file",
                                 [](auto& c) -> auto& { return c.tessellation.cache_file; }));

        // ── Feature toggles (no validation) ────────────────────
        // One bool key per FeatureToggles field EXCEPT heat_haze, which is
        // driven by the top-level heat_haze_enabled key above (the sole heat
        // haze flag, --no-heat-haze, maps there).
        s.push_back(bool_field("features.grass", [](auto& c) -> auto& { return c.features.grass; }));
        s.push_back(bool_field("features.particles",
                               [](auto& c) -> auto& { return c.features.particles; }));
        s.push_back(bool_field("features.god_rays",
                               [](auto& c) -> auto& { return c.features.god_rays; }));
        s.push_back(bool_field("features.volumetric_fog",
                               [](auto& c) -> auto& { return c.features.volumetric_fog; }));
        s.push_back(bool_field("features.clouds", [](auto& c) -> auto& { return c.features.clouds; }));
        s.push_back(bool_field("features.sky", [](auto& c) -> auto& { return c.features.sky; }));
        s.push_back(bool_field("features.lens_flares",
                               [](auto& c) -> auto& { return c.features.lens_flares; }));
        s.push_back(bool_field("features.rain", [](auto& c) -> auto& { return c.features.rain; }));
        s.push_back(bool_field("features.wet_reflections",
                               [](auto& c) -> auto& { return c.features.wet_reflections; }));
        s.push_back(bool_field("features.soft_shadows",
                               [](auto& c) -> auto& { return c.features.soft_shadows; }));
        s.push_back(bool_field("features.contact_shadows",
                               [](auto& c) -> auto& { return c.features.contact_shadows; }));
        s.push_back(bool_field("features.point_light_shadows",
                               [](auto& c) -> auto& { return c.features.point_light_shadows; }));
        s.push_back(bool_field("features.msaa", [](auto& c) -> auto& { return c.features.msaa; }));
        s.push_back(bool_field("features.refraction",
                               [](auto& c) -> auto& { return c.features.refraction; }));
        s.push_back(bool_field("features.reflections",
                               [](auto& c) -> auto& { return c.features.reflections; }));
        s.push_back(bool_field("features.volumetric_clouds",
                               [](auto& c) -> auto& { return c.features.volumetric_clouds; }));
        s.push_back(bool_field("features.ssgi", [](auto& c) -> auto& { return c.features.ssgi; }));
        s.push_back(bool_field("features.massive_foliage",
                               [](auto& c) -> auto& { return c.features.massive_foliage; }));
        s.push_back(bool_field("features.ground_relief",
                               [](auto& c) -> auto& { return c.features.ground_relief; }));
        s.push_back(bool_field("features.bloom", [](auto& c) -> auto& { return c.features.bloom; }));
        s.push_back(bool_field("features.ambient_occlusion",
                               [](auto& c) -> auto& { return c.features.ambient_occlusion; }));
        s.push_back(bool_field("features.taa", [](auto& c) -> auto& { return c.features.taa; }));
        s.push_back(bool_field("features.depth_of_field",
                               [](auto& c) -> auto& { return c.features.depth_of_field; }));

        // GPU instancing / skinning (gpu-instancing-and-skinning).
        // Both default ON and expose a `--no-<feature>` disable flag, so they are
        // config-file-mappable like the feature toggles above.
        s.push_back(bool_field("features.gpu_instancing",
                               [](auto& c) -> auto& { return c.features.gpu_instancing; }));
        s.push_back(bool_field("features.gpu_skinning",
                               [](auto& c) -> auto& { return c.features.gpu_skinning; }));

        // Advanced rendering capabilities default to ON.
        // These seven now default ON and expose a `--no-<feature>`
        // disable + `--<feature>` compat enable alias pair, so — like the alias
        // pairs above — they are config-file-mappable. The remaining three
        // capabilities (color_grading / physical_camera /
        // dynamic_resolution) stay opt-in and map to no config key.
        s.push_back(bool_field("features.clear_coat",
                               [](auto& c) -> auto& { return c.features.clear_coat; }));
        s.push_back(bool_field("features.anisotropic",
                               [](auto& c) -> auto& { return c.features.anisotropic; }));
        s.push_back(bool_field("features.subsurface",
                               [](auto& c) -> auto& { return c.features.subsurface; }));
        s.push_back(bool_field("features.cloth",
                               [](auto& c) -> auto& { return c.features.cloth; }));
        s.push_back(bool_field("features.transparent_shadows",
                               [](auto& c) -> auto& { return c.features.transparent_shadows; }));
        s.push_back(bool_field("features.colored_penumbra",
                               [](auto& c) -> auto& { return c.features.colored_penumbra; }));
        s.push_back(bool_field("features.specular_aa",
                               [](auto& c) -> auto& { return c.features.specular_aa; }));

        // ── Weather (validated) ────────────────────────────────
        // rain_intensity routes through parse_rain_intensity, the SAME helper
        // --rain-intensity uses: the numeric value is rendered to a token and
        // parsed so the [0,1] clamp semantics are byte-for-byte identical, and a
        // clamp emits the matching WARN text (key in place of the flag).
        s.push_back(FieldSpec{
            "weather.rain_intensity", FieldType::Float,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const double raw = v.node.get<double>();
                const std::string buf = std::format("{}", raw);
                const RainIntensityParse p = parse_rain_intensity(buf.c_str());
                if (p.clamped) {
                    d.warn(std::format("WARN: weather.rain_intensity '{}' out of [0,1]; clamped to {:.1f}",
                                       buf, p.value));
                }
                c.weather.rain_intensity = p.value;
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["weather.rain_intensity"] = c.weather.rain_intensity;
            }});

        // dry_down_seconds clamps to [0.5,30] via the shared
        // clamp_dry_down_seconds helper; like the CLI's --dry-down-seconds this
        // clamps silently (the helper reports no clamp flag), preserving parity.
        s.push_back(FieldSpec{
            "weather.dry_down_seconds", FieldType::Float,
            [](const JsonValue& v, BenchConfig& c, Diagnostics&) {
                c.weather.dry_down_seconds =
                    clamp_dry_down_seconds(static_cast<float>(v.node.get<double>()));
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["weather.dry_down_seconds"] = c.weather.dry_down_seconds;
            }});

        // ── Music (background-music-playback) ──────────────────
        // enabled — the --music/--no-music toggles both map here (no
        // validation). volume routes through parse_music_volume, the SAME helper
        // --music-volume uses: the numeric value is rendered to a token and
        // parsed so the [0,1] clamp semantics are byte-for-byte identical, and a
        // clamp emits the matching WARN text (key in place of the flag).
        s.push_back(bool_field("music.enabled", [](auto& c) -> auto& { return c.music.enabled; }));
        s.push_back(FieldSpec{
            "music.volume", FieldType::Float,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const double raw = v.node.get<double>();
                const std::string buf = std::format("{}", raw);
                const MusicVolumeParse p = parse_music_volume(buf.c_str());
                if (p.clamped) {
                    d.warn(std::format("WARN: music.volume '{}' out of [0,1]; clamped to {:.2f}",
                                       buf, p.value));
                }
                c.music.volume = p.value;
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["music.volume"] = c.music.volume;
            }});

        // ── Quality knobs (validated) ──────────────────────────
        s.push_back(quality_level_field("quality.soft_shadow_level",
                                        [](auto& c) -> auto& { return c.quality.soft_shadow_level; }));
        s.push_back(quality_level_field("quality.contact_shadow_level",
                                        [](auto& c) -> auto& { return c.quality.contact_shadow_level; }));
        s.push_back(quality_level_field("quality.cloud_quality",
                                        [](auto& c) -> auto& { return c.quality.cloud_quality; }));
        s.push_back(quality_level_field("quality.ssgi_quality",
                                        [](auto& c) -> auto& { return c.quality.ssgi_quality; }));
        s.push_back(quality_level_field("quality.ao_quality",
                                        [](auto& c) -> auto& { return c.quality.ao_quality; }));
        s.push_back(quality_level_field("quality.bloom_quality",
                                        [](auto& c) -> auto& { return c.quality.bloom_quality; }));
        s.push_back(quality_level_field("quality.dof_quality",
                                        [](auto& c) -> auto& { return c.quality.dof_quality; }));
        s.push_back(quality_level_field("quality.taa_quality",
                                        [](auto& c) -> auto& { return c.quality.taa_quality; }));
        s.push_back(int_field("quality.sun_shadow_map_size",
                              [](auto& c) -> auto& { return c.quality.sun_shadow_map_size; }));
        s.push_back(int_field("quality.point_shadow_map_size",
                              [](auto& c) -> auto& { return c.quality.point_shadow_map_size; }));
        s.push_back(bool_field("cpu_stress.enabled",
                               [](auto& c) -> auto& { return c.cpu_stress.enabled; }));
        s.push_back(int_field("cpu_stress.workers",
                              [](auto& c) -> auto& { return c.cpu_stress.workers; }));
        s.push_back(float_field("cpu_stress.duration_seconds",
                                [](auto& c) -> auto& { return c.cpu_stress.duration_seconds; }));

        // shadow_light_budget clamps to [0, shadow_light_budget_max] via the
        // shared helper (matching --point-light-quality).
        s.push_back(FieldSpec{
            "quality.shadow_light_budget", FieldType::Int,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const int raw = v.node.get<int>();
                const IntKnobClamp r = clamp_quality_shadow_light_budget(raw, c.quality);
                if (r.clamped) {
                    d.warn(std::format(
                        "WARN: quality.shadow_light_budget '{}' out of range [0,{}]; clamped to {}",
                        raw, std::max(0, c.quality.shadow_light_budget_max), r.value));
                }
                c.quality.shadow_light_budget = r.value;
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["quality.shadow_light_budget"] = c.quality.shadow_light_budget;
            }});

        // foliage_density clamps to [1.0, foliage_density_max] via the shared
        // helper (matching --foliage-quality; NaN collapses to the 1.0 baseline).
        s.push_back(FieldSpec{
            "quality.foliage_density", FieldType::Float,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const float raw = static_cast<float>(v.node.get<double>());
                const FloatKnobClamp r = clamp_quality_foliage_density(raw, c.quality);
                if (r.clamped) {
                    d.warn(std::format(
                        "WARN: quality.foliage_density '{}' out of range [1.0,{:.2f}]; clamped to {:.2f}",
                        raw, std::max(1.0f, c.quality.foliage_density_max), r.value));
                }
                c.quality.foliage_density = r.value;
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["quality.foliage_density"] = c.quality.foliage_density;
            }});

        // msaa_samples — the ONLY fixed-set Setting. apply_msaa_samples REJECTS
        // (does not clamp) an out-of-set value, retaining the prior value and
        // flagging the rejected request (matching --msaa-quality).

        // (clamp-before-fixed-set ordering): MSAA carries no
        // separate range clamp, so there is nothing to clamp ahead of the
        // fixed-set rule here. Were a future Setting subject to BOTH a range
        // clamp and a fixed-set rule, the range clamp MUST run first and the
        // fixed-set acceptance be evaluated against the already-clamped value.
        s.push_back(FieldSpec{
            "quality.msaa_samples", FieldType::Int,
            [](const JsonValue& v, BenchConfig& c, Diagnostics& d) {
                const int raw = v.node.get<int>();
                const MsaaApplyResult r = apply_msaa_samples(raw, c.quality.msaa_samples);
                if (r.rejected) {
                    d.warn(std::format(
                        "WARN: quality.msaa_samples '{}' (={}) is not in {{1,2,4}}; retaining {} sample(s)",
                        raw, r.rejected_value, r.value));
                }
                c.quality.msaa_samples = r.value;
            },
            [](const BenchConfig& c, JsonObject& o) {
                o.node["quality.msaa_samples"] = c.quality.msaa_samples;
            }});

        return s;
    }();
    return schema;
}

namespace {

// Human-readable name for a FieldType, used in the type-mismatch diagnostic so
// the message names both the expected and the actual JSON category.
const char* field_type_name(FieldType t) {
    switch (t) {
        case FieldType::Bool:             return "boolean";
        case FieldType::Int:              return "integer";
        case FieldType::Float:            return "number";
        case FieldType::String:           return "string";
        case FieldType::QualityLevelEnum: return "integer (quality level)";
    }
    return "value";
}

// Whether a parsed JSON value is acceptable for a FieldSpec of the given type.
// JSON `null` is rejected for ALL types: every nlohmann predicate below returns
// false for a null node, so a `null` value never satisfies any FieldType and is
// reported as a type mismatch. A Float field accepts either a
// JSON integer or float (is_number), so `18` binds to a float field.
bool json_compatible(const nlohmann::json& v, FieldType t) {
    switch (t) {
        case FieldType::Bool:             return v.is_boolean();
        case FieldType::Int:              return v.is_number_integer();
        case FieldType::Float:            return v.is_number();
        case FieldType::String:           return v.is_string();
        case FieldType::QualityLevelEnum: return v.is_number_integer();
    }
    return false;
}

// Index from canonical key → FieldSpec, built once over the static schema. The
// schema's string_view keys point at string literals with program lifetime, and
// the schema vector itself is static, so both the keys and the pointers stay
// valid for the life of the process.
const std::unordered_map<std::string_view, const FieldSpec*>& schema_index() {
    static const std::unordered_map<std::string_view, const FieldSpec*> index = [] {
        std::unordered_map<std::string_view, const FieldSpec*> m;
        for (const FieldSpec& spec : config_schema()) {
            m.emplace(spec.key, &spec);
        }
        return m;
    }();
    return index;
}

} // namespace

// load_config_file() — read, parse, and map a Config_File onto a copy of `base`
// The work happens on a working copy so any
// open/parse failure returns the untouched base (failure atomicity, Requirements
// The module never prints: every condition is appended to the
// returned diagnostics vector.
ConfigLoadResult load_config_file(const std::string& path, const BenchConfig& base) {
    ConfigLoadResult result;
    result.config    = base;
    result.parsed_ok = false;

    // 1. Open + read the file as UTF-8 bytes. On open failure
    //    name the path and the reason, leave config == base
    //    errno is captured immediately after the failed open.
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        const int err = errno;
        result.diagnostics.push_back(
            std::format("--config: cannot open '{}': {}", path, std::strerror(err)));
        return result;
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // 2. Reject empty / whitespace-only content as not well-formed BEFORE
    //    invoking the parser.
    const bool only_whitespace = std::all_of(content.begin(), content.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    if (only_whitespace) {
        result.diagnostics.push_back(std::format(
            "--config '{}': file is empty/whitespace-only; expected a JSON object", path));
        return result;
    }

    // 3. Parse via the Config_Library with exceptions caught at the module
    //    boundary. On a syntax error name the path and the library-reported
    //    location (byte offset + message) and leave config == base — no fields
    //    applied, even ones that appeared before the error

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(content);
    } catch (const nlohmann::json::parse_error& e) {
        result.diagnostics.push_back(std::format(
            "--config '{}': JSON parse error at byte {}: {}", path, e.byte, e.what()));
        return result;
    }

    // 4. Require a top-level object. A valid-JSON-but-non-object document (array,
    //    scalar, or null) is malformed for our schema and handled like a syntax
    //    error.
    if (!root.is_object()) {
        result.diagnostics.push_back(std::format(
            "--config '{}': expected a top-level JSON object but got {}", path, root.type_name()));
        return result;
    }

    // 5. Map each top-level member against the schema on a working copy of base,
    //    so the base is only superseded on a well-formed document. Collect
    //    per-key diagnostics in insertion order.
    BenchConfig working = base;
    Diagnostics diag;
    const auto& index = schema_index();

    // Presets are base layers, independent of JSON object iteration order.
    // Every explicit key below then overlays the preset.
    if (const auto it = root.find("quality_preset"); it != root.end()) {
        if (it->is_string()) {
            const std::string value = it->get<std::string>();
            if (const auto preset = parse_render_preset(value)) {
                apply_render_preset(working, *preset);
            } else {
                diag.warn(std::format(
                    "WARN: quality_preset '{}' is unknown; retaining {}",
                    value, render_preset_id(working.render_preset)));
            }
        } else {
            diag.warn(std::format(
                "--config '{}': key 'quality_preset' expected string but got {}; retaining current value",
                path, it->type_name()));
        }
    } else if (const auto legacy = root.find("quality_preset_label");
               legacy != root.end()) {
        if (legacy->is_string()) {
            const std::string value = legacy->get<std::string>();
            if (const auto preset = parse_render_preset(value)) {
                apply_render_preset(working, *preset);
            } else {
                diag.warn(std::format(
                    "WARN: deprecated quality_preset_label '{}' is unknown; retaining {}",
                    value, render_preset_id(working.render_preset)));
            }
        } else {
            diag.warn(std::format(
                "--config '{}': key 'quality_preset_label' expected string but got {}; retaining current value",
                path, legacy->type_name()));
        }
    }
    if (root.contains("quality_preset") &&
        root.contains("quality_preset_label") &&
        !root.at("quality_preset_label").is_string()) {
        diag.warn(std::format(
            "--config '{}': key 'quality_preset_label' expected string but got {}; retaining current value",
            path, root.at("quality_preset_label").type_name()));
    }

    // Scene isolation is a base layer. Explicit settings in the same file
    // always overlay it, independent of JSON key order.
    if (const auto it = root.find("render_test_scene"); it != root.end()) {
        if (it->is_string()) {
            index.at("render_test_scene")->load(JsonValue{*it}, working, diag);
        } else {
            diag.warn(std::format("--config '{}': key 'render_test_scene' expected string but got {}; retaining current scene",
                                  path, it->type_name()));
        }
    }
    for (const auto& [key, value] : root.items()) {
        if (key == "quality_preset" || key == "quality_preset_label" || key == "render_test_scene") continue;
        const auto it = index.find(key);
        if (it == index.end()) {
            // Unknown_Key: name it, change nothing, continue

            diag.warn(std::format("--config '{}': unknown key '{}' ignored", path, key));
            continue;
        }

        const FieldSpec* spec = it->second;
        if (!json_compatible(value, spec->type)) {
            // Type-incompatible (including JSON null): name the key and the
            // expected/actual mismatch, retain the current field value, continue

            diag.warn(std::format(
                "--config '{}': key '{}' expected {} but got {}; retaining current value",
                path, key, field_type_name(spec->type), value.type_name()));
            continue;
        }

        // Compatible: invoke the FieldSpec's shared validator, which applies the
        // same clamp/reject logic the CLI uses and writes the field (Requirements

        const JsonValue jv{value};
        spec->load(jv, working, diag);
    }

    working.quality_preset_label =
        std::string(render_preset_label(working.render_preset));

    // 6. A well-formed object is a parse success even if individual Settings were
    //    clamped, rejected, unknown, or type-incompatible
    //    Commit the working copy and the collected diagnostics.
    result.config      = working;
    result.diagnostics = std::move(diag.messages);
    result.parsed_ok   = true;
    return result;
}

// write_config_file() — serialize `cfg` to `path` as JSON, emitting one key for
// every Setting in config_schema(). Returns an empty
// diagnostics vector on success; on an open/write failure returns a single
// diagnostic naming the path and the failure with no other side effect
// The module never prints.
std::string serialize_config(const BenchConfig& cfg) {
    // 1. Build the output object by invoking FieldSpec.store for every schema
    //    entry, so every CLI-exposed Setting is emitted exactly once. Each store
    //    writes cfg's current value under its canonical key.
    //    By construction this yields only Recognized_Keys with in-range values,
    //    so the result reloads with zero diagnostics.
    nlohmann::json root = nlohmann::json::object();
    JsonObject out{root};
    for (const FieldSpec& spec : config_schema()) {
        if (spec.key == "quality_preset_label") continue;
        spec.store(cfg, out);
    }

    // 2. Serialize with the Config_Library's pretty-printer (2-space indent).
    //    Floating-point Settings round-trip exactly: nlohmann's serializer emits
    //    each double with the shortest decimal string that reparses to the
    //    identical IEEE-754 double. Our float fields are promoted to double by
    //    the store callbacks (json assignment), and the loader reads get<double>()
    //    and casts back to float. Since the SAME float→double value is what gets
    //    serialized, and the emitted text round-trips that double bit-for-bit, the
    //    reloaded double casts back to the identical float — so the default
    //    formatting already satisfies  with no hand-formatting.
    return root.dump(2);
}

std::vector<std::string> write_config_file(const std::string& path, const BenchConfig& cfg) {
    const std::string text = serialize_config(cfg);

    // 3. Open `path` for writing. On open failure capture errno immediately and
    //    return a single diagnostic naming the path and the reason, with no other
    //    side effect.
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os.is_open()) {
        const int err = errno;
        return {std::format("--write-config: cannot open '{}' for writing: {}", path,
                            std::strerror(err))};
    }

    // 4. Write the serialized text. If the stream goes bad during/after the
    //    write, trailing newline, or close, return a diagnostic naming the path and
    //    the write failure.
    os << text << '\n';
    os.close();
    if (!os.good()) {
        return {std::format("--write-config: failed to write configuration to '{}'", path)};
    }

    // 5. Success: no diagnostics.
    return {};
}

// Replay settings against the file/default base in their original order.
// The persistence schema intentionally omits some CLI-only settings, so copying
// schema fields from a CLI-over-default config loses valid renderer controls.
// Reusing the registered handlers also preserves rejected values from the file
// and applies presets at their actual command-line position.
void apply_cli_overlay(BenchConfig& cfg, const ParseResult& parse_result) {
    const auto& table = flag_table();
    Diagnostics replay_diagnostics;
    for (const auto& operation : parse_result.config_operations) {
        const auto it = std::find_if(table.begin(), table.end(),
            [&](const FlagSpec& spec) { return spec.name == operation.flag; });
        if (it != table.end() && it->apply) {
            it->apply(cfg, operation.value, replay_diagnostics);
        }
    }
}

} // namespace heaven
