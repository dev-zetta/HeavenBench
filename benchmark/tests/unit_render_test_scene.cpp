#include "engine/render_test_scene.h"
#include "engine/render_test_scene_catalog.h"
#include "engine/material_model_ref.h"
#include "dds_loader_cpu.h"

#include <cmath>
#include <functional>
#include <set>
#include <utility>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

class ProceduralBackend final : public heaven::IRhiBackend {
public:
    explicit ProceduralBackend(const heaven::AssetFileSystem& assets) : assets_(assets) {}
    heaven::GraphicsApi api() const noexcept override { return heaven::GraphicsApi::Filament; }
    std::string device_name() const override { return "CPU procedural test"; }
    std::string api_version() const override { return {}; }
    heaven::RendererFeatureStats renderer_feature_stats() const override { return {}; }
    heaven::GpuMesh upload_mesh(const heaven::Mesh& mesh) override {
        heaven::GpuMesh gpu;
        gpu.name = mesh.name; gpu.path = mesh.path;
        gpu.bbox_min = mesh.bbox_min; gpu.bbox_max = mesh.bbox_max;
        for (const auto& s : mesh.surfaces) {
            gpu.surfaces.push_back({next_handle_++, static_cast<std::uint32_t>(s.indices.size()), s.name,
                mesh.bbox_min, mesh.bbox_max});
        }
        return gpu;
    }
    heaven::GpuMesh upload_skinned(const heaven::SkinnedMesh& mesh) override {
        ++skinned_uploads;
        heaven::Mesh bind;
        bind.name = mesh.name; bind.path = mesh.path;
        for (int i = 0; std::cmp_less(i, mesh.surfaces.size()); ++i) {
            bind.surfaces.push_back({mesh.surfaces[i].name, mesh.skin_frame(i, 0), mesh.surfaces[i].indices});
        }
        return upload_mesh(bind);
    }
    std::uint64_t load_texture(const std::string& path, heaven::TextureSemantic semantic) override {
        if (!heaven::load_dds_cpu(path, std::cref(assets_))) return 0;
        semantics.insert(semantic);
        return next_handle_++;
    }
    heaven::Result<void> init_pipelines(const heaven::AppConfig& /*config*/) override { return {}; }
    heaven::Result<void> set_render_test_ground(std::size_t index, glm::vec2 min, glm::vec2 max, float z) override {
        ground_valid = index == 0 && min.x < max.x && min.y < max.y && std::isfinite(z);
        return {};
    }
    heaven::FrameResult render_frame(std::span<const heaven::Instance> /*instances*/,
        std::span<const int> /*dirty_instance_indices*/, std::span<const heaven::GpuMesh> /*meshes*/,
        const heaven::MaterialResolver& /*resolve_material*/, const heaven::Camera& /*camera*/,
        const heaven::LightingParams& /*lighting*/, const heaven::WeatherState& /*weather*/,
        const heaven::TonemapParams& /*tonemap*/, int /*viewport_w*/, int /*viewport_h*/) override { return {}; }
    void present() override {}
    heaven::Result<void> update_mesh_vertices(std::uint64_t /*surface_handle*/,
        const heaven::Vertex* /*vertices*/, std::uint32_t /*vertex_count*/) override { return {}; }
    std::set<heaven::TextureSemantic> semantics;
    bool ground_valid = false;
    int skinned_uploads = 0;
private:
    const heaven::AssetFileSystem& assets_;
    std::uint64_t next_handle_ = 1;
};

bool check(bool value, const char* message) {
    if (!value) std::fprintf(stderr, "render test scene: %s\n", message);
    return value;
}

bool procedural_scenes_have_valid_geometry_and_mounted_dds() {
    using namespace heaven;
    std::set<std::string_view> ids;
    for (const auto& info : render_test_scene_catalog()) {
        if (!check(ids.insert(info.id).second, "duplicate catalog ID")) return false;
        if (render_test_scene_uses_assets(info.id)) {
            if (!check(!make_render_test_scene(info.id), "authored scene must reject procedural generation")) return false;
            continue;
        }
        auto definition = make_render_test_scene(info.id);
        if (!check(bool(definition), "cannot generate catalog scene")) return false;
        auto assets = AssetFileSystem::create_memory("render-test-unit");
        if (!check(bool(assets), "cannot create memory asset context")) return false;
        if (!check(bool(mount_render_test_scene_assets(*definition, *assets)), "cannot mount procedural DDS")) return false;
        for (const auto& texture : definition->textures) {
            const auto dds = load_dds_cpu(texture.path, std::cref(*assets));
            if (!check(bool(dds) && dds.width == 64 && dds.height == 64 &&
                       dds.mip_count == 7 && dds.mip_data.size() == 7 &&
                       dds.format == DDSFormatCPU::RGBA8, "generated DDS cannot decode with full mip chain")) return false;
        }
        std::set<std::string_view> materials;
        for (const auto& material : definition->materials) materials.insert(material.surface_name);
        std::uint64_t triangles = 0;
        for (const auto& mesh : definition->meshes) {
            for (const auto& s : mesh.surfaces) {
                if (!check(materials.contains(s.name), "missing material record")) return false;
                for (const auto& v : s.vertices) {
                    const glm::vec3 p{v.px, v.py, v.pz}, n{v.nx, v.ny, v.nz}, t{v.tx, v.ty, v.tz};
                    if (!check(std::isfinite(glm::dot(p, p)) && std::isfinite(v.u) && std::isfinite(v.v) &&
                               std::abs(glm::dot(n, n) - 1) < 0.001f &&
                               std::abs(glm::dot(t, t) - 1) < 0.001f &&
                               std::abs(glm::dot(t, n)) < 0.001f, "non-finite or invalid tangent frame")) return false;
                    if (!check(glm::all(glm::greaterThanEqual(p, mesh.bbox_min)) &&
                               glm::all(glm::lessThanEqual(p, mesh.bbox_max)), "vertex outside bounds")) return false;
                }
                if (!check(s.indices.size() % 3 == 0, "incomplete triangle")) return false;
                for (std::size_t i = 0; i < s.indices.size(); i += 3) {
                    const auto a = s.indices[i], b = s.indices[i + 1], c = s.indices[i + 2];
                    if (!check(a < s.vertices.size() && b < s.vertices.size() && c < s.vertices.size(), "index outside surface")) return false;
                    const auto p = [](const Vertex& v) { return glm::vec3(v.px, v.py, v.pz); };
                    const auto n = [](const Vertex& v) { return glm::vec3(v.nx, v.ny, v.nz); };
                    const auto cross = glm::cross(p(s.vertices[b]) - p(s.vertices[a]), p(s.vertices[c]) - p(s.vertices[a]));
                    const auto sum_n = n(s.vertices[a]) + n(s.vertices[b]) + n(s.vertices[c]);
                    if (!check(glm::dot(cross, cross) > 1e-15f && glm::dot(cross, sum_n) > 0, "degenerate or reversed triangle")) return false;
                }
                triangles += s.indices.size() / 3;
            }
        }
        if (!check(triangles > 0 && triangles < 100'000, "fixture triangle budget")) return false;
        for (const auto& instance : definition->instances) {
            if (!check(instance.mesh_index >= 0 && std::size_t(instance.mesh_index) < definition->meshes.size(), "instance outside mesh array")) return false;
        }
        if (!check(!definition->grounds.empty() && definition->grounds.front().instance_index == 0,
                   "fixture has no explicit ground instance")) return false;
        if (info.id == "atmosphere") {
            const auto& ground = definition->grounds.front();
            bool checked_tower = false;
            for (const auto& instance : definition->instances) {
                const auto& mesh = definition->meshes[instance.mesh_index];
                if (mesh.name != "far_tower") continue;
                checked_tower = true;
                for (int corner = 0; corner < 8; ++corner) {
                    const glm::vec3 local{corner & 1 ? mesh.bbox_max.x : mesh.bbox_min.x,
                        corner & 2 ? mesh.bbox_max.y : mesh.bbox_min.y,
                        corner & 4 ? mesh.bbox_max.z : mesh.bbox_min.z};
                    const auto world = glm::vec3(instance.transform * glm::vec4(local, 1));
                    if (!check(world.x >= ground.local_min.x && world.x <= ground.local_max.x &&
                               world.y >= ground.local_min.y && world.y <= ground.local_max.y,
                               "atmosphere far tower footprint extends beyond ground")) return false;
                }
            }
            if (!check(checked_tower, "atmosphere far tower missing")) return false;
        }
        if (info.id == "shadow-cascade") {
            const auto ground = std::ranges::find(definition->materials,
                "render_test_ground", &RenderTestMaterial::surface_name);
            if (!check(ground != definition->materials.end() &&
                       ground->diffuse_texture.ends_with("/white.dds"),
                       "fractional shadow control needs a uniform readable receiver")) return false;
            const auto m = std::ranges::find(definition->materials,
                "render_test_leaf_shadow_card", &RenderTestMaterial::surface_name);
            if (!check(m != definition->materials.end() && m->material.alpha_ref == 0.5f,
                       "masked shadow control missing")) return false;
            const auto t = std::ranges::find(definition->textures, m->diffuse_texture,
                &RenderTestTexture::path);
            if (!check(t != definition->textures.end() && t->bytes->size() > 128 + 4 * (32 * 64 + 32) + 3,
                       "shadow-only texture missing")) return false;
            const auto alpha = (*t->bytes)[128 + 4 * (32 * 64 + 32) + 3];
            if (!check(alpha > 127 && alpha < 217, "shadow control has no fractional interior alpha")) return false;
            const float parameter = (static_cast<float>(alpha) / 255.0f - 0.40f) / (0.85f - 0.40f);
            const float coverage = parameter * parameter * (3 - 2 * parameter);
            if (!check(coverage > 0 && coverage < 1, "shadow alpha does not yield fractional shader coverage")) return false;
        }
        if (info.id == "lighting" || info.id == "bloom") {
            if (!check(definition->lights.size() == 3 &&
                       std::ranges::all_of(definition->lights, [](const SceneLight& light) { return light.multiplier == 64; }),
                       "night fixture local lights lack the isolated intensity scale")) return false;
        }
        if (info.id == "depth-of-field") {
            bool checked_near = false;
            const float fy = 540.0f / (2 * std::tan(glm::radians(25.0f)));
            for (const auto& instance : definition->instances) {
                const auto& mesh = definition->meshes[instance.mesh_index];
                if (mesh.name != "focus_0") continue;
                checked_near = true;
                const auto lo = glm::vec3(instance.transform * glm::vec4(mesh.bbox_min, 1));
                const auto hi = glm::vec3(instance.transform * glm::vec4(mesh.bbox_max, 1));
                const float depth = lo.y + 8;
                const glm::vec4 projected{480 + fy * lo.x / depth, 270 - fy * (hi.z - 2) / depth,
                    480 + fy * hi.x / depth, 270 - fy * (lo.z - 2) / depth};
                const glm::vec4 previous_frame{283.1343f, 246.8393f, 375.7770f, 455.2854f};
                if (!check(std::abs(depth - 1.5f) < 1e-5f &&
                           glm::all(glm::lessThan(glm::abs(projected - previous_frame), glm::vec4(0.02f))),
                           "near DOF target must preserve framing at1.5m")) return false;
            }
            if (!check(checked_near, "near DOF target missing")) return false;
        }
        if (info.id == "particles") {
            const auto m = std::ranges::find(definition->materials,
                "render_test_particle_background", &RenderTestMaterial::surface_name);
            const auto mesh = std::ranges::find(definition->meshes,
                "particle_background", &Mesh::name);
            if (!check(m != definition->materials.end() && mesh != definition->meshes.end() &&
                       m->diffuse_texture == m->emission_texture && m->material.emission_scale == 8 &&
                       !m->material.emission_light_source && mesh->bbox_max.z == 7 &&
                       mesh->bbox_min.x == -5.5f && mesh->bbox_max.x == 5.5f,
                       "haze backdrop must be tall, emissive and not spawn illumination")) return false;
        }
        if (info.id == "displacement") {
            const auto coarse = std::ranges::find(definition->meshes, "coarse_height", &Mesh::name);
            const auto material = std::ranges::find(definition->materials,
                "render_test_coarse_height", &RenderTestMaterial::surface_name);
            if (!check(coarse != definition->meshes.end() && material != definition->materials.end(),
                       "coarse height control missing")) return false;
            const auto height = load_dds_cpu(material->height_texture, std::cref(*assets));
            if (!height) return false;
            const auto bilinear = [&](int level, float u, float v) {
                const auto& mip = height.mip_data[level];
                const int w = static_cast<int>(height.width >> level);
                const int h = static_cast<int>(height.height >> level);
                const float x = u * static_cast<float>(w) - 0.5f, y = v * static_cast<float>(h) - 0.5f;
                const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
                const auto texel = [&](int px, int py) {
                    px = (px % w + w) % w; py = (py % h + h) % h;
                    const auto offset = 4 * (static_cast<std::size_t>(py) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(px));
                    return static_cast<float>(mip.data()[offset]) / 255.0f;
                };
                return std::lerp(std::lerp(texel(ix, iy), texel(ix + 1, iy), x - static_cast<float>(ix)),
                    std::lerp(texel(ix, iy + 1), texel(ix + 1, iy + 1), x - static_cast<float>(ix)),
                    y - static_cast<float>(iy));
            };
            const float lod = std::log2(64.0f / 9.0f);
            const int level = static_cast<int>(std::floor(lod));
            float minimum = 1, maximum = 0;
            for (const auto& v : coarse->surfaces.front().vertices) {
                const float h = std::lerp(bilinear(level, v.u, v.v),
                    bilinear(level + 1, v.u, v.v), lod - static_cast<float>(level));
                minimum = std::min(minimum, h); maximum = std::max(maximum, h);
            }
            if (!check(maximum - minimum > 0.15f,
                       "coarse height samples alias to a neutral plane")) return false;
        }
    }
    return check(ids.size() == 22 && !make_render_test_scene("unknown"), "catalog/unknown ID contract");
}

bool procedural_material_models_and_animation_are_real_inputs() {
    using namespace heaven;
    using namespace material_model_ref;
    auto models = make_render_test_scene("material-models");
    if (!models) return false;
    std::set<Material_Model> routed;
    const EnabledModels enabled{true, true, true, true};
    for (const auto& m : models->materials) {
        routed.insert(select_material_model(m.surface_name, m.material.material_name,
            m.material.parent_chain, Material_Model::Standard_Lit, enabled));
    }
    if (!check(routed.contains(Material_Model::Clear_Coat) && routed.contains(Material_Model::Anisotropic) &&
               routed.contains(Material_Model::Cloth) && routed.contains(Material_Model::Subsurface_Translucent),
               "procedural materials fail advanced model routing")) return false;
    auto animation = make_render_test_scene("animation");
    if (!check(bool(animation) && animation->skinned_objects.size() == 1 && animation->cloths.size() == 1,
               "animation fixture lacks skeleton or cloth")) return false;
    const auto& skin = animation->skinned_objects.front().mesh;
    const auto rest = skin.skin_frame(0, 0), bent = skin.skin_frame(0, 15);
    float max_motion = 0;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        max_motion = std::max(max_motion, glm::length(glm::vec3(rest[i].px - bent[i].px,
            rest[i].py - bent[i].py, rest[i].pz - bent[i].pz)));
    }
    return check(max_motion > 0.1f && !animation->cloths.front().anchor_idx.empty() &&
                 !animation->cloths.front().linear.empty() && animation->instances.size() > animation->meshes.size(),
                 "procedural animation/instancing inputs do not vary");
}

bool procedural_upload_preserves_routes_and_feature_gates() {
    using namespace heaven;
    for (const auto& info : render_test_scene_catalog()) {
        if (render_test_scene_uses_assets(info.id)) continue;
        AppConfig config;
        if (!check(apply_render_test_scene_defaults(config, info.id) && config.render_test_scene == info.id,
                   "defaults do not select the catalog scene")) return false;
        auto definition = make_render_test_scene(info.id);
        auto assets = AssetFileSystem::create_memory("render-test-upload-unit");
        if (!definition || !assets || !mount_render_test_scene_assets(*definition, *assets)) return false;
        ProceduralBackend backend(*assets);
        auto uploaded = upload_render_test_scene(std::move(*definition), backend, config);
        if (!check(bool(uploaded) && backend.ground_valid &&
                   uploaded->scene.cpu_meshes.size() == uploaded->scene.gpu_meshes.size() &&
                   uploaded->scene.total_triangles > 0 &&
                   backend.semantics.contains(TextureSemantic::ColorSRGB) &&
                   backend.semantics.contains(TextureSemantic::ScalarLinear) &&
                   backend.semantics.contains(TextureSemantic::NormalLinear), "ordinary resource upload contract")) return false;
        for (const auto& gpu : uploaded->scene.gpu_meshes) {
            for (const auto& s : gpu.surfaces) {
                if (!check(uploaded->resolve_material(gpu.path, s.name).diffuse != 0, "uploaded material not resolved")) return false;
            }
        }
        if (info.id == "material-models") {
            const auto matte = uploaded->resolve_material({}, "render_test_matte");
            if (!check(matte.has_detail && matte.detail_diffuse != 0 && matte.detail_normal != 0 &&
                       matte.detail_specular != 0 && matte.detail_mode == 4 &&
                       matte.base_transform[0] != 1 && matte.detail_transform[0] != 1,
                       "detail/UV transform stimuli are inactive")) return false;
        }
        if (info.id == "displacement") {
            const auto pom = uploaded->resolve_material({}, "render_test_pom_height");
            const auto dense = uploaded->resolve_material({}, "render_test_dense_ground_relief");
            if (!check(pom.height != 0 && !pom.geometry_displacement && dense.geometry_displacement &&
                       dense.displaced_surface_normal && dense.ground_relief_shading,
                       "POM-only and enabled relief controls are not distinct")) return false;
        }
        if (info.id == "animation" && !check(backend.skinned_uploads == 1 && uploaded->scene.animated.size() == 1,
                                            "GPU skinning route not exercised")) return false;
    }
    auto assets = AssetFileSystem::create_memory("render-test-relief-unit");
    auto definition = make_render_test_scene("displacement");
    if (!assets || !definition || !mount_render_test_scene_assets(*definition, *assets)) return false;
    ProceduralBackend backend(*assets);
    AppConfig config;
    config.features.ground_relief = false;
    auto off = upload_render_test_scene(std::move(*definition), backend, config);
    if (!off) return false;
    const auto coarse = off->resolve_material({}, "render_test_coarse_height");
    const auto dense = off->resolve_material({}, "render_test_dense_ground_relief");
    return check(coarse.geometry_displacement && !dense.geometry_displacement &&
                 !dense.displaced_surface_normal && !dense.ground_relief_shading && dense.height != 0,
                 "ground relief off must retain POM map and disable dense geometry/shading");
}

heaven::testing::Register geometry_test(procedural_scenes_have_valid_geometry_and_mounted_dds);
heaven::testing::Register routing_test(procedural_material_models_and_animation_are_real_inputs);
heaven::testing::Register upload_test(procedural_upload_preserves_routes_and_feature_gates);

} // namespace
