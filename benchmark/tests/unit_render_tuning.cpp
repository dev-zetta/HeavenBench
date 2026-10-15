/*
 * HeavenBench — centralized renderer-tuning interface checks
 */
#include "render_tuning.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef MATERIAL_TUNING_BINDINGS_SRC_PATH
#error "render tuning source paths must be supplied by CMake"
#endif

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool require_contains(const std::filesystem::path& path,
                      std::string_view needle) {
    const std::string source = read_file(path);
    if (!source.empty() && source.find(needle) != std::string::npos) {
        return true;
    }
    std::printf("[unit_render_tuning] FAILED: %s lacks '%.*s'\n",
                path.string().c_str(), static_cast<int>(needle.size()), needle.data());
    return false;
}

bool require_material_parameter(const std::filesystem::path& path,
                                std::string_view name,
                                std::string_view type) {
    const std::string source = read_file(path);
    const std::string name_token = "name : " + std::string(name);
    const auto name_pos = source.find(name_token);
    const auto begin = name_pos == std::string::npos
        ? std::string::npos : source.rfind('{', name_pos);
    const auto end = name_pos == std::string::npos
        ? std::string::npos : source.find('}', name_pos);
    const std::string type_token = "type : " + std::string(type);
    if (begin != std::string::npos && end != std::string::npos &&
        source.substr(begin, end - begin).find(type_token) !=
            std::string::npos) {
        return true;
    }
    std::printf(
        "[unit_render_tuning] FAILED: %s lacks parameter '%.*s' of type '%.*s'\n",
        path.string().c_str(), static_cast<int>(name.size()), name.data(),
        static_cast<int>(type.size()), type.data());
    return false;
}

bool test_render_tuning() {
    using namespace heaven::render_tuning;
    bool ok = true;

    ok &= clamp_active_count(0, 64) == 1;
    ok &= clamp_active_count(32, 64) == 32;
    ok &= clamp_active_count(128, 64) == 64;
    ok &= godrays.maximum_samples == shader_limits.godray_samples;
    ok &= volumetric_fog.maximum_lights ==
          shader_limits.volumetric_fog_lights;
    ok &= (ibl.face_size & (ibl.face_size - 1u)) == 0u;
    ok &= (1u << (ibl.mip_levels - 1u)) == ibl.face_size;
    ok &= shader_limits.rain_streaks == 60000;
    ok &= shader_limits.rain_splashes == 4096;
    ok &= shader_limits.grass_blade_segments == 8;
    ok &= fullscreen.scene_far_depth > fullscreen.volumetric_fog_distance;
    ok &= material_surface.ground_relief.x > 1.0f;
    ok &= material_surface.ground_relief.y >= 0.5f;
    ok &= material_surface.ground_relief.z <
          material_surface.dielectric.z;
    ok &= material_surface.ground_relief.y <
          material_surface.ground_relief.z;
    ok &= material_surface.ground_relief_cavity.z > 0.0f &&
          material_surface.ground_relief_cavity.z <= 0.18f;
    ok &= material_surface.normal_map_strength == 1.50f;

    const std::filesystem::path materials(FILAMENT_MATERIALS_SRC_DIR);
    const std::vector<std::string> lit_families = {
        "heaven_lit.mat", "heaven_lit_twosided.mat",
        "heaven_lit_doublesided.mat", "heaven_lit_clearcoat.mat",
        "heaven_lit_aniso.mat", "heaven_lit_colored_penumbra.mat",
        "heaven_subsurface.mat", "heaven_cloth.mat",
        "heaven_foliage_shadow.mat", "heaven_window.mat",
    };
    for (const auto& file : lit_families) {
        ok &= require_material_parameter(
            materials / file, "surfaceSamplingTuning", "float4");
        ok &= require_material_parameter(
            materials / file, "surfaceWetnessTuning", "float4");
        ok &= require_material_parameter(
            materials / file, "normalStrength", "float");
        ok &= require_contains(
            materials / file, "n.xy * materialParams.normalStrength");
    }

    ok &= require_material_parameter(
        materials / "heaven_glass.mat", "normalStrength", "float");
    ok &= require_contains(
        materials / "heaven_glass.mat",
        "n.xy * materialParams.normalStrength");

    for (const auto* file : {
             "heaven_lit.mat", "heaven_lit_colored_penumbra.mat"}) {
        ok &= require_material_parameter(
            materials / file, "groundReliefSurface", "bool");
        ok &= require_material_parameter(
            materials / file, "groundReliefTuning", "float4");
        ok &= require_material_parameter(
            materials / file, "groundReliefCavityTuning", "float3");
        ok &= require_contains(materials / file,
                               "material.ambientOcclusion = 1.0 - cavity");
    }

    struct MaterialInterface {
        const char* file;
        const char* name;
        const char* type;
    };
    const MaterialInterface interfaces[] = {
        {"heaven_grass.mat", "grassWindTuning", "float4"},
        {"heaven_grass.mat", "grassTipTuning", "float3"},
        {"heaven_grass.mat", "flowerTintTuning", "float4"},
        {"heaven_grass_depth.mat", "grassWindTuning", "float4"},
        {"heaven_clouds.mat", "cloudShapeTuning", "float4"},
        {"heaven_clouds.mat", "cloudDetailTuning", "float4"},
        {"heaven_volumetric_clouds.mat", "cloudDensityTuning", "float4"},
        {"heaven_godrays.mat", "activeSamples", "int"},
        {"heaven_volumetric_fog.mat", "fogShaderTuning", "float4"},
        {"heaven_volumetric_fog.mat", "activeSteps", "int"},
        {"heaven_ssgi.mat", "ssgiStabilityTuning", "float4"},
        {"heaven_smoke.mat", "particleMotionTuning", "float4"},
        {"heaven_flame.mat", "particleShapeTuning", "float4"},
        {"heaven_glow.mat", "particleShapeTuning", "float4"},
        {"heaven_firefly.mat", "particleMotionTuning", "float4"},
        {"heaven_heat_haze.mat", "hazeShapeTuning", "float4"},
        {"heaven_rain.mat", "rainShaderTuning", "float4"},
        {"heaven_splash.mat", "splashShapeTuning", "float4"},
        {"heaven_lens_flare.mat", "flareShaderTuning", "float4"},
        {"heaven_window.mat", "windowGlassTuning", "float4"},
        {"heaven_sky.mat", "skyAtmosphereTuning", "float4"},
    };
    for (const auto& interface : interfaces) {
        ok &= require_material_parameter(
            materials / interface.file, interface.name, interface.type);
    }

    ok &= require_contains(materials / "heaven_godrays.mat",
                           "const int MAX_SAMPLES = 64");
    ok &= require_contains(materials / "heaven_volumetric_fog.mat",
                           "type : float4[16], name : lightPosRadius");
    ok &= require_contains(materials / "heaven_volumetric_fog.mat",
                           "const int MAX_STEPS = 32");
    ok &= require_contains(materials / "includes" / "effect_volumetric_clouds_fragment.glsl",
                           "), 128)");

    const std::filesystem::path binders(MATERIAL_TUNING_BINDINGS_SRC_PATH);
    for (const auto& interface : interfaces) {
        ok &= require_contains(
            binders, "setParameter(\"" + std::string(interface.name) + "\"");
    }

    ok &= require_contains(MATERIAL_INSTANCE_CACHE_SRC_PATH,
                           "bind_standard_surface(*mi)");
    ok &= require_contains(MATERIAL_INSTANCE_CACHE_SRC_PATH,
                           "bind_ground_relief(*mi)");
    ok &= require_contains(MATERIAL_INSTANCE_CACHE_SRC_PATH,
                           "bind_window(*mi)");
    ok &= require_contains(MATERIAL_INSTANCE_CACHE_SRC_PATH,
                           "bind_glass(*mi)");
    ok &= require_contains(
        MATERIAL_TUNING_BINDINGS_SRC_PATH,
        "setParameter(\"normalStrength\"");
    ok &= require_contains(
        GRASS_DEPTH_PROXY_SRC_PATH, "bind_grass_depth(*mi_)");

    const std::filesystem::path features(RENDER_FEATURES_SRC_DIR);
    const std::vector<std::pair<std::string, std::string>> family_paths = {
        {"grass_feature.cpp", "bind_grass_visible(*mi_)"},
        {"sky_feature.cpp", "bind_sky(*mi_)"},
        {"massive_foliage_feature.cpp", "bind_grass_visible(*grass_mi_)"},
        {"massive_foliage_feature.cpp", "bind_grass_visible(*flower_mi_)"},
        {"cloud_feature.cpp", "bind_clouds(*mi_)"},
        {"godrays_feature.cpp", "bind_godrays(*mi_)"},
        {"volfog_feature.cpp", "bind_volumetric_fog(*mi_)"},
        {"volumetric_cloud_feature.cpp", "bind_volumetric_clouds(*mi_)"},
        {"ssgi_feature.cpp", "bind_ssgi(*mi_)"},
        {"rain_feature.cpp", "bind_rain(*rain_mi_)"},
        {"rain_feature.cpp", "bind_splash(*splash_mi_)"},
        {"lens_flare_feature.cpp", "bind_lens_flare(*lf.mi)"},
        {"particle_feature.cpp", "bind_smoke(*se.mi)"},
        {"particle_feature.cpp", "bind_flame(*se.mi)"},
        {"particle_feature.cpp", "bind_glow(*se.mi)"},
        {"particle_feature.cpp", "bind_firefly(*se.mi)"},
        {"particle_feature.cpp", "bind_heat_haze(*haze.mi)"},
    };
    for (const auto& [file, binder] : family_paths) {
        ok &= require_contains(features / file, binder);
    }

    if (!ok) {
        std::printf("[unit_render_tuning] centralized tuning checks failed\n");
    }
    return ok;
}

heaven::testing::Register register_render_tuning(test_render_tuning);

} // namespace
