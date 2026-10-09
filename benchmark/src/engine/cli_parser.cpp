/*
 * HeavenBench — CLI argument parser (implementation).
 *
 * added the pure parsing primitives (parse_int_strict /
 * parse_float_strict / wrap_24).  populated flag_table() with the
 * boolean toggles (Arity::None) and the seven backward-compatibility alias
 * pairs.  appends the value flags (Arity::Value): the strict-parsed
 * non-family numerics, the weather/quality handlers (which reuse the shared
 * engine/types.h helpers with the same atof-style leniency the legacy path
 * used), and the --api no-op.  implements the parse driver: an internal
 * build_flag_index() (hash map keyed by flag name) plus parse_cli(), which walks
 * argv resolving each token through that map.  implements
 * generate_help(): a one-to-one image of flag_table() formatted as
 * "  <name> <value_label>   <help>\n", one line per entry in table order.
 */
#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/render_presets.h"
#include "engine/tessellation_levels.h"
#include "engine/render_test_scene_catalog.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace heaven {

namespace {

// Shared by the four ordered-level quality flags (--soft-shadow-quality,
// --contact-shadow-quality, --cloud-quality, --ssgi-quality). Mirrors the
// `apply_level` lambda in apply_quality_args exactly: parse the integer level
// with atoi-style leniency (NOT strict parsing — junk → 0, matching the legacy
// quality path), clamp to {0,1,2} via the shared clamp_quality_level helper,
// push the same WARN text the legacy path printed on a clamp, and store the
// clamped level. The diagnostic is the legacy message minus its trailing
// newline (the main.cpp wrapper appends one per diagnostic line).
void apply_quality_level_flag(std::string_view flag, std::string_view value,
                              QualityLevel& dst, Diagnostics& diag) {
    const std::string buf(value);
    const int raw = parse_int_prefix(value).value_or(0);
    const QualityLevelClamp c = clamp_quality_level(raw);
    if (c.clamped) {
        diag.warn(std::format("WARN: {} '{}' out of range [0,2]; clamped to level {}",
                              flag, buf, static_cast<int>(c.value)));
    }
    dst = c.value;
}

// parse a comma-separated float vector for the multi-value grading
// flags (--white-balance, ASC CDL slope/offset/power). atof-style leniency
// matches the quality family — a non-numeric field collapses to 0.0 — and the
// caller validates the expected arity (2 for white balance, 3 for the CDL
// triples), reporting a malformed value when it does not match. Kept internal
// to this TU; the I/O-free numeric math itself stays in engine/types.h.
std::vector<float> parse_csv_floats(std::string_view value) {
    std::vector<float> out;
    const std::string buf(value);
    std::size_t pos = 0;
    while (true) {
        const std::size_t comma = buf.find(',', pos);
        const std::string field =
            (comma == std::string::npos) ? buf.substr(pos)
                                         : buf.substr(pos, comma - pos);
        out.push_back(parse_float_prefix(field).value_or(0.0f));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

bool is_capture_flag(std::string_view flag) {
    return flag == "--camera-pose" || flag == "--camera-tour-time" ||
           flag == "--camera-fov" || flag == "--capture" ||
           flag == "--capture-frames" || flag == "--capture-count" ||
           flag == "--capture-fps" || flag == "--profile" || flag == "--profile-seconds";
}

std::optional<std::array<float, 5>> parse_camera_pose(std::string_view value) {
    std::array<float, 5> pose{};
    for (std::size_t component = 0; component < pose.size(); ++component) {
        const auto separator = value.find(',');
        const bool last = component + 1 == pose.size();
        if (last != (separator == std::string_view::npos)) return std::nullopt;
        const auto number = parse_float_strict(value.substr(0, separator));
        if (!number) return std::nullopt;
        pose[component] = *number;
        if (!last) value.remove_prefix(separator + 1);
    }
    if (pose[4] < -89.0f || pose[4] > 89.0f) return std::nullopt;
    return pose;
}

void apply_capture_value(VisualCaptureOptions& capture, std::string_view flag,
                         std::string_view value, Diagnostics& diag) {
    if (flag == "--camera-pose") {
        if (const auto pose = parse_camera_pose(value)) {
            capture.camera_pose = pose;
            return;
        }
    } else if (flag == "--camera-tour-time") {
        if (const auto time = parse_float_strict(value); time && *time >= 0.0f) {
            capture.tour_time = *time;
            return;
        }
    } else if (flag == "--camera-fov") {
        if (const auto fov = parse_float_strict(value); fov && *fov >= 1.0f && *fov <= 170.0f) {
            capture.fov = *fov;
            capture.fov_explicit = true;
            return;
        }
    } else if (flag == "--capture-frames") {
        if (const auto frames = parse_int_strict(value); frames && *frames >= 1 && *frames <= 36000) {
            capture.frames = *frames;
            return;
        }
    } else if (flag == "--capture-count") {
        if (const auto count = parse_int_strict(value); count && *count >= 1 && *count <= 600) {
            capture.count = *count;
            return;
        }
    } else if (flag == "--capture-fps") {
        if (const auto fps = parse_float_strict(value); fps && *fps >= 1.0f && *fps <= 240.0f) {
            capture.fps = *fps;
            return;
        }
    } else if (flag == "--profile-seconds") {
        if (const auto seconds = parse_float_strict(value); seconds && *seconds >= 63.0f && *seconds <= 3600.0f) {
            capture.profile_seconds = *seconds;
            return;
        }
    } else if (flag == "--profile" && !value.empty() && !value.contains('\0')) {
        capture.profile_output = std::string(value);
        return;
    } else if (flag == "--capture" && !value.empty() && !value.contains('\0')) {
        capture.output = std::string(value);
        return;
    }
    capture.invalid = true;
    diag.warn(std::format("{}: invalid visual-audit value '{}' (see --help)", flag, value));
}

// Build the hash-based flag index from flag_table() keyed by FlagSpec::name for
// O(1) resolution. Values point into the table's stable static
// storage, so the map may outlive this call safely. Kept internal to this TU —
// it is an implementation detail of parse_cli, not part of the public surface.
std::unordered_map<std::string_view, const FlagSpec*> build_flag_index() {
    const std::vector<FlagSpec>& table = flag_table();
    std::unordered_map<std::string_view, const FlagSpec*> index;
    index.reserve(table.size());
    for (const FlagSpec& spec : table) {
        index.emplace(spec.name, &spec);
    }
    return index;
}

// Inverse of config_schema(): maps each BenchConfig-mapping flag name to the
// canonical Config_File schema key its `apply` writes (e.g. "--no-grass" →
// "features.grass"). This is the mapping that lets the parse driver record a
// `seen_key` whenever a persistent flag is applied. Ordered config_operations,
// not this persistence-only map, drive the complete CLI-over-file overlay.

// It is declared HERE (not in config_file.cpp) on purpose: cli_parser must not
// include config_file.h — that would create a header dependency cycle, since
// config_file.h already depends on cli_parser.h for BenchConfig. The trade-off
// is that this table and config_schema() must list the same key set; a coverage
// unit test  asserts the two sets are equal so they cannot silently
// drift. Every alias pair maps to the same key (the last occurrence on the
// command line wins). Flags that map to NO BenchConfig field
// are intentionally absent: --help (signaling only), --api (a no-op), and
// --config / --write-config (startup actions captured on ParseResult, not
// Settings).
const std::unordered_map<std::string_view, std::string_view>& flag_schema_keys() {
    static const std::unordered_map<std::string_view, std::string_view> m = {
        // Boolean toggles.
        {"--fullscreen",          "fullscreen"},
        {"--no-tessellation",     "tessellation.enabled"},
        {"--no-heat-haze",        "heat_haze_enabled"},
        {"--no-grass",            "features.grass"},
        {"--no-particles",        "features.particles"},
        {"--no-god-rays",         "features.god_rays"},
        {"--no-volumetric-fog",   "features.volumetric_fog"},
        {"--no-sky",              "features.sky"},
        {"--no-lens-flares",      "features.lens_flares"},
        {"--no-soft-shadows",     "features.soft_shadows"},
        {"--no-contact-shadows",  "features.contact_shadows"},
        {"--no-bloom",            "features.bloom"},
        {"--bloom",               "features.bloom"},
        {"--no-ambient-occlusion", "features.ambient_occlusion"},
        {"--ambient-occlusion",    "features.ambient_occlusion"},
        {"--no-taa",              "features.taa"},
        {"--taa",                 "features.taa"},
        {"--no-depth-of-field",   "features.depth_of_field"},
        {"--depth-of-field",      "features.depth_of_field"},
        {"--clouds",              "features.clouds"},
        {"--wind-debug",          "wind_debug"},
        {"--hdr",                 "hdr"},
        {"--no-rain",             "features.rain"},
        {"--no-wet-reflections",  "features.wet_reflections"},
        {"--no-gpu-instancing",   "features.gpu_instancing"},
        {"--no-gpu-skinning",     "features.gpu_skinning"},
        {"--benchmark",           "benchmark_mode"},
        {"--cpu-stress",          "cpu_stress.enabled"},
        {"--no-cpu-stress",       "cpu_stress.enabled"},

        // Music toggles — both members map to the same key (the last music
        // toggle on the command line wins).
        {"--music",               "music.enabled"},
        {"--no-music",            "music.enabled"},

        // Backward-compatibility alias pairs (both members → one key).
        {"--no-point-light-shadows", "features.point_light_shadows"},
        {"--point-light-shadows",    "features.point_light_shadows"},
        {"--no-msaa",                "features.msaa"},
        {"--msaa",                   "features.msaa"},
        {"--no-refraction",          "features.refraction"},
        {"--refraction",             "features.refraction"},
        {"--no-reflections",         "features.reflections"},
        {"--reflections",            "features.reflections"},
        {"--no-volumetric-clouds",   "features.volumetric_clouds"},
        {"--volumetric-clouds",      "features.volumetric_clouds"},
        {"--no-ssgi",                "features.ssgi"},
        {"--ssgi",                   "features.ssgi"},
        {"--no-massive-foliage",     "features.massive_foliage"},
        {"--massive-foliage",        "features.massive_foliage"},
        {"--no-ground-relief",       "features.ground_relief"},
        {"--ground-relief",          "features.ground_relief"},

        // default-on alias pairs (both members → one key). These seven
        // capabilities now default ON, so they are config-file-mappable like the
        // alias pairs above; the bare `--<feature>` compat enable and its
        // `--no-<feature>` disable map to the same canonical key.
        {"--no-clear-coat",          "features.clear_coat"},
        {"--clear-coat",             "features.clear_coat"},
        {"--no-anisotropic",         "features.anisotropic"},
        {"--anisotropic",            "features.anisotropic"},
        {"--no-subsurface",          "features.subsurface"},
        {"--subsurface",             "features.subsurface"},
        {"--no-cloth",               "features.cloth"},
        {"--cloth",                  "features.cloth"},
        {"--no-transparent-shadows", "features.transparent_shadows"},
        {"--transparent-shadows",    "features.transparent_shadows"},
        {"--no-colored-penumbra",    "features.colored_penumbra"},
        {"--colored-penumbra",       "features.colored_penumbra"},
        {"--no-specular-aa",         "features.specular_aa"},
        {"--specular-aa",            "features.specular_aa"},

        // Value flags.
        {"--data",                  "data_dir"},
        {"--texture-quality",       "texture_quality"},
        {"--node",                  "node_file"},
        {"--test-scene",            "render_test_scene"},
        {"--quality-preset",        "quality_preset_label"},
        {"--preset",                "quality_preset"},
        {"--material-workflow",     "material_workflow"},
        {"--width",                 "width"},
        {"--height",                "height"},
        {"--cloth-rate",            "cloth_sim_hz"},
        {"--tess-scale",            "tessellation.scale"},
        {"--tess-level",            "tessellation.enabled"},
        {"--tess-factor",           "tessellation.factor"},
        {"--tess-distance",         "tessellation.distance"},
        {"--tess-cache",            "tessellation.cache_file"},
        {"--time-of-day",           "time_of_day"},
        {"--rain-intensity",        "weather.rain_intensity"},
        {"--dry-down-seconds",      "weather.dry_down_seconds"},
        {"--music-volume",          "music.volume"},
        {"--soft-shadow-quality",   "quality.soft_shadow_level"},
        {"--contact-shadow-quality","quality.contact_shadow_level"},
        {"--cloud-quality",         "quality.cloud_quality"},
        {"--ssgi-quality",          "quality.ssgi_quality"},
        {"--point-light-quality",   "quality.shadow_light_budget"},
        {"--foliage-quality",       "quality.foliage_density"},
        {"--msaa-quality",          "quality.msaa_samples"},
        {"--ao-quality",            "quality.ao_quality"},
        {"--bloom-quality",         "quality.bloom_quality"},
        {"--bloom-strength",        "bloom_strength"},
        {"--dof-quality",           "quality.dof_quality"},
        {"--taa-quality",           "quality.taa_quality"},
        {"--sun-shadow-map",        "quality.sun_shadow_map_size"},
        {"--point-shadow-map",      "quality.point_shadow_map_size"},
        {"--cpu-stress-workers",    "cpu_stress.workers"},
        {"--cpu-stress-seconds",    "cpu_stress.duration_seconds"},
        {"--hdr-nits",              "hdr_peak_nits"},
        {"--hdr-paper-white",       "hdr_paper_white_nits"},
    };
    return m;
}

// Record the canonical schema key for `flag` into `seen` if the flag maps to a
// BenchConfig field. Called by the parse driver immediately after a flag's
// `apply` runs, so seen_keys reflects exactly the Settings the command line set.
void record_seen_key(std::string_view flag, std::set<std::string>& seen) {
    if (flag == "--tess-level") {
        for (const auto key : {"tessellation.enabled", "tessellation.scale",
                               "tessellation.factor", "tessellation.distance"})
            seen.emplace(key);
        return;
    }
    const auto& m = flag_schema_keys();
    if (const auto it = m.find(flag); it != m.end()) {
        seen.emplace(it->second);
    }
}

} // namespace

// ── Pure parsing primitives ─────────────────────────────────────

// Strict full-token numeric parsing, mirroring parse_rain_intensity's
// end-pointer validation: std::strto* must advance past the start (so the token
// is not empty/garbage) and stop exactly at the terminating NUL (so there is no
// trailing junk like "1280px"). std::string_view is not guaranteed
// NUL-terminated, so we materialize a std::string to obtain a valid C string
// for std::strtol/std::strtod.

float wrap_24(float t) {
    while (t < 0.0f)   t += 24.0f;
    while (t >= 24.0f) t -= 24.0f;
    return t;
}

// ── The single registration table ──────────────────────────────

// populated the boolean toggles (Arity::None) and the seven
// backward-compatibility alias pairs.  appends the value flags
// (Arity::Value): the strict-parsed non-family numerics, the weather/quality
// handlers reusing the shared engine/types.h helpers, and the --api no-op. The
// parse driver and help generator arrive in.x. Each entry sets its
// declared target(s) in a small apply lambda; the `value` argument is empty for
// toggles and ignored. --help carries no apply (parse_cli detects it specially
// and sets help_requested), but it lives in the table so the generated help
// lists it.
const std::vector<FlagSpec>& flag_table() {
    static const std::vector<FlagSpec> table = [] {
        std::vector<FlagSpec> t;

        // ── Boolean toggles (Arity::None) ──────────────────────
        t.push_back({"--fullscreen", Arity::None, "", "Fullscreen mode",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.fullscreen = true; }});
        t.push_back({"--no-tessellation", Arity::None, "", "Disable CPU tessellation entirely",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.tessellation.enabled = false; }});
        t.push_back({"--no-heat-haze", Arity::None, "",
            "Disable chimney heat-haze refraction (saves a fullscreen pass)",
            [](BenchConfig& c, std::string_view, Diagnostics&) {
                c.heat_haze_enabled = false;
                c.features.heat_haze = false;
            }});
        t.push_back({"--no-grass", Arity::None, "", "Disable procedural grass",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.grass = false; }});
        t.push_back({"--no-particles", Arity::None, "", "Disable smoke/fire/glow particle effects",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.particles = false; }});
        t.push_back({"--no-god-rays", Arity::None, "", "Disable volumetric light shafts",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.god_rays = false; }});
        t.push_back({"--no-volumetric-fog", Arity::None, "", "Disable per-light volumetric fog",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.volumetric_fog = false; }});
        t.push_back({"--no-sky", Arity::None, "", "Disable the procedural sky sphere",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.sky = false; }});
        t.push_back({"--no-lens-flares", Arity::None, "", "Disable lamp/torch lens flares",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.lens_flares = false; }});
        t.push_back({"--no-soft-shadows", Arity::None, "", "Disable PCSS/DPCF soft directional shadows",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.soft_shadows = false; }});
        t.push_back({"--no-contact-shadows", Arity::None, "", "Disable screen-space contact shadows",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.contact_shadows = false; }});
        t.push_back({"--no-bloom", Arity::None, "", "Disable bloom and lens dirt",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.bloom = false; }});
        t.push_back({"--bloom", Arity::None, "", "Enable bloom and lens dirt",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.bloom = true; }});
        t.push_back({"--no-ambient-occlusion", Arity::None, "", "Disable screen-space ambient occlusion",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.ambient_occlusion = false; }});
        t.push_back({"--ambient-occlusion", Arity::None, "", "Enable screen-space ambient occlusion",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.ambient_occlusion = true; }});
        t.push_back({"--no-taa", Arity::None, "", "Disable temporal anti-aliasing",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.taa = false; }});
        t.push_back({"--taa", Arity::None, "", "Enable temporal anti-aliasing",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.taa = true; }});
        t.push_back({"--no-depth-of-field", Arity::None, "", "Disable authored and physical-camera depth of field",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.depth_of_field = false; }});
        t.push_back({"--depth-of-field", Arity::None, "", "Allow authored and physical-camera depth of field",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.depth_of_field = true; }});
        t.push_back({"--clouds", Arity::None, "", "Enable the (dormant) cloud layer",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.clouds = true; }});
        t.push_back({"--wind-debug", Arity::None, "",
            "Render world-space wind direction arrows at each flag",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.wind_debug = true; }});
        t.push_back({"--hdr", Arity::None, "",
            "HDR10 (PQ/Rec.2020) display output; requires a Wayland HDR session",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.hdr = true; }});
        t.push_back({"--no-rain", Arity::None, "", "Disable falling rain / splashes / wetness",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.rain = false; }});
        t.push_back({"--no-wet-reflections", Arity::None, "",
            "Disable wet-surface screen-space reflections",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.wet_reflections = false; }});

        // ── GPU instancing / skinning toggles (Arity::None) ────

        // Both default ON. --no-gpu-instancing falls back to the
        // one-renderable-per-instance path; --no-gpu-skinning falls back to the
        // CPU linear-blend skinning + vertex re-upload path. The all-disabled
        // configuration is byte-parity with the Shipping_Look.
        t.push_back({"--no-gpu-instancing", Arity::None, "",
            "Disable GPU instancing of repeated static meshes",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.gpu_instancing = false; }});
        t.push_back({"--no-gpu-skinning", Arity::None, "",
            "Disable GPU skinning of animated meshes (use CPU skinning)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.gpu_skinning = false; }});

        // ── Music toggles (Arity::None) ────────────────────────

        // Background music defaults ON. --music explicitly
        // enables it (provided for symmetry and to override a config file,
        // --no-music disables it for the whole run
        // Both map to the same config.music.enabled field, so
        // because the parse driver applies flags left to right the LAST of the
        // two on the command line wins with no special casing.
        t.push_back({"--music", Arity::None, "", "Enable background music (default)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.music.enabled = true; }});
        t.push_back({"--no-music", Arity::None, "", "Disable background music for the run",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.music.enabled = false; }});
        t.push_back({"--benchmark", Arity::None, "", "Run benchmark with camera path playback",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.benchmark_mode = true; }});
        // --help has no apply: parse_cli detects it specially and sets
        // help_requested (see the current "Help handling"). It lives in the
        // table only so generate_help lists it.
        t.push_back({"--help", Arity::None, "", "Show this help listing and exit", nullptr});

        // ── Backward-compatibility alias pairs ─────────────────

        // Each feature that now defaults ON exposes a `--no-<name>` disable
        // toggle and a bare `--<name>` enable alias (retained for scripts
        // written when the feature defaulted off). The last occurrence on the
        // command line wins.
        t.push_back({"--no-point-light-shadows", Arity::None, "",
            "Disable shadow-casting point/spot lights",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.point_light_shadows = false; }});
        t.push_back({"--point-light-shadows", Arity::None, "",
            "Enable shadow-casting point/spot lights (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.point_light_shadows = true; }});
        t.push_back({"--no-msaa", Arity::None, "", "Disable MSAA on top of TAA",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.msaa = false; }});
        t.push_back({"--msaa", Arity::None, "", "Enable MSAA on top of TAA (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.msaa = true; }});
        t.push_back({"--no-refraction", Arity::None, "", "Disable screen-space glass refraction",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.refraction = false; }});
        t.push_back({"--refraction", Arity::None, "",
            "Enable screen-space glass refraction (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.refraction = true; }});
        t.push_back({"--no-reflections", Arity::None, "",
            "Disable screen-space reflections (SSR) — for testing",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.reflections = false; }});
        t.push_back({"--reflections", Arity::None, "",
            "Enable screen-space reflections (SSR) (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.reflections = true; }});
        t.push_back({"--no-volumetric-clouds", Arity::None, "", "Disable raymarched volumetric clouds",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.volumetric_clouds = false; }});
        t.push_back({"--volumetric-clouds", Arity::None, "",
            "Enable raymarched volumetric clouds (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.volumetric_clouds = true; }});
        t.push_back({"--no-ssgi", Arity::None, "", "Disable screen-space global illumination",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.ssgi = false; }});
        t.push_back({"--ssgi", Arity::None, "",
            "Enable screen-space global illumination (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.ssgi = true; }});
        t.push_back({"--no-massive-foliage", Arity::None, "",
            "Disable massive foliage / GPU-driven geometry",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.massive_foliage = false; }});
        t.push_back({"--massive-foliage", Arity::None, "",
            "Enable massive foliage / GPU-driven geometry (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.massive_foliage = true; }});
        t.push_back({"--no-ground-relief", Arity::None, "",
            "Disable generated relief on village gravel",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.ground_relief = false; }});
        t.push_back({"--ground-relief", Arity::None, "",
            "Enable generated relief on village gravel (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.ground_relief = true; }});

        // ──  default-on capabilities (Arity::None alias pairs) ──

        // advanced-materials-and-grading capabilities. By
        // product decision these seven now default ON (best visual look), so —
        // following the alias-pair precedent above (--no-msaa/--msaa, etc.) —
        // each exposes a `--no-<feature>` DISABLE flag plus a bare `--<feature>`
        // compat ENABLE alias retained for scripts written when the feature
        // defaulted off. Both members are Arity::None and set the same
        // FeatureToggles field (--no-x → false, --x → true); the last
        // occurrence on the command line wins. They set their
        // field through the table's own lambda — the single source of truth
        // shared by parse_cli and the toggle tests.
        t.push_back({"--no-clear-coat", Arity::None, "",
            "Disable clear-coat shading lobe on coated surfaces",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.clear_coat = false; }});
        t.push_back({"--clear-coat", Arity::None, "",
            "Enable clear-coat shading lobe on coated surfaces (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.clear_coat = true; }});
        t.push_back({"--no-anisotropic", Arity::None, "",
            "Disable anisotropic (brushed-metal) specular shading",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.anisotropic = false; }});
        t.push_back({"--anisotropic", Arity::None, "",
            "Enable anisotropic (brushed-metal) specular shading (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.anisotropic = true; }});
        t.push_back({"--no-subsurface", Arity::None, "",
            "Disable subsurface / translucency shading on thin organic surfaces",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.subsurface = false; }});
        t.push_back({"--subsurface", Arity::None, "",
            "Enable subsurface / translucency shading on thin organic surfaces (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.subsurface = true; }});
        t.push_back({"--no-cloth", Arity::None, "",
            "Disable cloth / sheen shading on fabric surfaces",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.cloth = false; }});
        t.push_back({"--cloth", Arity::None, "",
            "Enable cloth / sheen shading on fabric surfaces (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.cloth = true; }});
        t.push_back({"--no-transparent-shadows", Arity::None, "",
            "Disable transparent/tinted foliage shadows (coverage-modulated)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.transparent_shadows = false; }});
        t.push_back({"--transparent-shadows", Arity::None, "",
            "Enable transparent/tinted foliage shadows (coverage-modulated) (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.transparent_shadows = true; }});
        t.push_back({"--no-colored-penumbra", Arity::None, "",
            "Disable colored soft-shadow penumbra",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.colored_penumbra = false; }});
        t.push_back({"--colored-penumbra", Arity::None, "",
            "Enable colored soft-shadow penumbra (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.colored_penumbra = true; }});
        t.push_back({"--no-specular-aa", Arity::None, "",
            "Disable geometric specular anti-aliasing",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.specular_aa = false; }});
        t.push_back({"--specular-aa", Arity::None, "",
            "Enable geometric specular anti-aliasing (reduces highlight shimmer) (compat alias)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.specular_aa = true; }});

        // ──  opt-in enable toggles (Arity::None) ────────

        // The remaining three  capabilities  stay
        // default OFF, so each is a bare `--<feature>` enable flag that flips
        // exactly its FeatureToggles bool true. Disable forms also permit
        // isolated fixtures to compare each path after their scene defaults.
        t.push_back({"--color-grading", Arity::None, "",
            "Enable the full color-grading chain (white balance, CDL, contrast, ...)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.color_grading = true; }});
        t.push_back({"--no-color-grading", Arity::None, "", "Disable the full color-grading chain",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.color_grading = false; }});
        t.push_back({"--physical-camera", Arity::None, "",
            "Enable the physically-based camera (aperture/shutter/ISO → exposure + DOF)",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.physical_camera = true; }});
        t.push_back({"--no-physical-camera", Arity::None, "", "Disable physical camera exposure and DOF",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.physical_camera = false; }});
        t.push_back({"--dynamic-resolution", Arity::None, "",
            "Enable dynamic resolution with FSR upscaling",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.dynamic_resolution = true; }});
        t.push_back({"--no-dynamic-resolution", Arity::None, "", "Disable dynamic resolution and FSR upscaling",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.features.dynamic_resolution = false; }});
        t.push_back({"--cpu-stress", Arity::None, "",
            "Enable the independent all-core real-scene CPU stress phase",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.cpu_stress.enabled = true; }});
        t.push_back({"--no-cpu-stress", Arity::None, "",
            "Disable the independent all-core real-scene CPU stress phase",
            [](BenchConfig& c, std::string_view, Diagnostics&) { c.cpu_stress.enabled = false; }});

        // ── Value flags (Arity::Value) ─────────────────────────

        // Two parsing disciplines coexist here, matching the legacy parser:

        //   • Non-family numeric flags (--width/--height ints; --cloth-rate,
        //     --tess-*, --hdr-nits, --hdr-paper-white, --time-of-day floats) use
        //     the STRICT parsers (parse_int_strict / parse_float_strict). A
        //     well-formed token parses identically to the legacy atoi/atof; a
        //     malformed token is REPORTED and the field is LEFT UNCHANGED
        //     instead of silently becoming 0.
        //   • Weather/quality family flags reuse the SAME shared pure helpers
        //     apply_weather_args / apply_quality_args call (parse_rain_intensity,
        //     clamp_dry_down_seconds, clamp_quality_level,
        //     clamp_quality_shadow_light_budget, clamp_quality_foliage_density,
        //     apply_msaa_samples) with the SAME atof-style leniency, so the
        //     parser's weather/quality results equal those wrappers' by
        //     construction. The clamp/ignore/reject
        //     diagnostics reproduce the legacy WARN text (minus the trailing
        //     newline the main.cpp wrapper appends).

        // Path / string flags — assigned verbatim.
        t.push_back({"--data", Arity::Value, "PATH", "Loose asset tree, ZIP, or ZIP folder (default: assets)",
            [](BenchConfig& c, std::string_view v, Diagnostics&) { c.data_dir = std::string(v); }});
        t.push_back({"--texture-quality", Arity::Value, "original|2k|4k",
            "Texture tier: exact originals, high-resolution 2K, or ultra 4K",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (const auto quality = parse_texture_quality(v)) {
                    c.texture_quality = *quality;
                } else {
                    d.warn(std::format(
                        "--texture-quality: unknown tier '{}'; retaining {}",
                        v, texture_quality_id(c.texture_quality)));
                }
            }});
        t.push_back({"--node", Arity::Value, "FILE", "Load specific .node file",
            [](BenchConfig& c, std::string_view v, Diagnostics&) { c.node_file = std::string(v); }});
        t.push_back({"--test-scene", Arity::Value, "ID",
            "Load a small procedural or Heaven-asset rendering fixture (see --list-test-scenes); later flags override its defaults",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (!apply_render_test_scene_defaults(c, v)) {
                    d.warn(std::format("--test-scene: unknown scene '{}'; see --list-test-scenes", v));
                }
            }});
        t.push_back({"--list-test-scenes", Arity::None, "",
            "List rendering fixtures and exit (asset-* scenes require Heaven assets)", {}});
        t.push_back({"--quality-preset", Arity::Value, "NAME",
            "Deprecated preset alias; use --preset",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (const auto preset = parse_render_preset(v)) {
                    apply_render_preset(c, *preset);
                } else {
                    d.warn(std::format(
                        "--quality-preset: unknown preset '{}'; retaining {}",
                        v, render_preset_id(c.render_preset)));
                }
            }});
        t.push_back({"--preset", Arity::Value, "NAME",
            "Rendering preset: balanced, visual-ultra, gpu-torture, cpu-torture",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (const auto preset = parse_render_preset(v)) {
                    apply_render_preset(c, *preset);
                } else {
                    d.warn(std::format("--preset: unknown preset '{}'; retaining {}",
                                       v, render_preset_id(c.render_preset)));
                }
            }});
        t.push_back({"--material-workflow", Arity::Value, "NAME",
            "Material workflow: metallic-roughness or specular-glossiness",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (const auto workflow = parse_material_workflow(v)) {
                    c.material_workflow = *workflow;
                } else {
                    d.warn(std::format(
                        "--material-workflow: unknown workflow '{}'; retaining {}",
                        v, material_workflow_id(c.material_workflow)));
                }
            }});
        t.push_back({"--tess-cache", Arity::Value, "FILE.zip",
            "Override the writable tessellation cache archive",
            [](BenchConfig& c, std::string_view v, Diagnostics&) {
                c.tessellation.cache_file = std::string(v);
            }});

        // Window size — strict integers.
        t.push_back({"--width", Arity::Value, "W", "Window width (default: 1920)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (auto n = parse_int_strict(v)) c.width = *n;
                else d.warn(std::format("--width: ignoring non-integer value '{}'", v));
            }});
        t.push_back({"--height", Arity::Value, "H", "Window height (default: 1080)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (auto n = parse_int_strict(v)) c.height = *n;
                else d.warn(std::format("--height: ignoring non-integer value '{}'", v));
            }});

        // Cloth simulation rate — strict float.
        t.push_back({"--cloth-rate", Arity::Value, "HZ",
            "Cloth simulation rate (default 60). Set low (e.g. 1) to slow-mo cloth debug",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (auto f = parse_float_strict(v)) c.cloth_sim_hz = *f;
                else d.warn(std::format("--cloth-rate: ignoring non-number value '{}'", v));
            }});

        // Named levels override the rendering preset's base values at their
        // actual command-line position. Manual controls remain strict floats.
        t.push_back({"--tess-level", Arity::Value, "LEVEL",
            "Tessellation: disabled / moderate (0.75x) / normal (1x) / extreme (1.5x)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (const auto level = parse_tessellation_level(v))
                    apply_tessellation_level(c.tessellation, *level);
                else d.warn(std::format(
                    "--tess-level: unknown level '{}'; use disabled, moderate, normal or extreme", v));
            }});
        t.push_back({"--tess-scale", Arity::Value, "F", "Displacement amount multiplier (default: 1)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (auto f = parse_float_strict(v)) c.tessellation.scale = *f;
                else d.warn(std::format("--tess-scale: ignoring non-number value '{}'", v));
            }});
        t.push_back({"--tess-factor", Arity::Value, "F", "Subdivision factor (default: 1)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (auto f = parse_float_strict(v)) c.tessellation.factor = *f;
                else d.warn(std::format("--tess-factor: ignoring non-number value '{}'", v));
            }});
        t.push_back({"--tess-distance", Arity::Value, "F", "Authored distance multiplier (default: 1)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (auto f = parse_float_strict(v)) c.tessellation.distance = *f;
                else d.warn(std::format("--tess-distance: ignoring non-number value '{}'", v));
            }});

        // Time of day — strict float, then wrapped into [0, 24).
        t.push_back({"--time-of-day", Arity::Value, "H", "Hour of day, 0..24 (default: 18.0 = sunset)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (auto t = parse_float_strict(v)) c.time_of_day = wrap_24(*t);
                else d.warn(std::format("--time-of-day: ignoring non-number value '{}'", v));
            }});

        // ── Weather family (shared helpers, atof-style leniency) ─
        t.push_back({"--rain-intensity", Arity::Value, "V",
            "Initial rain intensity, 0..1 (default: 0.5 = medium rain)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const RainIntensityParse parsed = parse_rain_intensity(buf.c_str());
                if (parsed.ignored)
                    d.warn(std::format("WARN: --rain-intensity '{}' is not numeric; using default 0.0", buf));
                else if (parsed.clamped)
                    d.warn(std::format("WARN: --rain-intensity '{}' out of [0,1]; clamped to {:.1f}",
                                       buf, parsed.value));
                c.weather.rain_intensity = parsed.value;
            }});
        t.push_back({"--dry-down-seconds", Arity::Value, "S",
            "Wetness dry-down duration, 0.5..30 (default: 5.0)",
            [](BenchConfig& c, std::string_view v, Diagnostics&) {
                c.weather.dry_down_seconds =
                    clamp_dry_down_seconds(parse_float_prefix(v).value_or(0.0f));
            }});

        // ── Music volume (shared helper, owns only diagnostics) ─

        // Delegates all numeric parsing, clamping, and rejection to the pure
        // parse_music_volume helper (engine/types.h, beside parse_rain_intensity)
        // and owns only the diagnostic text, mirroring --rain-intensity: a
        // non-numeric token is `ignored` (retains the default 1.0), a
        // numeric token outside [0,1] is `clamped` to the nearest bound (Req
        // and an in-range number is stored verbatim. The missing
        // value case  is handled by the generic value-flag driver,
        // which emits "expected a value" and leaves the field at its default 1.0.
        t.push_back({"--music-volume", Arity::Value, "V",
            "Music volume, 0..1 (default: 1.0)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const MusicVolumeParse parsed = parse_music_volume(buf.c_str());
                if (parsed.ignored)
                    d.warn(std::format("WARN: --music-volume '{}' is not a number; using default {:.1f}",
                                       buf, parsed.value));
                else if (parsed.clamped)
                    d.warn(std::format("WARN: --music-volume '{}' out of [0,1]; clamped to {:.2f}",
                                       buf, parsed.value));
                c.music.volume = parsed.value;
            }});

        // ── Quality family (shared helpers, atof-style leniency) ─
        t.push_back({"--soft-shadow-quality", Arity::Value, "L", "Soft-shadow level 0..2 (default: 1)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                apply_quality_level_flag("--soft-shadow-quality", v, c.quality.soft_shadow_level, d);
            }});
        t.push_back({"--contact-shadow-quality", Arity::Value, "L", "Contact-shadow level 0..2 (default: 1)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                apply_quality_level_flag("--contact-shadow-quality", v, c.quality.contact_shadow_level, d);
            }});
        t.push_back({"--cloud-quality", Arity::Value, "L", "Volumetric-cloud level 0..2 (default: 1)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                apply_quality_level_flag("--cloud-quality", v, c.quality.cloud_quality, d);
            }});
        t.push_back({"--ssgi-quality", Arity::Value, "L", "SSGI level 0..2 (default: 1)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                apply_quality_level_flag("--ssgi-quality", v, c.quality.ssgi_quality, d);
            }});
        t.push_back({"--point-light-quality", Arity::Value, "N",
            "Shadow-casting light budget 0..16 (default: 2)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const IntKnobClamp r = clamp_quality_shadow_light_budget(
                    parse_int_prefix(v).value_or(0), c.quality);
                if (r.clamped)
                    d.warn(std::format("WARN: --point-light-quality '{}' out of range [0,{}]; clamped to {}",
                                       buf, std::max(0, c.quality.shadow_light_budget_max), r.value));
                c.quality.shadow_light_budget = r.value;
            }});
        t.push_back({"--foliage-quality", Arity::Value, "F",
            "Foliage density multiplier >=1.0 (default: 1.0)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const FloatKnobClamp r = clamp_quality_foliage_density(
                    parse_float_prefix(v).value_or(0.0f), c.quality);
                if (r.clamped)
                    d.warn(std::format("WARN: --foliage-quality '{}' out of range [1.0,{:.2f}]; clamped to {:.2f}",
                                       buf, std::max(1.0f, c.quality.foliage_density_max), r.value));
                c.quality.foliage_density = r.value;
            }});
        t.push_back({"--msaa-quality", Arity::Value, "N", "MSAA sample count in {1,2,4} (default: 1)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const MsaaApplyResult r = apply_msaa_samples(
                    parse_int_prefix(v).value_or(0), c.quality.msaa_samples);
                if (r.rejected)
                    d.warn(std::format("WARN: --msaa-quality '{}' (={}) is not in {{1,2,4}}; retaining {} sample(s)",
                                       buf, r.rejected_value, r.value));
                c.quality.msaa_samples = r.value;
            }});
        t.push_back({"--ao-quality", Arity::Value, "L", "GTAO quality 0..2",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                apply_quality_level_flag("--ao-quality", v, c.quality.ao_quality, d);
            }});
        t.push_back({"--bloom-quality", Arity::Value, "L", "Bloom quality 0..2",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                apply_quality_level_flag("--bloom-quality", v, c.quality.bloom_quality, d);
            }});
        t.push_back({"--bloom-strength", Arity::Value, "F",
            "Requested bloom strength 0..1 (default: 0.3), scaled by time of day",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const auto raw = parse_float_strict(v);
                if (!raw) {
                    d.warn(std::format("--bloom-strength: ignoring non-number value '{}'", v));
                    return;
                }
                const auto bounded = clamp_bloom_strength(*raw);
                if (bounded.clamped) d.warn(std::format(
                    "WARN: --bloom-strength '{}' out of range [0,1]; using {}",
                    v, bounded.value));
                c.bloom_strength = bounded.value;
            }});
        t.push_back({"--dof-quality", Arity::Value, "L", "Depth-of-field quality 0..2",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                apply_quality_level_flag("--dof-quality", v, c.quality.dof_quality, d);
            }});
        t.push_back({"--taa-quality", Arity::Value, "L", "TAA quality 0..2",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                apply_quality_level_flag("--taa-quality", v, c.quality.taa_quality, d);
            }});
        t.push_back({"--sun-shadow-map", Arity::Value, "N",
            "Directional shadow map size (1024, 2048, or 4096)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const auto n = parse_int_strict(v);
                if (n && (*n == 1024 || *n == 2048 || *n == 4096))
                    c.quality.sun_shadow_map_size = *n;
                else d.warn(std::format(
                    "--sun-shadow-map: '{}' is not one of 1024,2048,4096", v));
            }});
        t.push_back({"--point-shadow-map", Arity::Value, "N",
            "Point-light cube shadow map size (256, 512, or 1024)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const auto n = parse_int_strict(v);
                if (n && (*n == 256 || *n == 512 || *n == 1024))
                    c.quality.point_shadow_map_size = *n;
                else d.warn(std::format(
                    "--point-shadow-map: '{}' is not one of 256,512,1024", v));
            }});
        t.push_back({"--cpu-stress-workers", Arity::Value, "N",
            "CPU stress workers (0 = hardware concurrency, maximum 256)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (const auto n = parse_int_strict(v))
                    c.cpu_stress.workers = std::clamp(*n, 0, 256);
                else d.warn(std::format(
                    "--cpu-stress-workers: ignoring non-integer '{}'", v));
            }});
        t.push_back({"--cpu-stress-seconds", Arity::Value, "S",
            "CPU stress duration in seconds",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (const auto seconds = parse_float_strict(v);
                    seconds && *seconds > 0.0f)
                    c.cpu_stress.duration_seconds = *seconds;
                else d.warn(std::format(
                    "--cpu-stress-seconds: expected a positive number, got '{}'", v));
            }});

        // HDR display output tuning — strict floats.
        t.push_back({"--hdr-nits", Arity::Value, "N", "Peak display luminance for HDR output (default: 1000)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (auto f = parse_float_strict(v)) c.hdr_peak_nits = *f;
                else d.warn(std::format("--hdr-nits: ignoring non-number value '{}'", v));
            }});
        t.push_back({"--hdr-paper-white", Arity::Value, "N",
            "HDR diffuse-white / brightness level in nits (default: 400; raise to brighten)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                if (auto f = parse_float_strict(v)) c.hdr_paper_white_nits = *f;
                else d.warn(std::format("--hdr-paper-white: ignoring non-number value '{}'", v));
            }});

        // ──  value flags (Arity::Value) ─────────────────

        // advanced-materials-and-grading tone-mapper / grading / camera / knob
        // flags. All numeric/enum parsing + clamping lives in the pure helpers
        // in engine/types.h (parse_tone_mapper, clamp_grading_*, clamp_camera_*,
        // clamp_dynres_min_scale, clamp_quality_level via the shared
        // apply_quality_level_flag); this layer owns only the diagnostic text,
        // following the apply_quality_args precedent
        // The single-value knobs use the same atof-style leniency the
        // quality family uses (a non-numeric token → 0.0, then clamped); the
        // multi-value grading flags (--white-balance, ASC CDL slope/offset/power)
        // route through parse_csv_floats and report a malformed arity instead.

        // --tone-mapper NAME — enum select, unknown name rejected back to ACES.
        t.push_back({"--tone-mapper", Arity::Value, "NAME",
            "SDR tone-mapper: aces (default), agx, pbr-neutral, gt7, generic",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const ToneMapperParse p = parse_tone_mapper(v);
                if (p.rejected)
                    d.warn(std::format("WARN: --tone-mapper '{}' is not a recognized "
                                       "operator; retaining ACES", p.rejected_name));
                c.tone_mapper = p.value;
            }});

        // Grading single-value controls — atof-style leniency + clamp helper.
        t.push_back({"--grading-contrast", Arity::Value, "F",
            "Color-grading contrast, 0..2 (default: 1.0 = neutral)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const FloatKnobClamp r =
                    clamp_grading_contrast(parse_float_prefix(v).value_or(0.0f));
                if (r.clamped)
                    d.warn(std::format("WARN: --grading-contrast '{}' out of range "
                                       "[{:.1f},{:.1f}]; clamped to {:.3f}",
                                       buf, ColorGradingConfig::k_contrast_lo,
                                       ColorGradingConfig::k_contrast_hi, r.value));
                c.grading.contrast = r.value;
            }});
        t.push_back({"--grading-saturation", Arity::Value, "F",
            "Color-grading saturation, 0..2 (default: 1.0 = neutral)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const FloatKnobClamp r =
                    clamp_grading_saturation(parse_float_prefix(v).value_or(0.0f));
                if (r.clamped)
                    d.warn(std::format("WARN: --grading-saturation '{}' out of range "
                                       "[{:.1f},{:.1f}]; clamped to {:.3f}",
                                       buf, ColorGradingConfig::k_saturation_lo,
                                       ColorGradingConfig::k_saturation_hi, r.value));
                c.grading.saturation = r.value;
            }});

        // --white-balance T,TINT — two normalized controls in [-1, 1].
        t.push_back({"--white-balance", Arity::Value, "T,TINT",
            "White balance temperature,tint each -1..1 (default: 0,0 = neutral)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::vector<float> f = parse_csv_floats(v);
                if (f.size() != 2) {
                    d.warn(std::format("--white-balance: expected T,TINT; ignoring "
                                       "malformed value '{}'", v));
                    return;
                }
                const FloatKnobClamp t = clamp_grading_white_balance(f[0]);
                const FloatKnobClamp n = clamp_grading_white_balance(f[1]);
                if (t.clamped || n.clamped)
                    d.warn(std::format("WARN: --white-balance '{}' out of range "
                                       "[{:.1f},{:.1f}]; clamped to {:.3f},{:.3f}",
                                       v, ColorGradingConfig::k_white_balance_lo,
                                       ColorGradingConfig::k_white_balance_hi,
                                       t.value, n.value));
                c.grading.temperature = t.value;
                c.grading.tint        = n.value;
            }});

        // ASC CDL slope / offset / power — RGB triples (no clamp helper; the
        // grade is artist-controlled and unbounded, mirroring Filament's API).
        t.push_back({"--grading-slope", Arity::Value, "R,G,B",
            "ASC CDL slope per channel (default: 1,1,1 = neutral)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::vector<float> f = parse_csv_floats(v);
                if (f.size() != 3) {
                    d.warn(std::format("--grading-slope: expected R,G,B; ignoring "
                                       "malformed value '{}'", v));
                    return;
                }
                c.grading.cdl_slope = glm::vec3(f[0], f[1], f[2]);
            }});
        t.push_back({"--grading-offset", Arity::Value, "R,G,B",
            "ASC CDL offset per channel (default: 0,0,0 = neutral)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::vector<float> f = parse_csv_floats(v);
                if (f.size() != 3) {
                    d.warn(std::format("--grading-offset: expected R,G,B; ignoring "
                                       "malformed value '{}'", v));
                    return;
                }
                c.grading.cdl_offset = glm::vec3(f[0], f[1], f[2]);
            }});
        t.push_back({"--grading-power", Arity::Value, "R,G,B",
            "ASC CDL power per channel (default: 1,1,1 = neutral)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::vector<float> f = parse_csv_floats(v);
                if (f.size() != 3) {
                    d.warn(std::format("--grading-power: expected R,G,B; ignoring "
                                       "malformed value '{}'", v));
                    return;
                }
                c.grading.cdl_power = glm::vec3(f[0], f[1], f[2]);
            }});

        // Physical-camera value flags — atof-style leniency + clamp helper.
        t.push_back({"--aperture", Arity::Value, "F",
            "Physical-camera aperture (f-number), 0.5..64 (default: 16)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const FloatKnobClamp r =
                    clamp_camera_aperture(parse_float_prefix(v).value_or(0.0f));
                if (r.clamped)
                    d.warn(std::format("WARN: --aperture '{}' out of range "
                                       "[{:.1f},{:.1f}]; clamped to {:.3f}",
                                       buf, PhysicalCameraConfig::k_aperture_lo,
                                       PhysicalCameraConfig::k_aperture_hi, r.value));
                c.physical_camera.aperture = r.value;
            }});
        t.push_back({"--shutter", Arity::Value, "S",
            "Physical-camera shutter speed (seconds), 1/8000..30 (default: 1/125)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const FloatKnobClamp r =
                    clamp_camera_shutter(parse_float_prefix(v).value_or(0.0f));
                if (r.clamped)
                    d.warn(std::format("WARN: --shutter '{}' out of range "
                                       "[{:.5f},{:.1f}]; clamped to {:.5f}",
                                       buf, PhysicalCameraConfig::k_shutter_lo,
                                       PhysicalCameraConfig::k_shutter_hi, r.value));
                c.physical_camera.shutter_speed = r.value;
            }});
        t.push_back({"--iso", Arity::Value, "N",
            "Physical-camera sensitivity (ISO), 25..204800 (default: 100)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const FloatKnobClamp r =
                    clamp_camera_iso(parse_float_prefix(v).value_or(0.0f));
                if (r.clamped)
                    d.warn(std::format("WARN: --iso '{}' out of range "
                                       "[{:.0f},{:.0f}]; clamped to {:.1f}",
                                       buf, PhysicalCameraConfig::k_iso_lo,
                                       PhysicalCameraConfig::k_iso_hi, r.value));
                c.physical_camera.sensitivity = r.value;
            }});

        // ──  quality knobs (--<feature>-quality) ─

        // --specular-aa-quality reuses the shared apply_quality_level_flag (the
        // same atoi + clamp_quality_level + WARN path the four existing level
        // flags use) into the specular-AA level; --dynamic-resolution-quality
        // mirrors --foliage-quality (atof + clamp helper + WARN on clamp).
        t.push_back({"--specular-aa-quality", Arity::Value, "L",
            "Specular-AA strength level 0..2 (default: 1)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                apply_quality_level_flag("--specular-aa-quality", v,
                                         c.quality.specular_aa_level, d);
            }});
        t.push_back({"--dynamic-resolution-quality", Arity::Value, "F",
            "Dynamic-resolution minimum render scale in (0,1] (default: 0.5)",
            [](BenchConfig& c, std::string_view v, Diagnostics& d) {
                const std::string buf(v);
                const FloatKnobClamp r = clamp_dynres_min_scale(
                    parse_float_prefix(v).value_or(0.0f), c.quality);
                if (r.clamped)
                    d.warn(std::format("WARN: --dynamic-resolution-quality '{}' out of "
                                       "range [{:.2f},{:.2f}]; clamped to {:.3f}",
                                       buf, QualityKnobs::k_dynres_min_scale_lo,
                                       QualityKnobs::k_dynres_min_scale_hi, r.value));
                c.quality.dynres_min_scale = r.value;
            }});

        // --api VALUE — backward-compatibility no-op. The value token is
        // consumed (so it is never reported as an unknown flag, Requirement
        // and graphics_api is left at its GraphicsApi::Filament default;
        // Filament is the only backend.
        t.push_back({"--api", Arity::Value, "API",
            "Graphics backend name (accepted as a no-op; Filament is the only backend)",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* no-op*/ }});

        // ── Configuration-file flags (Arity::Value) ────────────

        // --config / --write-config target a startup ACTION (a Config_File
        // path) rather than a BenchConfig field. They live in the table so
        // generate_help lists them  and so the hash-lookup
        // driver consumes their value token, but the driver special-cases them
        // by name (like --help): it records the path on ParseResult and never
        // calls these no-op applys. They therefore map to no schema key and are
        // absent from flag_schema_keys() / seen_keys.
        t.push_back({"--config", Arity::Value, "FILE",
            "Load settings from FILE (run.sh supplies the repository config.json)",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver*/ }});
        t.push_back({"--write-config", Arity::Value, "FILE",
            "Write the effective configuration to a JSON file and exit",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver*/ }});

        // Visual-audit actions are deliberately absent from config/seen_keys.
        t.push_back({"--camera-pose", Arity::Value, "X,Y,Z,YAW,PITCH",
            "Set a finite world-space camera pose; angles in degrees, pitch -89..89; excludes --camera-tour-time",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--camera-tour-time", Arity::Value, "SECONDS",
            "Hold the authored camera tour at a nonnegative time; excludes --camera-pose",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--capture-tour-motion", Arity::None, "",
            "Advance the authored tour during capture; requires --capture and --camera-tour-time",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--camera-fov", Arity::Value, "DEGREES",
            "Visual-audit vertical field of view, 1..170 (default: 60)",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--capture", Arity::Value, "FILE.ppm",
            "Capture the rendered scene to a PPM and exit; excludes --benchmark",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--capture-frames", Arity::Value, "N",
            "Capture after 1..36000 frames at fixed 1/60-second simulation steps (default: 90)",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--capture-count", Arity::Value, "N",
            "Capture 1..600 consecutive frames after warmup (default: 1); subsequent files add .000001 etc. before the extension",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--capture-floating-motion", Arity::None, "",
            "Animate floating islands during capture as in free-fly mode; requires --capture",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});

        t.push_back({"--capture-fps", Arity::Value, "N",
            "Simulation rate for deterministic captures, 1..240 (default: 60); does not cap rendering FPS",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--capture-realtime", Arity::None, "",
            "Use the normal wall-clock simulation and shader clocks during capture; requires --capture",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--profile", Arity::Value, "FILE.json",
            "Measure the current camera view without screenshot readback; write frame timings and exit",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--profile-seconds", Arity::Value, "N",
            "Profile 63..3600 seconds including a two-second warmup (default: 65)",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});

        t.push_back({"--microprofile", Arity::Value, "BASE",
            "Record a diagnostic CPU trace to BASE; benchmark tours recorded at any point are unscored",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--microprofile-seconds", Arity::Value, "N",
            "Record 0.1..3600 seconds (default: 10); requires --microprofile",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});
        t.push_back({"--microprofile-visible", Arity::None, "",
            "Open the profiler panel during the CLI trace; requires --microprofile",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver */ }});

        // ── Used-asset packer flags ────────────────────────────

        // Like --config above, these activate a startup ACTION rather than a
        // BenchConfig field, so the parse driver special-cases them by name and
        // records the result on ParseResult; the table entries exist so
        // generate_help lists them and so the value flag consumes its token.
        // Their applys are never invoked. --pack-assets activates packing and
        // takes an OPTIONAL Pack_Target; with no value (or a
        // following dash-token) it defaults to "assets/assets.zip" in main.
        // --pack-move opts into
        // Move_Mode; --pack-overwrite allows writing
        // over an existing Pack_Target.
        t.push_back({"--pack-assets", Arity::Value, "[FILE.zip]",
            "Pack used assets into a ZIP (default: assets/assets.zip) and exit (no render window)",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver*/ }});
        t.push_back({"--pack-move", Arity::None, "",
            "With --pack-assets: move used assets out of the source tree instead of copying",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver*/ }});
        t.push_back({"--pack-overwrite", Arity::None, "",
            "With --pack-assets: replace an existing target archive",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver*/ }});
        t.push_back({"--pack-texture-set", Arity::Value, "TEXTURE-SET.json",
            "With --pack-assets: write a validated hi-res-only extension ZIP",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver*/ }});
        t.push_back({"--build-tessellation-cache", Arity::Value, "[FILE.zip]",
            "Headlessly prebuild the complete tessellation cache archive and exit",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver*/ }});
        t.push_back({"--benchmark-loading", Arity::Value, "[OUTPUT_DIR]",
            "Benchmark loose and ZIP-backed loading and emit JSON plus HTML",
            [](BenchConfig&, std::string_view, Diagnostics&) { /* handled in parse driver*/ }});

        return t;
    }();
    return table;
}

ParseResult parse_cli(std::span<const std::string_view> args) {
    ParseResult result;

    // Resolve every flag token through a hash-based lookup keyed by name rather
    // than a sequential strcmp chain.
    const std::unordered_map<std::string_view, const FlagSpec*> index = build_flag_index();

    BenchConfig& cfg = result.config;
    Diagnostics diag;
    bool microprofile_seconds_seen = false;

    // Iterate args[1..end); args[0] is the program name. `i` advances by two
    // for a value flag that consumes its following token.
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view token = args[i];

        const auto it = index.find(token);
        if (it == index.end()) {
            // Unknown token: report it and keep parsing the remaining tokens

            diag.warn(std::format("unknown option '{}'", token));
            continue;
        }

        const FlagSpec& spec = *it->second;

        if (spec.arity == Arity::None) {
            if (spec.name == "--microprofile-visible") {
                result.microprofile.visible = true;
                continue;
            }
            if (spec.name == "--capture-realtime") {
                result.capture.realtime = true;
                continue;
            }
            if (spec.name == "--capture-floating-motion") {
                result.capture.floating_motion = true;
                continue;
            }
            if (spec.name == "--capture-tour-motion") {
                result.capture.tour_motion = true;
                continue;
            }
            // --pack-move / --pack-overwrite record packer intent on the result
            // (a startup action), not a BenchConfig field, so they are
            // special-cased by name before the generic toggle path (the apply
            // signature cannot reach ParseResult). They never set
            // help_requested and are not applied.
            if (spec.name == "--pack-move") {
                result.pack_move = true;
                continue;
            }
            if (spec.name == "--pack-overwrite") {
                result.pack_overwrite = true;
                continue;
            }
            // --help (and, robustly, any known toggle with no apply callable) is
            // a signaling-only entry: it sets help_requested and is NOT applied
            // (see the current "Help handling"). The parser
            // does not exit — that is the wrapper's job.
            if (spec.name == "--list-test-scenes") {
                result.list_test_scenes_requested = true;
            } else if (!spec.apply) {
                result.help_requested = true;
            } else {
                spec.apply(cfg, {}, diag);
                result.config_operations.push_back({std::string(spec.name), {}});
                record_seen_key(spec.name, result.seen_keys);
            }
            continue;
        }

        // Arity::Value below.

        if (spec.name == "--microprofile" || spec.name == "--microprofile-seconds") {
            microprofile_seconds_seen |= spec.name == "--microprofile-seconds";
            if (i + 1 >= args.size() || args[i + 1].starts_with("--")) {
                result.microprofile.invalid = true;
                diag.warn(std::format("{}: expected a value", spec.name));
                continue;
            }
            const auto value = args[++i];
            if (spec.name == "--microprofile" && !value.empty() && !value.contains('\0')) {
                result.microprofile.output_base = std::string(value);
            } else if (spec.name == "--microprofile-seconds") {
                const auto seconds = parse_float_strict(value);
                if (seconds && *seconds >= 0.1f && *seconds <= 3600.0f) {
                    result.microprofile.seconds = *seconds;
                } else {
                    result.microprofile.invalid = true;
                    diag.warn("--microprofile-seconds: expected a finite value in 0.1..3600");
                }
            } else {
                result.microprofile.invalid = true;
                diag.warn("--microprofile: expected a nonempty output base without NUL bytes");
            }
            continue;
        }

        if (is_capture_flag(spec.name)) {
            if (i + 1 >= args.size() || args[i + 1].starts_with("--")) {
                result.capture.invalid = true;
                diag.warn(std::format("{}: expected a visual-audit value", spec.name));
            } else {
                apply_capture_value(result.capture, spec.name, args[++i], diag);
            }
            continue;
        }

        // --pack-assets activates the used-asset packer and takes an OPTIONAL
        // Pack_Target. Like --config it is special-cased by name (it targets a
        // startup action on ParseResult, not a BenchConfig field). Critically,
        // pack_requested is set the moment the TOKEN appears — independent of
        // whether a value follows — so "flag present" is distinguishable from
        // "flag absent". The value is optional: the next
        // token is consumed as Pack_Target ONLY when one exists AND it does NOT
        // begin with '-' (so a following flag such as `--data` is left for the
        // parser, not swallowed as a target). Otherwise pack_target stays unset
        // and NO diagnostic is emitted; main then defaults the target to
        // "assets".
        if (spec.name == "--pack-assets") {
            result.pack_requested = true;
            if (i + 1 < args.size() && !args[i + 1].starts_with('-')) {
                result.pack_target = std::string(args[++i]);
            }
            continue;
        }
        if (spec.name == "--build-tessellation-cache" ||
            spec.name == "--benchmark-loading") {
            const bool cache_action =
                spec.name == "--build-tessellation-cache";
            if (cache_action) result.build_tessellation_cache_requested = true;
            else result.benchmark_loading_requested = true;
            if (i + 1 < args.size() && !args[i + 1].starts_with('-')) {
                if (cache_action) {
                    result.tessellation_cache_target =
                        std::string(args[++i]);
                } else {
                    result.loading_benchmark_output =
                        std::string(args[++i]);
                }
            }
            continue;
        }

        // --config / --write-config capture a Config_File path on the result's
        // side channel rather than mutating a BenchConfig field, so they are
        // special-cased by name (like --help) before the generic value path
        // The value token is still
        // consumed via ++i so it is never reported as an unknown option.
        if (spec.name == "--pack-texture-set") {
            if (i + 1 >= args.size()) {
                diag.warn("--pack-texture-set requires a value");
            } else {
                result.pack_texture_set = std::string(args[++i]);
            }
            continue;
        }
        if (spec.name == "--config" || spec.name == "--write-config") {
            if (i + 1 >= args.size()) {
                // Trailing token: no value to take — report and record nothing,
                // then continue parsing.
                diag.warn(std::format("{}: expected a value", token));
                continue;
            }
            const std::string_view value = args[++i];
            if (spec.name == "--config") {
                if (value.empty()) {
                    // Empty path: report and record nothing.
                    diag.warn("--config: empty path ignored");
                    continue;
                }
                // Last occurrence wins.
                result.config_path = std::string(value);
            } else {
                result.write_config_path = std::string(value);
            }
            continue;
        }

        // Generic value flag: consume the following token. If the flag is the
        // final token there is no value to take — report it and leave the field
        // unchanged.
        if (i + 1 >= args.size()) {
            diag.warn(std::format("{}: expected a value", token));
            if (spec.name == "--test-scene") result.test_scene_invalid = true;
            continue;
        }
        const std::string_view value = args[++i];
        spec.apply(cfg, value, diag);
        if (spec.name == "--test-scene" && !std::ranges::any_of(
                render_test_scene_catalog(), [value](const auto& scene) { return scene.id == value; })) {
            result.test_scene_invalid = true;
        }
        result.config_operations.push_back({std::string(spec.name), std::string(value)});
        record_seen_key(spec.name, result.seen_keys);
    }

    if (!result.microprofile.output_base &&
        (microprofile_seconds_seen || result.microprofile.visible)) {
        result.microprofile.invalid = true;
        diag.warn("--microprofile-seconds and --microprofile-visible require --microprofile");
    }

    if ((result.capture.floating_motion || result.capture.realtime || result.capture.fps != 60.0f || result.capture.count > 1) && !result.capture.output) {
        result.capture.invalid = true;
        diag.warn("Capture sequence and floating motion require --capture");
    }
    if (result.capture.realtime && result.capture.fps != 60.0f) {
        result.capture.invalid = true;
        diag.warn("--capture-realtime cannot use a fixed --capture-fps override");
    }
    if ((result.capture.profile_output && (result.capture.output || cfg.benchmark_mode)) ||
        (!result.capture.profile_output && result.capture.profile_seconds != 65.0f)) {
        result.capture.invalid = true;
        diag.warn("--profile requires a separate unscored run; --profile-seconds requires --profile");
    }
    if (result.capture.tour_motion &&
        (!result.capture.output || !result.capture.tour_time)) {
        result.capture.invalid = true;
        diag.warn("--capture-tour-motion requires --capture and --camera-tour-time");
    }
    if (result.capture.camera_pose && result.capture.tour_time) {
        result.capture.invalid = true;
        diag.warn("--camera-pose and --camera-tour-time cannot be used together");
    }
    if (result.capture.output && cfg.benchmark_mode) {
        result.capture.invalid = true;
        diag.warn("--capture and --benchmark cannot be used together");
    }

    if (!result.seen_keys.contains("quality_preset_label")) {
        cfg.quality_preset_label =
            std::string(render_preset_label(cfg.render_preset));
    }
    result.diagnostics = std::move(diag.messages);
    return result;
}

void apply_cli_arguments(AppConfig& config,
                         std::span<const std::string_view> args) {
    const ParseResult parsed = parse_cli(args);
    BenchConfig merged;
    static_cast<AppConfig&>(merged) = config;
    apply_cli_overlay(merged, parsed);
    config = static_cast<const AppConfig&>(merged);
}

// Render the help listing entirely from flag_table(), in table order, emitting
// exactly one line per FlagSpec and none for any flag absent from the table
// Each line follows the current format
//   "  <name> <value_label>   <help>\n"
// where value_label is empty for toggles (Arity::None). This is a one-to-one
// image of the table, so the per-entry mapping stays stable.
std::string generate_help() {
    std::string out;
    for (const FlagSpec& spec : flag_table()) {
        out += std::format("  {} {}   {}\n", spec.name, spec.value_label, spec.help);
    }
    return out;
}

} // namespace heaven
