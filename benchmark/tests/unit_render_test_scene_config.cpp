/* Scene precedence, authored selection and generated-only asset isolation. */
#include "engine/asset_file_system.h"
#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/render_test_scene_catalog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}

namespace {
bool run_render_test_scene_config() {
    using namespace heaven;
    const auto check = [](bool ok, const char* message) {
        if (!ok) std::fprintf(stderr, "[render_test_scene_config] %s\n", message);
        return ok;
    };
    bool ok = true;
    const std::array<std::string_view, 4> args{"heaven_benchmark", "--test-scene", "contact-shadow", "--no-contact-shadows"};
    const auto parsed = parse_cli(args);
    ok &= check(parsed.config.render_test_scene == "contact-shadow" &&
                !parsed.config.features.contact_shadows && parsed.diagnostics.empty(),
                "later CLI override must beat fixture defaults");
    BenchConfig overlaid;
    overlaid.features.grass = true;
    apply_cli_overlay(overlaid, parsed);
    ok &= check(!overlaid.features.grass && !overlaid.features.contact_shadows,
                "ordered CLI overlay must apply isolation against file/default base");
    const std::array<std::string_view, 3> bad{"heaven_benchmark", "--test-scene", "missing-fixture"};
    ok &= check(parse_cli(bad).test_scene_invalid, "unknown scene must fail before rendering");
    const std::array<std::string_view, 2> missing{"heaven_benchmark", "--test-scene"};
    ok &= check(parse_cli(missing).test_scene_invalid, "missing scene must fail before rendering");
    const std::array<std::string_view, 2> listing{"heaven_benchmark", "--list-test-scenes"};
    const auto list = parse_cli(listing);
    ok &= check(list.list_test_scenes_requested && !list.help_requested,
                "listing must be a separate startup action");

    std::set<std::string_view> catalog_ids;
    std::size_t authored_count = 0;
    for (const auto& info : render_test_scene_catalog()) {
        ok &= check(catalog_ids.insert(info.id).second, "catalog IDs must be unique");
        ok &= check(std::ranges::all_of(info.camera_pose, [](float value) { return std::isfinite(value); }) &&
                    info.fov > 0 && info.fov < 180 && info.time_of_day >= 0 && info.time_of_day < 24,
                    "catalog camera and hour must be finite and usable");
        const auto nodes = render_test_scene_asset_nodes(info.id);
        ok &= check(render_test_scene_uses_assets(info.id) == !nodes.empty(),
                    "authored predicate and selected roots disagree");
        if (nodes.empty()) continue;
        ++authored_count;
        std::set<std::string_view> unique_nodes;
        for (const auto node : nodes) {
            ok &= check(node.starts_with("heaven/nodes/") && node.ends_with(".node") &&
                        node.find("..") == std::string_view::npos &&
                        unique_nodes.insert(node).second,
                        "authored roots must be unique logical node paths");
            const auto offset = render_test_scene_asset_translation(info.id, node);
            ok &= check(std::ranges::all_of(offset, [](float value) { return std::isfinite(value); }),
                        "authored fixture translation must be finite");
        }
        AppConfig asset_config;
        asset_config.tessellation.enabled = true;
        asset_config.features.grass = asset_config.features.gpu_skinning = true;
        ok &= check(apply_render_test_scene_defaults(asset_config, info.id) &&
                    asset_config.render_test_scene == info.id &&
                    asset_config.tessellation.enabled == (info.id == "asset-contact-shadow") &&
                    asset_config.features.grass == (info.id == "asset-foliage") &&
                    asset_config.features.gpu_skinning == (info.id == "asset-animation"),
                    "authored defaults must isolate workload and feature inputs");
    }
    ok &= check(catalog_ids.size() == 22 && authored_count == 5 &&
                !render_test_scene_uses_assets("contact-shadow") &&
                !render_test_scene_uses_assets("missing-fixture") &&
                render_test_scene_asset_nodes("missing-fixture").empty(),
                "procedural catalog and unknown IDs must remain asset independent");
    const auto contact_nodes = render_test_scene_asset_nodes("asset-contact-shadow");
    ok &= check(contact_nodes.size() == 2 && contact_nodes.front() == "heaven/nodes/house2.node" &&
                contact_nodes.back() == "heaven/nodes/stone1.node",
                "contact fixture must retain the reported authored roof and stone sources");
    ok &= check(render_test_scene_asset_translation("asset-contact-shadow", contact_nodes.front()) ==
                    std::array<float, 3>{} &&
                render_test_scene_asset_translation("asset-contact-shadow", contact_nodes.back()) ==
                    std::array<float, 3>{8, 0, 4.8f} &&
                render_test_scene_asset_translation("asset-windows-lamps", "heaven/nodes/lamp_post.node") ==
                    render_test_scene_asset_translation("asset-windows-lamps", "heaven/nodes/lamp_post_light.node") &&
                render_test_scene_asset_translation("asset-foliage", "heaven/nodes/fly_island2.node") ==
                    std::array<float, 3>{} &&
                render_test_scene_asset_translation("asset-foliage", "heaven/nodes/fir1.node") ==
                    std::array<float, 3>{0, 0, 6.675f},
                "fixture placements must preserve house/island origins, align lamps and seat the fir");
    const std::array<std::string_view, 4> asset_args{
        "heaven_benchmark", "--test-scene", "asset-animation", "--no-gpu-skinning"};
    const auto asset_parsed = parse_cli(asset_args);
    ok &= check(!asset_parsed.test_scene_invalid && asset_parsed.diagnostics.empty() &&
                asset_parsed.config.render_test_scene == "asset-animation" &&
                !asset_parsed.config.features.gpu_skinning,
                "authored scene selection must allow an explicit CPU skinning control");
    const auto airship_nodes = render_test_scene_asset_nodes("asset-airship");
    ok &= check(airship_nodes.size() == 1 && airship_nodes.front() == "heaven/nodes/airship.node" &&
                render_test_scene_asset_translation("asset-airship", airship_nodes.front()) ==
                    std::array<float, 3>{},
                "airship fixture must load the original group without a placement override");
    AppConfig airship_config;
    ok &= check(apply_render_test_scene_defaults(airship_config, "asset-airship") &&
                !airship_config.tessellation.enabled &&
                airship_config.texture_quality == TextureQuality::Original &&
                !airship_config.features.soft_shadows && !airship_config.features.contact_shadows &&
                !airship_config.features.taa && !airship_config.features.ssgi &&
                !airship_config.features.reflections && !airship_config.features.refraction &&
                !airship_config.features.bloom && !airship_config.features.ambient_occlusion &&
                !airship_config.features.god_rays && !airship_config.features.volumetric_fog &&
                !airship_config.features.heat_haze && !airship_config.heat_haze_enabled &&
                !airship_config.features.particles && !airship_config.features.grass &&
                !airship_config.features.gpu_instancing && !airship_config.features.gpu_skinning &&
                airship_config.weather.rain_intensity == 0,
                "airship must isolate original material/normal rendering and basic Sun shadows");

    const auto path = std::filesystem::temp_directory_path() / "heaven-render-test-scene-config.json";
    struct Remove { std::filesystem::path path; ~Remove() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
    {
        std::ofstream file(path);
        file << R"({"features.contact_shadows":false,"render_test_scene":"contact-shadow","time_of_day":11})";
    }
    const auto loaded = load_config_file(path.string(), BenchConfig{});
    ok &= check(loaded.parsed_ok && loaded.diagnostics.empty() &&
                loaded.config.render_test_scene == "contact-shadow" &&
                !loaded.config.features.contact_shadows && !loaded.config.features.grass &&
                loaded.config.time_of_day == 11.0f,
                "explicit JSON fields must beat fixture defaults regardless of key order");
    {
        std::ofstream file(path);
        file << R"({"features.grass":false,"render_test_scene":"asset-foliage","data_dir":"/external/heaven","time_of_day":9})";
    }
    const auto loaded_asset = load_config_file(path.string(), BenchConfig{});
    ok &= check(loaded_asset.parsed_ok && loaded_asset.diagnostics.empty() &&
                loaded_asset.config.render_test_scene == "asset-foliage" &&
                !loaded_asset.config.features.grass && !loaded_asset.config.tessellation.enabled &&
                loaded_asset.config.data_dir == "/external/heaven" && loaded_asset.config.time_of_day == 9,
                "authored defaults must preserve explicit external data and feature overrides");
    {
        std::ofstream file(path);
        file << R"({"render_test_scene":"asset-airship","data_dir":"/external/heaven"})";
    }
    const auto loaded_airship = load_config_file(path.string(), BenchConfig{});
    {
        std::ofstream file(path);
        file << R"({"features.soft_shadows":true,"render_test_scene":"asset-airship","data_dir":"/external/heaven"})";
    }
    const auto loaded_airship_soft = load_config_file(path.string(), BenchConfig{});
    ok &= check(loaded_airship.parsed_ok && loaded_airship.diagnostics.empty() &&
                loaded_airship_soft.parsed_ok && loaded_airship_soft.diagnostics.empty() &&
                !loaded_airship.config.features.soft_shadows && loaded_airship_soft.config.features.soft_shadows,
                "explicit soft shadows must override the isolated airship defaults");
    ok &= check(write_config_file(path.string(), loaded_airship.config).empty(),
                "cannot serialize airship default control");
    std::ifstream airship_default_file(path);
    const std::string airship_default_fields{std::istreambuf_iterator<char>{airship_default_file},
                                            std::istreambuf_iterator<char>{}};
    airship_default_file.close();
    auto normalized_airship_soft = loaded_airship_soft.config;
    normalized_airship_soft.features.soft_shadows = false;
    ok &= check(write_config_file(path.string(), normalized_airship_soft).empty(),
                "cannot serialize airship soft-shadow control");
    std::ifstream airship_soft_file(path);
    const std::string airship_soft_fields{std::istreambuf_iterator<char>{airship_soft_file},
                                         std::istreambuf_iterator<char>{}};
    ok &= check(!airship_default_fields.empty() && airship_default_fields == airship_soft_fields,
                "soft-shadow airship contrast must change no other effective setting");

    auto memory = AssetFileSystem::create_memory("benchmark");
    ok &= check(memory.has_value(), "memory namespace creation");
    if (memory) {
        ok &= check(!memory->exists("benchmark/CMakeLists.txt") &&
                    !memory->open_read("benchmark/CMakeLists.txt"),
                    "missing generated assets must never fall through to real files");
        auto bytes = std::make_shared<AssetBuffer>(3, std::uint8_t{0x42});
        ok &= check(memory->mount_memory("benchmark/textures/one.dds", bytes).has_value(),
                    "generated texture mount");
        ok &= check(memory->exists("textures/one.dds") &&
                    memory->find("benchmark/textures", ".dds").size() == 1 &&
                    memory->read_all("benchmark/textures/one.dds")->size() == 3,
                    "generated lookup aliases and deterministic search");
        ok &= check(!memory->mount_memory("benchmark/textures/one.dds", bytes),
                    "duplicate generated path must be rejected");
        ok &= check(!memory->logical_path("../outside"), "generated namespace traversal must be rejected");
    }
    return ok;
}
const heaven::testing::Register g_register_render_test_scene_config{&run_render_test_scene_config};
}
