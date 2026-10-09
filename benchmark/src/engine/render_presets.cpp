#include "engine/render_presets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace heaven {
namespace {

std::string normalized(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        if (c == '_' || c == ' ') return '-';
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

void apply_balanced(AppConfig& c) noexcept {
    c.tessellation.enabled = true;
    c.tessellation.scale = 1.0f;
    c.tessellation.factor = 1.0f;
    c.tessellation.distance = 1.0f;
    c.features.clouds = false;
    c.features.volumetric_clouds = true;
    c.features.ssgi = true;
    c.features.specular_aa = true;
    c.features.msaa = true;
    c.features.bloom = true;
    c.features.ambient_occlusion = true;
    c.features.taa = true;
    c.features.depth_of_field = true;
    c.features.dynamic_resolution = false;
    c.features.gpu_instancing = true;
    c.features.gpu_skinning = true;
    c.quality.soft_shadow_level = QualityLevel::Medium;
    c.quality.contact_shadow_level = QualityLevel::Medium;
    c.quality.shadow_light_budget = 2;
    c.quality.msaa_samples = 2;
    c.quality.cloud_quality = QualityLevel::Medium;
    c.quality.ssgi_quality = QualityLevel::Medium;
    c.quality.foliage_density = 1.0f;
    c.quality.specular_aa_level = QualityLevel::Medium;
    c.quality.ao_quality = QualityLevel::Medium;
    c.quality.bloom_quality = QualityLevel::Low;
    c.quality.dof_quality = QualityLevel::Low;
    c.quality.taa_quality = QualityLevel::Low;
    c.quality.sun_shadow_map_size = 2048;
    c.quality.point_shadow_map_size = 256;
    c.material_workflow = MaterialWorkflow::MetallicRoughnessCompat;
    c.cpu_stress = {};
}

bool same_owned_fields(const AppConfig& a, const AppConfig& b) noexcept {
    const auto& aq = a.quality;
    const auto& bq = b.quality;
    return a.tessellation.enabled == b.tessellation.enabled &&
        a.tessellation.scale == b.tessellation.scale &&
        a.tessellation.factor == b.tessellation.factor &&
        a.tessellation.distance == b.tessellation.distance &&
        a.features.clouds == b.features.clouds &&
        a.features.volumetric_clouds == b.features.volumetric_clouds &&
        a.features.ssgi == b.features.ssgi &&
        a.features.specular_aa == b.features.specular_aa &&
        a.features.msaa == b.features.msaa &&
        a.features.bloom == b.features.bloom &&
        a.features.ambient_occlusion == b.features.ambient_occlusion &&
        a.features.taa == b.features.taa &&
        a.features.depth_of_field == b.features.depth_of_field &&
        a.features.dynamic_resolution == b.features.dynamic_resolution &&
        a.features.gpu_instancing == b.features.gpu_instancing &&
        a.features.gpu_skinning == b.features.gpu_skinning &&
        aq.soft_shadow_level == bq.soft_shadow_level &&
        aq.contact_shadow_level == bq.contact_shadow_level &&
        aq.shadow_light_budget == bq.shadow_light_budget &&
        aq.msaa_samples == bq.msaa_samples &&
        aq.cloud_quality == bq.cloud_quality &&
        aq.ssgi_quality == bq.ssgi_quality &&
        aq.foliage_density == bq.foliage_density &&
        aq.specular_aa_level == bq.specular_aa_level &&
        aq.ao_quality == bq.ao_quality &&
        aq.bloom_quality == bq.bloom_quality &&
        aq.dof_quality == bq.dof_quality &&
        aq.taa_quality == bq.taa_quality &&
        aq.sun_shadow_map_size == bq.sun_shadow_map_size &&
        aq.point_shadow_map_size == bq.point_shadow_map_size &&
        a.material_workflow == b.material_workflow &&
        a.cpu_stress.enabled == b.cpu_stress.enabled &&
        a.cpu_stress.workers == b.cpu_stress.workers &&
        a.cpu_stress.duration_seconds == b.cpu_stress.duration_seconds;
}

} // namespace

std::string_view render_preset_id(RenderPreset preset) noexcept {
    switch (preset) {
        case RenderPreset::BalancedSystem: return "balanced";
        case RenderPreset::VisualUltra: return "visual-ultra";
        case RenderPreset::GpuTorture: return "gpu-torture";
        case RenderPreset::CpuTorture: return "cpu-torture";
        case RenderPreset::Custom: return "custom";
    }
    return "custom";
}

std::string_view render_preset_label(RenderPreset preset) noexcept {
    switch (preset) {
        case RenderPreset::BalancedSystem: return "Balanced System";
        case RenderPreset::VisualUltra: return "Visual Ultra";
        case RenderPreset::GpuTorture: return "GPU Torture";
        case RenderPreset::CpuTorture: return "CPU Torture";
        case RenderPreset::Custom: return "Custom";
    }
    return "Custom";
}

std::optional<RenderPreset> parse_render_preset(std::string_view value) {
    const std::string v = normalized(value);
    if (v == "balanced" || v == "balanced-system") return RenderPreset::BalancedSystem;
    if (v == "visual-ultra" || v == "ultra") return RenderPreset::VisualUltra;
    if (v == "gpu-torture") return RenderPreset::GpuTorture;
    if (v == "cpu-torture") return RenderPreset::CpuTorture;
    if (v == "custom") return RenderPreset::Custom;
    return std::nullopt;
}

void apply_render_preset(AppConfig& c, RenderPreset preset) {
    if (preset == RenderPreset::Custom) {
        c.render_preset = RenderPreset::Custom;
        c.quality_preset_label = "Custom";
        return;
    }

    apply_balanced(c);
    if (preset == RenderPreset::VisualUltra || preset == RenderPreset::GpuTorture) {
        c.tessellation.enabled = true;
        c.tessellation.scale = 1.5f;
        c.tessellation.factor = 1.5f;
        c.tessellation.distance = 1.5f;
        c.quality.soft_shadow_level = QualityLevel::High;
        c.quality.contact_shadow_level = QualityLevel::High;
        c.quality.msaa_samples = 4;
        c.quality.cloud_quality = QualityLevel::High;
        c.quality.ssgi_quality = QualityLevel::High;
        c.quality.specular_aa_level = QualityLevel::High;
        c.quality.ao_quality = QualityLevel::High;
        c.quality.taa_quality = QualityLevel::High;
        c.quality.sun_shadow_map_size = 4096;
    }
    if (preset == RenderPreset::VisualUltra) {
        c.quality.shadow_light_budget = 4;
        c.quality.point_shadow_map_size = 512;
        c.quality.foliage_density =
            std::min(1.5f, c.quality.foliage_density_max);
        c.quality.bloom_quality = QualityLevel::Medium;
        c.quality.dof_quality = QualityLevel::Medium;
    } else if (preset == RenderPreset::GpuTorture) {
        c.quality.shadow_light_budget = c.quality.shadow_light_budget_max;
        c.quality.point_shadow_map_size = 1024;
        c.quality.foliage_density = c.quality.foliage_density_max;
        c.quality.bloom_quality = QualityLevel::High;
        c.quality.dof_quality = QualityLevel::High;
    } else if (preset == RenderPreset::CpuTorture) {
        c.cpu_stress.enabled = true;
        c.cpu_stress.workers = 0;
        c.cpu_stress.duration_seconds = 10.0f;
    }

    c.render_preset = preset;
    c.quality_preset_label = std::string(render_preset_label(preset));
}

RenderPreset detect_render_preset(const AppConfig& config) {
    if (config.render_preset == RenderPreset::Custom)
        return RenderPreset::Custom;
    AppConfig candidate = config;
    apply_render_preset(candidate, config.render_preset);
    return same_owned_fields(config, candidate)
        ? config.render_preset
        : RenderPreset::Custom;
}

std::string_view material_workflow_id(MaterialWorkflow workflow) noexcept {
    return workflow == MaterialWorkflow::SpecularGlossinessExperimental
        ? "specular-glossiness" : "metallic-roughness";
}

std::optional<MaterialWorkflow> parse_material_workflow(
    std::string_view value) {
    const std::string v = normalized(value);
    if (v == "metallic-roughness" || v == "compat")
        return MaterialWorkflow::MetallicRoughnessCompat;
    if (v == "specular-glossiness" || v == "spec-gloss")
        return MaterialWorkflow::SpecularGlossinessExperimental;
    return std::nullopt;
}

} // namespace heaven
