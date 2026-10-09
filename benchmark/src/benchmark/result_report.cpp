/*
 * HeavenBench — automatic benchmark result reports
 */
#include "benchmark/result_report.h"

#include "benchmark/score_calculator.h"
#include "engine/render_presets.h"
#include "json.hpp"

#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

namespace heaven {
namespace {

using Json = nlohmann::json;

std::string_view quality_level_id(QualityLevel level) {
    switch (level) {
        case QualityLevel::Low: return "Low";
        case QualityLevel::Medium: return "Medium";
        case QualityLevel::High: return "High";
    }
    return "Medium";
}

template <glm::length_t L, typename T, glm::qualifier Q>
Json glm_vector(const glm::vec<L, T, Q>& value) {
    Json out = Json::array();
    for (glm::length_t i = 0; i < L; ++i) out.push_back(value[i]);
    return out;
}

Json configuration_json(const BenchConfig& c) {
    const FeatureToggles& f = c.features;
    const WeatherConfig& w = c.weather;
    const QualityKnobs& q = c.quality;
    const ColorGradingConfig& g = c.grading;

    Json features = {
        {"grass", f.grass},
        {"particles", f.particles},
        {"god_rays", f.god_rays},
        {"volumetric_fog", f.volumetric_fog},
        {"clouds", f.clouds},
        {"sky", f.sky},
        {"lens_flares", f.lens_flares},
        {"heat_haze", f.heat_haze},
        {"rain", f.rain},
        {"wet_reflections", f.wet_reflections},
        {"soft_shadows", f.soft_shadows},
        {"contact_shadows", f.contact_shadows},
        {"point_light_shadows", f.point_light_shadows},
        {"msaa", f.msaa},
        {"refraction", f.refraction},
        {"reflections", f.reflections},
        {"volumetric_clouds", f.volumetric_clouds},
        {"ssgi", f.ssgi},
        {"massive_foliage", f.massive_foliage},
        {"ground_relief", f.ground_relief},
        {"clear_coat", f.clear_coat},
        {"anisotropic", f.anisotropic},
        {"subsurface", f.subsurface},
        {"cloth", f.cloth},
        {"transparent_shadows", f.transparent_shadows},
        {"colored_penumbra", f.colored_penumbra},
        {"specular_aa", f.specular_aa},
        {"color_grading", f.color_grading},
        {"physical_camera", f.physical_camera},
        {"dynamic_resolution", f.dynamic_resolution},
        {"gpu_instancing", f.gpu_instancing},
        {"gpu_skinning", f.gpu_skinning},
    };

    Json weather = {
        {"rain_intensity", w.rain_intensity},
        {"dry_down_seconds", w.dry_down_seconds},
        {"wet_soak_seconds", w.wet_soak_seconds},
        {"wet_dry_seconds", w.wet_dry_seconds},
        {"min_sun_scale_at_1", w.min_sun_scale_at_1},
        {"overcast_desat_at_1", w.overcast_desat_at_1},
        {"overcast_lum_at_1", w.overcast_lum_at_1},
        {"max_fog_density_at_1", w.max_fog_density_at_1},
        {"max_spec_boost_at_1", w.max_spec_boost_at_1},
        {"max_diffuse_darken_at_1", w.max_diffuse_darken_at_1},
    };

    const auto quality_level_name = [](QualityLevel level) {
        switch (level) {
            case QualityLevel::Low: return "Low";
            case QualityLevel::Medium: return "Medium";
            case QualityLevel::High: return "High";
        }
        return "Unknown";
    };

    Json quality = {
        {"soft_shadow_level", quality_level_name(q.soft_shadow_level)},
        {"contact_shadow_level", quality_level_name(q.contact_shadow_level)},
        {"contact_shadow_step_min", q.contact_shadow_step_min},
        {"contact_shadow_step_max", q.contact_shadow_step_max},
        {"shadow_light_budget", q.shadow_light_budget},
        {"shadow_light_budget_max", q.shadow_light_budget_max},
        {"msaa_samples", q.msaa_samples},
        {"cloud_quality", quality_level_name(q.cloud_quality)},
        {"cloud_step_min", q.cloud_step_min},
        {"cloud_step_max", q.cloud_step_max},
        {"ssgi_quality", quality_level_name(q.ssgi_quality)},
        {"ssgi_sample_max", q.ssgi_sample_max},
        {"foliage_density", q.foliage_density},
        {"foliage_density_max", q.foliage_density_max},
        {"foliage_blade_baseline", q.foliage_blade_baseline},
        {"foliage_blade_max", q.foliage_blade_max},
        {"specular_aa_level", quality_level_name(q.specular_aa_level)},
        {"ao_quality", quality_level_name(q.ao_quality)},
        {"bloom_quality", quality_level_name(q.bloom_quality)},
        {"dof_quality", quality_level_name(q.dof_quality)},
        {"taa_quality", quality_level_name(q.taa_quality)},
        {"sun_shadow_map_size", q.sun_shadow_map_size},
        {"point_shadow_map_size", q.point_shadow_map_size},
        {"dynamic_resolution_min_scale", q.dynres_min_scale},
    };

    Json grading = {
        {"temperature", g.temperature},
        {"tint", g.tint},
        {"mixer_red", glm_vector(g.mixer_red)},
        {"mixer_green", glm_vector(g.mixer_green)},
        {"mixer_blue", glm_vector(g.mixer_blue)},
        {"shadows", glm_vector(g.shadows)},
        {"midtones", glm_vector(g.midtones)},
        {"highlights", glm_vector(g.highlights)},
        {"cdl_slope", glm_vector(g.cdl_slope)},
        {"cdl_offset", glm_vector(g.cdl_offset)},
        {"cdl_power", glm_vector(g.cdl_power)},
        {"contrast", g.contrast},
        {"saturation", g.saturation},
        {"night_adaptation", g.night_adaptation},
    };

    return {
        {"data_dir", c.data_dir},
        {"texture_quality", texture_quality_id(c.texture_quality)},
        {"node_file", c.node_file},
        {"shader_dir", c.shader_dir},
        {"width", c.width},
        {"height", c.height},
        {"fullscreen", c.fullscreen},
        {"hdr", {
            {"enabled", c.hdr},
            {"peak_nits", c.hdr_peak_nits},
            {"paper_white_nits", c.hdr_paper_white_nits},
        }},
        {"tessellation", {
            {"enabled", c.tessellation.enabled},
            {"scale", c.tessellation.scale},
            {"factor", c.tessellation.factor},
            {"distance", c.tessellation.distance},
        }},
        {"time_of_day", c.time_of_day},
        {"heat_haze_enabled", c.heat_haze_enabled},
        {"features", std::move(features)},
        {"weather", std::move(weather)},
        {"music", {
            {"enabled", c.music.enabled},
            {"volume", c.music.volume},
        }},
        {"quality", std::move(quality)},
        {"tone_mapper", to_string(c.tone_mapper)},
        {"grading", std::move(grading)},
        {"physical_camera", {
            {"aperture", c.physical_camera.aperture},
            {"shutter_speed", c.physical_camera.shutter_speed},
            {"sensitivity_iso", c.physical_camera.sensitivity},
        }},
        {"wind_debug", c.wind_debug},
        {"cloth_sim_hz", c.cloth_sim_hz},
        {"quality_preset", render_preset_id(c.render_preset)},
        {"effective_preset", render_preset_id(detect_render_preset(c))},
        {"material_workflow", material_workflow_id(c.material_workflow)},
        {"cpu_stress", {
            {"enabled", c.cpu_stress.enabled},
            {"workers", c.cpu_stress.workers},
            {"duration_seconds", c.cpu_stress.duration_seconds},
        }},
        {"graphics_api", to_string(c.graphics_api)},
        {"benchmark_mode", c.benchmark_mode},
    };
}

std::tm utc_time(std::time_t value) {
    std::tm out{};
#ifdef _WIN32
    gmtime_s(&out, &value);
#else
    gmtime_r(&value, &out);
#endif
    return out;
}

std::tm local_time(std::time_t value) {
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &value);
#else
    localtime_r(&value, &out);
#endif
    return out;
}

int timestamp_milliseconds(std::chrono::system_clock::time_point timestamp) {
    const auto duration = timestamp.time_since_epoch();
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(duration - seconds)
            .count();
    if (millis < 0) millis += 1000;
    return static_cast<int>(millis);
}

std::string timestamp_utc(std::chrono::system_clock::time_point timestamp) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(timestamp);
    const std::tm tm = utc_time(raw);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0')
        << timestamp_milliseconds(timestamp) << 'Z';
    return out.str();
}

std::string filename_timestamp(std::chrono::system_clock::time_point timestamp) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(timestamp);
    const std::tm tm = local_time(raw);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d_%H%M%S")
        << '_' << std::setw(3) << std::setfill('0')
        << timestamp_milliseconds(timestamp);
    return out.str();
}

std::string filename_component(std::string value) {
    for (char& ch : value) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (!std::isalnum(u) && ch != '.' && ch != '-' && ch != '_') ch = '_';
    }
    return value.empty() ? "unknown" : value;
}

std::string html_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += ch; break;
        }
    }
    return out;
}

void append_detail_row(std::ostringstream& out, std::string_view label,
                       std::string_view value) {
    out << "<tr><td class=\"right\">" << html_escape(label)
        << ":</td><td><div class=\"highlight\">" << html_escape(value)
        << "</div></td></tr>\n";
}

std::string display_number(double value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

void flatten_configuration(const Json& value, const std::string& prefix,
                           std::vector<std::pair<std::string, std::string>>& out) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string key =
                prefix.empty() ? it.key() : prefix + "." + it.key();
            flatten_configuration(it.value(), key, out);
        }
        return;
    }
    out.emplace_back(prefix, value.dump());
}

Json report_json(const RunResult& result, const BenchConfig& config,
                 const ApplicationBuildInfo& application,
                 std::string_view report_id,
                 std::string_view created_at_utc) {
    const HardwareScoreResult& p = result.performance;
    const SystemInfo& system = result.system;
    const SettingsInfo& settings = result.settings;

    Json root = {
        {"schema_version", kResultReportSchemaVersion},
        {"application", {
            {"name", "HeavenBench"},
            {"version", application.version},
            {"build_architecture", application.architecture},
        }},
        {"run", {
            {"id", report_id},
            {"timestamp_utc", created_at_utc},
            {"frames", result.frames},
            {"duration_seconds", result.total_time},
        }},
        {"scores", {
            {"version", p.score_version},
            {"overall", p.scores.overall},
            {"cpu", p.scores.cpu},
            {"gpu", p.scores.gpu},
        }},
        {"results", {
            {"average_fps", result.average_fps},
            {"min_fps", result.min_fps},
            {"max_fps", result.max_fps},
            {"path_geomean_fps", p.metrics.path_geomean_fps},
            {"one_percent_low_fps", p.metrics.one_percent_low_fps},
            {"effective_fps", p.metrics.effective_fps},
        }},
        {"cpu", {
            {"scene_meshes", p.metrics.cpu.scene_meshes},
            {"scene_instances", p.metrics.cpu.scene_instances},
            {"updates_per_second", p.metrics.cpu.updates_per_second},
            {"realtime_multiplier",
                p.metrics.cpu.updates_per_second / kCpuSimulationRate},
            {"checksum", p.metrics.cpu.checksum},
            {"timing_ms", {
                {"average", p.metrics.cpu.timing.average_ms},
                {"p95", p.metrics.cpu.timing.p95_ms},
                {"p99", p.metrics.cpu.timing.p99_ms},
                {"samples", p.metrics.cpu.timing.samples},
            }},
        }},
        {"cpu_stress", {
            {"enabled", result.cpu_stress.enabled},
            {"workers", result.cpu_stress.workers},
            {"scene_meshes", result.cpu_stress.scene_meshes},
            {"scene_instances", result.cpu_stress.scene_instances},
            {"duration_seconds", result.cpu_stress.duration_seconds},
            {"aggregate_updates_per_second",
                result.cpu_stress.aggregate_updates_per_second},
            {"worker_updates_per_second",
                result.cpu_stress.worker_updates_per_second},
            {"checksums", result.cpu_stress.checksums},
        }},
        {"renderer_features", {
            {"ibl_prefiltered", result.renderer_features.ibl_prefiltered},
            {"msaa_requested", result.renderer_features.msaa_requested},
            {"msaa_active", result.renderer_features.msaa_active},
            {"msaa_samples", result.renderer_features.msaa_samples},
            {"custom_msaa_resolve",
                result.renderer_features.custom_msaa_resolve},
            {"alpha_to_coverage",
                result.renderer_features.alpha_to_coverage},
            {"bent_normals", result.renderer_features.bent_normals},
            {"multi_bounce_ao", result.renderer_features.multi_bounce_ao},
            {"gpu_instancing", result.renderer_features.gpu_instancing},
            {"gpu_skinning", result.renderer_features.gpu_skinning},
            {"refraction", {
                {"requested", result.renderer_features.refraction_requested},
                {"active", result.renderer_features.refraction_active},
            }},
            {"ao_quality", quality_level_id(
                result.renderer_features.ao_quality)},
            {"sun_shadow_map_size",
                result.renderer_features.sun_shadow_map_size},
            {"point_shadow_map_size",
                result.renderer_features.point_shadow_map_size},
            {"material_workflow", {
                {"requested", material_workflow_id(
                    result.renderer_features.material_workflow_requested)},
                {"active", material_workflow_id(
                    result.renderer_features.material_workflow_active)},
                {"compatibility_surfaces",
                    result.renderer_features.compatibility_workflow_surfaces},
                {"experimental_surfaces",
                    result.renderer_features.experimental_workflow_surfaces},
                {"compatibility_instances",
                    result.renderer_features.compatibility_workflow_instances},
                {"experimental_instances",
                    result.renderer_features.experimental_workflow_instances},
            }},
            {"enabled_render_features",
                result.renderer_features.enabled_render_features},
            {"total_render_features",
                result.renderer_features.total_render_features},
            {"material_bindings",
                result.renderer_features.material_bindings},
            {"textures", {
                {"quality_requested", texture_quality_id(
                    result.renderer_features.texture_quality_requested)},
                {"quality_active", texture_quality_id(
                    result.renderer_features.texture_quality_active)},
                {"srgb", result.renderer_features.srgb_textures},
                {"scalar_linear", result.renderer_features.scalar_textures},
                {"normal_linear", result.renderer_features.normal_textures},
                {"hdr_linear", result.renderer_features.hdr_textures},
                {"fallbacks", result.renderer_features.texture_fallbacks},
                {"hires", result.renderer_features.hires_textures},
                {"original", result.renderer_features.original_textures},
                {"missing_variant_fallbacks",
                    result.renderer_features.texture_missing_variant_fallbacks},
                {"unsupported_format_fallbacks",
                    result.renderer_features.texture_unsupported_format_fallbacks},
                {"invalid_variant_fallbacks",
                    result.renderer_features.texture_invalid_variant_fallbacks},
                {"selected_mip_bytes",
                    result.renderer_features.selected_texture_bytes},
                {"effective_max_dimension",
                    result.renderer_features.effective_texture_max_dimension},
            }},
        }},
        {"gpu", {
            {"equivalent_fps", p.metrics.gpu_equivalent_fps},
            {"timing_ms", {
                {"average", p.metrics.gpu.average_ms},
                {"p95", p.metrics.gpu.p95_ms},
                {"p99", p.metrics.gpu.p99_ms},
                {"samples", p.metrics.gpu.samples},
            }},
        }},
        {"system", {
            {"platform", {
                {"description", system.platform},
                {"os_name", system.platform_raw.os_name},
                {"kernel_version", system.platform_raw.kernel_version},
                {"architecture", system.platform_raw.machine_arch},
            }},
            {"cpu", {
                {"description", system.cpu},
                {"model_name", system.cpu_raw.model_name},
                {"clock_mhz", system.cpu_raw.clock_mhz >= 0.0
                    ? Json(system.cpu_raw.clock_mhz) : Json(nullptr)},
                {"logical_cores", system.cpu_raw.logical_cores >= 1
                    ? Json(system.cpu_raw.logical_cores) : Json(nullptr)},
            }},
            {"gpu", {
                {"description", system.gpu},
                {"model_name", system.gpu_raw.model_name},
                {"vram_mb", system.gpu_raw.vram_mb >= 0
                    ? Json(system.gpu_raw.vram_mb) : Json(nullptr)},
                {"driver_name", system.gpu_raw.driver_name},
                {"driver_version", system.gpu_raw.driver_version},
                {"driver_info", system.gpu_raw.driver_info},
                {"driver_check_summary", system.gpu_raw.driver_check_summary},
                {"driver_check_recommendation", system.gpu_raw.driver_check_recommendation},
            }},
        }},
        {"settings", {
            {"render_api", settings.render_api},
            {"display_mode", settings.display_mode},
            {"quality_preset", settings.quality_preset},
            {"effective_preset",
                render_preset_id(detect_render_preset(config))},
            {"width", settings.display_raw.width},
            {"height", settings.display_raw.height},
            {"aa_samples", settings.display_raw.aa_samples},
            {"fullscreen", settings.display_raw.fullscreen},
        }},
        {"configuration", configuration_json(config)},
    };
    if (result.diagnostic) {
        root["run"]["diagnostic"] = true;
        root["run"]["scored"] = false;
        root["run"]["diagnostic_reason"] = "microprofiler recording";
        for (const char* key : {"overall", "cpu", "gpu"}) root["scores"][key] = nullptr;
    }
    return root;
}

std::string report_html(const Json& root) {
    const Json& scores = root.at("scores");
    const Json& results = root.at("results");
    const Json& cpu = root.at("cpu");
    const Json& gpu = root.at("gpu");
    const Json& cpu_stress = root.at("cpu_stress");
    const Json& renderer_features = root.at("renderer_features");
    const Json& system = root.at("system");
    const Json& settings = root.at("settings");
    const Json& run = root.at("run");
    const Json& app = root.at("application");

    std::ostringstream out;
    out <<
R"(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>HeavenBench benchmark results</title>
<style>
body { background:#1a242a; color:#b4c8d2; margin:20px; font:14px Arial,sans-serif; }
h1 { color:#ff9900; font-size:36px; text-align:center; }
h2 { text-align:center; font-size:24px; margin-top:32px; }
h3 { color:#fff; text-align:center; }
.right { text-align:right; white-space:nowrap; }
.orange { color:#ff9900; }
.highlight { color:#fff; }
.meta { text-align:center; }
table.result { border:0; margin:auto; }
table.result td { border:0; padding:3px 8px; font-size:180%; }
table.result .highlight { color:#ff9900; font-weight:bold; }
table.detail { border:1px solid #b4c8d2; border-collapse:collapse; margin:auto; min-width:520px; }
table.detail td, table.detail th { border:1px solid #b4c8d2; padding:5px 8px; }
details { max-width:1000px; margin:32px auto; }
details table { width:100%; }
footer { margin:42px 10px 10px; text-align:center; }
</style></head><body>
)";
    out << "<h1>"
        << html_escape(format_application_title({
               app.at("version").get<std::string>(),
               app.at("build_architecture").get<std::string>()}))
        << "</h1>\n<table class=\"result\">\n";
    append_detail_row(out, "FPS",
        display_number(results.at("average_fps").get<double>(), 1));
    const bool diagnostic = run.value("diagnostic", false);
    for (const auto& [label, key] : {std::pair{"Overall Score", "overall"},
                                  std::pair{"CPU Score", "cpu"},
                                  std::pair{"GPU Score", "gpu"}}) {
        append_detail_row(out, label, diagnostic ? "Unscored (diagnostic)" :
            std::to_string(round_score(scores.at(key).get<double>())));
    }
    append_detail_row(out, "Min FPS",
        display_number(results.at("min_fps").get<double>(), 1));
    append_detail_row(out, "Max FPS",
        display_number(results.at("max_fps").get<double>(), 1));
    out << "</table>\n<p class=\"meta\">Report "
        << html_escape(run.at("id").get<std::string>()) << " &middot; "
        << html_escape(run.at("timestamp_utc").get<std::string>())
        << " &middot; schema " << root.at("schema_version").get<int>()
        << " &middot; score version " << scores.at("version").get<int>()
        << "</p>\n";

    if (diagnostic) out << "<p class=\"meta\">Microprofiler recording occurred during this tour. "
        "Frame statistics are diagnostic; no benchmark score is awarded.</p>\n";
    out << "<h2>Overall</h2><table class=\"detail\">\n";
    append_detail_row(out, "Average FPS",
        display_number(results.at("average_fps").get<double>(), 2));
    append_detail_row(out, "Path geomean FPS",
        display_number(results.at("path_geomean_fps").get<double>(), 2));
    append_detail_row(out, "1% low FPS",
        display_number(results.at("one_percent_low_fps").get<double>(), 2));
    append_detail_row(out, "Effective FPS",
        display_number(results.at("effective_fps").get<double>(), 2));
    append_detail_row(out, "Frames", std::to_string(run.at("frames").get<uint64_t>()));
    append_detail_row(out, "Measured time",
        display_number(run.at("duration_seconds").get<double>(), 3) + " s");
    out << "</table>\n";

    if (cpu_stress.at("enabled").get<bool>()) {
        out << "<h2>CPU multicore stress</h2><table class=\"detail\">\n";
        append_detail_row(out, "Workers",
            std::to_string(cpu_stress.at("workers").get<int>()));
        append_detail_row(out, "Scene per worker",
            std::to_string(cpu_stress.at("scene_meshes").get<size_t>()) + " meshes / " +
            std::to_string(cpu_stress.at("scene_instances").get<size_t>()) + " instances");
        append_detail_row(out, "Aggregate updates/s",
            display_number(
                cpu_stress.at("aggregate_updates_per_second").get<double>(), 1));
        append_detail_row(out, "Duration",
            display_number(cpu_stress.at("duration_seconds").get<double>(), 1) +
            " s");
        const auto& rates = cpu_stress.at("worker_updates_per_second");
        const auto& checksums = cpu_stress.at("checksums");
        for (size_t i = 0; i < rates.size(); ++i) {
            append_detail_row(
                out, "Worker " + std::to_string(i),
                display_number(rates.at(i).get<double>(), 1) +
                    " updates/s; checksum " +
                    display_number(checksums.at(i).get<double>(), 6));
        }
        out << "</table>\n";
    }

    out << "<h2>CPU</h2><table class=\"detail\">\n";
    append_detail_row(out, "Scene",
        std::to_string(cpu.at("scene_meshes").get<size_t>()) + " meshes / " +
        std::to_string(cpu.at("scene_instances").get<size_t>()) + " instances");
    append_detail_row(out, "Updates per second",
        display_number(cpu.at("updates_per_second").get<double>(), 1));
    append_detail_row(out, "Real-time speed",
        display_number(cpu.at("realtime_multiplier").get<double>(), 1) + "x");
    append_detail_row(out, "Average / p95 / p99",
        display_number(cpu.at("timing_ms").at("average").get<double>(), 3) +
        " / " + display_number(cpu.at("timing_ms").at("p95").get<double>(), 3) +
        " / " + display_number(cpu.at("timing_ms").at("p99").get<double>(), 3) +
        " ms");
    append_detail_row(out, "Samples",
        std::to_string(cpu.at("timing_ms").at("samples").get<size_t>()));
    out << "</table>\n";

    out << "<h2>GPU</h2><table class=\"detail\">\n";
    append_detail_row(out, "Equivalent FPS",
        display_number(gpu.at("equivalent_fps").get<double>(), 2));
    append_detail_row(out, "Average / p95 / p99",
        display_number(gpu.at("timing_ms").at("average").get<double>(), 3) +
        " / " + display_number(gpu.at("timing_ms").at("p95").get<double>(), 3) +
        " / " + display_number(gpu.at("timing_ms").at("p99").get<double>(), 3) +
        " ms");
    append_detail_row(out, "Samples",
        std::to_string(gpu.at("timing_ms").at("samples").get<size_t>()));
    out << "</table>\n";

    out << "<h2>System</h2><table class=\"detail\">\n";
    append_detail_row(out, "Build architecture",
        app.at("build_architecture").get<std::string>());
    append_detail_row(out, "Platform",
        system.at("platform").at("description").get<std::string>());
    append_detail_row(out, "CPU model",
        system.at("cpu").at("description").get<std::string>());
    append_detail_row(out, "GPU model",
        system.at("gpu").at("description").get<std::string>());
    constexpr std::array driver_fields{
        std::pair{"driver_name", "GPU driver"},
        std::pair{"driver_version", "Driver version"},
        std::pair{"driver_info", "Driver details"},
        std::pair{"driver_check_summary", "Driver check"},
        std::pair{"driver_check_recommendation", "Driver recommendation"},
    };
    for (const auto& [key, label] : driver_fields) {
        const auto& value = system.at("gpu").at(key).get_ref<const std::string&>();
        if (!value.empty()) append_detail_row(out, label, value);
    }
    out << "</table>\n<h2>Settings</h2><table class=\"detail\">\n";
    append_detail_row(out, "Render", settings.at("render_api").get<std::string>());
    append_detail_row(out, "Mode", settings.at("display_mode").get<std::string>());
    append_detail_row(out, "Preset", settings.at("quality_preset").get<std::string>());
    append_detail_row(out, "Effective preset",
        settings.at("effective_preset").get<std::string>());
    out << "</table>\n";

    out << "<h2>Active renderer features</h2><table class=\"detail\">\n";
    append_detail_row(out, "HDR IBL",
        renderer_features.at("ibl_prefiltered").get<bool>()
            ? "prefiltered" : "flat ambient fallback");
    append_detail_row(out, "MSAA",
        std::to_string(renderer_features.at("msaa_samples").get<int>()) +
        "x; custom resolve " +
        (renderer_features.at("custom_msaa_resolve").get<bool>() ? "on" : "off") +
        "; alpha-to-coverage " +
        (renderer_features.at("alpha_to_coverage").get<bool>() ? "on" : "off"));
    append_detail_row(out, "AO material options",
        renderer_features.at("ao_quality").get<std::string>() + "; " +
        std::string(renderer_features.at("bent_normals").get<bool>()
            ? "bent normals" : "default normals") +
        (renderer_features.at("multi_bounce_ao").get<bool>()
            ? ", multi-bounce" : ""));
    append_detail_row(out, "Shadow maps",
        "sun " +
        std::to_string(renderer_features.at("sun_shadow_map_size").get<int>()) +
        "; point " +
        std::to_string(renderer_features.at("point_shadow_map_size").get<int>()));
    const Json& workflow = renderer_features.at("material_workflow");
    append_detail_row(out, "Material workflow",
        workflow.at("requested").get<std::string>() + " requested; " +
        workflow.at("active").get<std::string>() + " active");
    append_detail_row(out, "Workflow surface/use counts",
        "compat " +
        std::to_string(workflow.at("compatibility_surfaces").get<uint64_t>()) +
        "/" +
        std::to_string(workflow.at("compatibility_instances").get<uint64_t>()) +
        "; experimental " +
        std::to_string(workflow.at("experimental_surfaces").get<uint64_t>()) +
        "/" +
        std::to_string(workflow.at("experimental_instances").get<uint64_t>()));
    const Json& textures = renderer_features.at("textures");
    append_detail_row(out, "Texture quality",
        textures.at("quality_requested").get<std::string>() +
        " requested; " + textures.at("quality_active").get<std::string>() +
        " active; hi-res/original " +
        std::to_string(textures.at("hires").get<uint64_t>()) + "/" +
        std::to_string(textures.at("original").get<uint64_t>()) +
        "; selected " +
        std::to_string(textures.at("selected_mip_bytes").get<uint64_t>()) +
        " bytes; max " +
        std::to_string(textures.at("effective_max_dimension").get<uint32_t>()) +
        " px");
    append_detail_row(out, "Texture semantics",
        "sRGB " + std::to_string(textures.at("srgb").get<uint64_t>()) +
        "; scalar " +
        std::to_string(textures.at("scalar_linear").get<uint64_t>()) +
        "; normal " +
        std::to_string(textures.at("normal_linear").get<uint64_t>()) +
        "; HDR " +
        std::to_string(textures.at("hdr_linear").get<uint64_t>()) +
        "; fallbacks " +
        std::to_string(textures.at("fallbacks").get<uint64_t>()) +
        " (missing " +
        std::to_string(textures.at("missing_variant_fallbacks").get<uint64_t>()) +
        ", unsupported " +
        std::to_string(textures.at("unsupported_format_fallbacks").get<uint64_t>()) +
        ", invalid/corrupt " +
        std::to_string(textures.at("invalid_variant_fallbacks").get<uint64_t>()) +
        ")");
    out << "</table>\n";

    std::vector<std::pair<std::string, std::string>> config_rows;
    flatten_configuration(root.at("configuration"), "", config_rows);
    out << "<details><summary><strong>Full effective configuration</strong></summary>"
           "<table class=\"detail\"><tr><th>Setting</th><th>Value</th></tr>\n";
    for (const auto& [key, value] : config_rows) {
        out << "<tr><td>" << html_escape(key) << "</td><td class=\"highlight\">"
            << html_escape(value) << "</td></tr>\n";
    }
    out << "</table></details>\n<footer>Generated by HeavenBench "
        << html_escape(app.at("version").get<std::string>())
        << "</footer>\n</body></html>\n";
    return out.str();
}

std::optional<std::string> write_temp_file(
        const std::filesystem::path& path, std::string_view contents) {
    errno = 0;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        const std::error_code ec(errno, std::generic_category());
        return "cannot open '" + path.string() + "' for writing: " + ec.message();
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    if (!out.good()) {
        return "failed to write '" + path.string() + "'";
    }
    return std::nullopt;
}

void remove_if_present(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

BenchmarkReportDocument build_benchmark_report(
        const RunResult& result, const BenchConfig& config,
        const ApplicationBuildInfo& application,
        std::chrono::system_clock::time_point timestamp) {
    const std::string safe_version = filename_component(application.version);
    const std::string report_id =
        "HeavenBench_" + safe_version + "_" + filename_timestamp(timestamp);
    const std::string created_at = timestamp_utc(timestamp);
    const Json root =
        report_json(result, config, application, report_id, created_at);

    BenchmarkReportDocument document;
    document.report_id = report_id;
    document.json = root.dump(
        2, ' ', false, Json::error_handler_t::replace) + '\n';
    document.html = report_html(root);
    return document;
}

BenchmarkReportWriteResult write_benchmark_report(
        const RunResult& result, const BenchConfig& config,
        const std::filesystem::path& results_directory,
        const ApplicationBuildInfo& application,
        std::chrono::system_clock::time_point timestamp) {
    BenchmarkReportWriteResult outcome;
    BenchmarkReportDocument document;
    try {
        document = build_benchmark_report(
            result, config, application, timestamp);
    } catch (const std::exception& e) {
        outcome.diagnostics.push_back(
            std::string("could not build benchmark report: ") + e.what());
        return outcome;
    }

    std::error_code ec;
    std::filesystem::create_directories(results_directory, ec);
    if (ec) {
        outcome.diagnostics.push_back(
            "cannot create results directory '" + results_directory.string() +
            "': " + ec.message());
        return outcome;
    }

    std::string stem = document.report_id;
    int suffix = 0;
    std::filesystem::path json_path;
    std::filesystem::path html_path;
    std::filesystem::path json_temp;
    std::filesystem::path html_temp;
    while (true) {
        const std::string candidate =
            stem + (suffix == 0 ? "" : "_" + std::to_string(suffix));
        json_path = results_directory / (candidate + ".json");
        html_path = results_directory / (candidate + ".html");
        json_temp = results_directory / (candidate + ".json.tmp");
        html_temp = results_directory / (candidate + ".html.tmp");
        if (!std::filesystem::exists(json_path, ec) && !ec &&
            !std::filesystem::exists(html_path, ec) && !ec &&
            !std::filesystem::exists(json_temp, ec) && !ec &&
            !std::filesystem::exists(html_temp, ec) && !ec) {
            break;
        }
        if (ec) {
            outcome.diagnostics.push_back(
                "cannot inspect results directory '" +
                results_directory.string() + "': " + ec.message());
            return outcome;
        }
        ++suffix;
    }

    if (suffix > 0) {
        const std::string candidate =
            stem + "_" + std::to_string(suffix);
        try {
            Json root = Json::parse(document.json);
            root["run"]["id"] = candidate;
            document.report_id = candidate;
            document.json = root.dump(
                2, ' ', false, Json::error_handler_t::replace) + '\n';
            document.html = report_html(root);
        } catch (const std::exception& e) {
            outcome.diagnostics.push_back(
                std::string("could not finalize benchmark report ID: ") +
                e.what());
            return outcome;
        }
    }

    if (const auto error = write_temp_file(json_temp, document.json)) {
        outcome.diagnostics.push_back(*error);
        remove_if_present(json_temp);
        return outcome;
    }
    if (const auto error = write_temp_file(html_temp, document.html)) {
        outcome.diagnostics.push_back(*error);
        remove_if_present(json_temp);
        remove_if_present(html_temp);
        return outcome;
    }

    std::filesystem::rename(json_temp, json_path, ec);
    if (ec) {
        outcome.diagnostics.push_back(
            "cannot publish '" + json_path.string() + "': " + ec.message());
        remove_if_present(json_temp);
        remove_if_present(html_temp);
        return outcome;
    }
    std::filesystem::rename(html_temp, html_path, ec);
    if (ec) {
        outcome.diagnostics.push_back(
            "cannot publish '" + html_path.string() + "': " + ec.message());
        remove_if_present(json_path);
        remove_if_present(html_temp);
        return outcome;
    }

    outcome.json_path = std::move(json_path);
    outcome.html_path = std::move(html_path);
    return outcome;
}

} // namespace heaven
