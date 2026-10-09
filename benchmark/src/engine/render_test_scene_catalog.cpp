#include "engine/render_test_scene_catalog.h"

#include "engine/types.h"

#include <algorithm>
#include <array>

namespace heaven {
namespace {

constexpr std::array<float, 5> kOverview{7.0f, -10.0f, 6.0f, 120.2564f, -19.7989f};
constexpr std::array<float, 5> kClose{4.5f, -6.0f, 3.5f, 119.3578f, -17.8024f};
constexpr std::array<RenderTestSceneInfo, 22> kScenes{{
    {"contact-shadow", "Contact shadows", "Flat and curved receivers, bevels and nearby thin blockers.", kClose, 50.0f, 8.0f},
    {"shadow-cascade", "Sun shadows", "Near and distant casters on a long ground plane.", {9.0f, -14.0f, 8.0f, 112.2490f, -13.3852f}, 50.0f, 9.0f},
    {"material-models", "Material models", "Matte, chrome, clear coat, brushed metal, cloth and thin organic surfaces.", kOverview},
    {"transparency", "Transparency", "Glass, a masked window, cutout cards and colored objects behind them.", kOverview, 50.0f, 10.0f},
    {"displacement", "Displacement", "The same scalar height field on coarse and dense patches, beside a flat POM control.", kClose, 50.0f, 8.0f},
    {"anti-aliasing", "Anti-aliasing", "Thin slats, diagonal edges, cutouts and glossy curved surfaces.", kOverview},
    {"ambient-occlusion", "Ambient occlusion", "Open space, floor contact, tight gaps and an inside corner.", kClose},
    {"reflections", "Reflections and GI", "Glossy receivers, chrome, emissive and colored walls.", kOverview},
    {"lighting", "Local lighting", "Colored point lights, a spotlight, blockers and a visible lamp.", kOverview, 50.0f, 0.0f},
    {"bloom", "Bloom and flares", "Bright emissive sources beside dark and mid-gray references.", kOverview, 50.0f, 0.0f},
    {"depth-of-field", "Depth of field", "Textured targets at 1.5, twenty and thirty-three metres.", {0.0f, -8.0f, 2.0f, 90.0f, 0.0f}},
    {"atmosphere", "Atmosphere", "A low sun, silhouette towers and local lights through fog and clouds.", {0.0f, -12.0f, 3.0f, 90.0f, 5.0f}, 50.0f, 8.0f},
    {"foliage", "Foliage", "Explicit procedural grass roots, flowers, leaf cards and a shadow caster.", kClose, 50.0f, 10.0f},
    {"particles", "Particles and heat haze", "Smoke and flame emitters against a striped background.", kOverview, 50.0f, 18.0f},
    {"rain", "Rain and wet surfaces", "Rain and splashes above matte and glossy sloped receivers.", kOverview, 50.0f, 14.0f},
    {"animation", "Animation and instancing", "Repeated static objects and a procedural two-bone animated ribbon.", kOverview},
    {"output", "Output and camera", "Color and gray references, bright emission and glossy highlights.", kOverview},
    {"asset-contact-shadow", "Heaven roof and stones", "Authored house2 roof, stone walls and a curved rock with their original materials.", {14.0f, -18.0f, 12.0f, 121.4296f, -19.5727f}, 50.0f, 8.0f},
    {"asset-foliage", "Heaven foliage", "An authored grass island and fir with original cutout textures.", {32.0f, -40.0f, 32.0f, 128.6598f, -26.9128f}, 50.0f, 10.0f},
    {"asset-windows-lamps", "Heaven windows and lamps", "Authored house windows, lamp glass and a standalone dusk light.", {5.0f, -16.0f, 7.0f, 110.5560f, -13.1764f}, 50.0f, 18.5f},
    {"asset-animation", "Heaven animation", "Original mill wing skeleton and wind-driven flag cloth.", {17.0f, -30.0f, 17.0f, 116.5651f, -11.7891f}, 50.0f, 12.0f},
    {"asset-airship", "Heaven airship rim", "Original metal porthole rim, cannon and hull at a fixed close view.", {-7.388115f, 4.675559f, 3.088985f, -46.880970f, -14.197375f}, 60.0f, 7.127434f},
}};

constexpr std::array<std::string_view, 2> kContactNodes{
    "heaven/nodes/house2.node", "heaven/nodes/stone1.node"};
constexpr std::array<std::string_view, 2> kFoliageNodes{
    "heaven/nodes/fly_island2.node", "heaven/nodes/fir1.node"};
constexpr std::array<std::string_view, 3> kWindowNodes{
    "heaven/nodes/house2.node", "heaven/nodes/lamp_post.node",
    "heaven/nodes/lamp_post_light.node"};
constexpr std::array<std::string_view, 2> kAnimationNodes{
    "heaven/nodes/mill.node", "heaven/nodes/flag.node"};
constexpr std::array<std::string_view, 1> kAirshipNodes{
    "heaven/nodes/airship.node"};

FeatureToggles isolated_features() {
    FeatureToggles f;
    f.grass = f.particles = f.god_rays = f.volumetric_fog = false;
    f.clouds = f.sky = f.lens_flares = f.heat_haze = false;
    f.rain = f.wet_reflections = f.bloom = f.ambient_occlusion = false;
    f.taa = f.depth_of_field = f.soft_shadows = f.contact_shadows = false;
    f.point_light_shadows = f.msaa = f.refraction = f.reflections = false;
    f.volumetric_clouds = f.ssgi = f.massive_foliage = f.ground_relief = false;
    f.clear_coat = f.anisotropic = f.subsurface = f.cloth = false;
    f.transparent_shadows = f.colored_penumbra = f.specular_aa = false;
    f.color_grading = f.physical_camera = f.dynamic_resolution = false;
    f.gpu_instancing = f.gpu_skinning = false;
    return f;
}

} // namespace

std::span<const RenderTestSceneInfo> render_test_scene_catalog() {
    return kScenes;
}

std::span<const std::string_view> render_test_scene_asset_nodes(std::string_view id) {
    if (id == "asset-contact-shadow") return kContactNodes;
    if (id == "asset-foliage") return kFoliageNodes;
    if (id == "asset-windows-lamps") return kWindowNodes;
    if (id == "asset-animation") return kAnimationNodes;
    if (id == "asset-airship") return kAirshipNodes;
    return {};
}

bool render_test_scene_uses_assets(std::string_view id) {
    return !render_test_scene_asset_nodes(id).empty();
}

std::array<float, 3> render_test_scene_asset_translation(
    std::string_view id, std::string_view node_path) {
    if (id == "asset-contact-shadow" && node_path == "heaven/nodes/stone1.node")
        return {8.0f, 0.0f, 4.8f};
    if (id == "asset-foliage" && node_path == "heaven/nodes/fir1.node")
        return {0.0f, 0.0f, 6.675f};
    if (id == "asset-windows-lamps" &&
        (node_path == "heaven/nodes/lamp_post.node" ||
         node_path == "heaven/nodes/lamp_post_light.node"))
        return {-6.0f, -1.0f, 0.0f};
    if (id == "asset-animation" && node_path == "heaven/nodes/flag.node")
        return {7.0f, 0.0f, 7.0f};
    return {};
}

bool apply_render_test_scene_defaults(AppConfig& config, std::string_view id) {
    const auto it = std::ranges::find(kScenes, id, &RenderTestSceneInfo::id);
    if (it == kScenes.end()) return false;
    config.render_test_scene = std::string(id);

    auto f = isolated_features();
    if (id == "contact-shadow" || id == "asset-contact-shadow") {
        f.contact_shadows = f.soft_shadows = f.colored_penumbra = f.taa = true;
    } else if (id == "shadow-cascade") {
        f.soft_shadows = f.taa = true;
    } else if (id == "material-models") {
        f.clear_coat = f.anisotropic = f.subsurface = f.cloth = true;
        f.refraction = f.colored_penumbra = f.specular_aa = f.taa = true;
    } else if (id == "transparency") {
        f.refraction = f.transparent_shadows = f.soft_shadows = f.taa = true;
    } else if (id == "displacement") {
        f.ground_relief = f.contact_shadows = f.soft_shadows = f.taa = true;
    } else if (id == "anti-aliasing") {
        f.msaa = f.taa = f.specular_aa = true;
    } else if (id == "ambient-occlusion") {
        f.ambient_occlusion = f.taa = true;
    } else if (id == "reflections") {
        f.reflections = f.ssgi = f.wet_reflections = f.taa = true;
    } else if (id == "lighting") {
        f.point_light_shadows = f.lens_flares = f.volumetric_fog = f.taa = true;
    } else if (id == "bloom") {
        f.bloom = f.lens_flares = f.taa = true;
    } else if (id == "depth-of-field") {
        f.depth_of_field = f.physical_camera = f.taa = true;
    } else if (id == "atmosphere") {
        f.sky = f.clouds = f.volumetric_clouds = f.god_rays = true;
        f.volumetric_fog = f.point_light_shadows = f.taa = true;
    } else if (id == "foliage") {
        f.grass = f.massive_foliage = f.subsurface = f.refraction = true;
        f.transparent_shadows = f.soft_shadows = f.taa = true;
    } else if (id == "particles") {
        f.particles = f.heat_haze = f.bloom = f.taa = true;
    } else if (id == "rain") {
        f.rain = f.wet_reflections = f.reflections = f.taa = true;
    } else if (id == "animation") {
        f.gpu_instancing = f.gpu_skinning = f.cloth = f.taa = true;
    } else if (id == "output") {
        f.color_grading = f.physical_camera = f.dynamic_resolution = true;
        f.bloom = f.specular_aa = f.taa = true;
    } else if (id == "asset-foliage") {
        f.grass = f.transparent_shadows = f.soft_shadows = f.taa = true;
    } else if (id == "asset-windows-lamps") {
        f.refraction = f.point_light_shadows = f.bloom = f.taa = true;
    } else if (id == "asset-animation") {
        f.gpu_skinning = f.cloth = f.soft_shadows = f.taa = true;
    }
    config.features = f;
    config.tessellation.enabled = !render_test_scene_uses_assets(id) ||
                                  id == "asset-contact-shadow";
    config.tessellation.scale = 1.0f;
    config.tessellation.factor = 1.0f;
    config.tessellation.distance = 1.0f;
    config.quality = QualityKnobs{};
    config.grading = ColorGradingConfig{};
    config.physical_camera = PhysicalCameraConfig{};
    if (id == "output") {
        config.grading.temperature = 0.15f;
        config.grading.contrast = 1.15f;
        config.grading.saturation = 1.15f;
    }
    if (id == "depth-of-field") {
        config.physical_camera.aperture = 1.4f;
        config.physical_camera.shutter_speed = 1.0f / 8000.0f;
    }
    config.heat_haze_enabled = f.heat_haze;
    config.time_of_day = it->time_of_day;
    config.texture_quality = TextureQuality::Original;
    config.weather.rain_intensity = id == "rain" ? 0.8f : 0.0f;
    if (id == "reflections") config.weather.rain_intensity = 0.25f;
    config.music.enabled = false;
    config.quality.msaa_samples = 4;
    return true;
}

} // namespace heaven
