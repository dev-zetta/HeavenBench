/* HeavenBench — CPU staging and transactional owner-thread mesh uploads. */
#include "engine/staged_scene_load.h"

#include "engine/material_rhi.h"
#include "engine/path_resolver.h"
#include "engine/render_test_scene_catalog.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <stdexcept>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}

namespace {
using namespace heaven;
constexpr std::uint64_t kSynthetic = std::uint64_t{1} << 63;

bool require(bool value, const char* message) {
    if (!value) std::fprintf(stderr, "staged scene test: %s\n", message);
    return value;
}

Surface triangle(const std::string& name) {
    Surface result;
    result.name = name;
    for (const auto position : {glm::vec3{0, 0, 0}, glm::vec3{1, 0, 0}, glm::vec3{0, 1, 0}}) {
        Vertex vertex{};
        vertex.px = position.x; vertex.py = position.y; vertex.pz = position.z;
        vertex.nz = 1;
        result.vertices.push_back(vertex);
    }
    result.indices = {0, 1, 2};
    return result;
}

class UploadBackend final : public IRhiBackend {
public:
    std::size_t static_uploads = 0, skinned_uploads = 0, releases = 0;
    std::size_t fail_upload = 0;
    bool synthetic_released = false;
    std::vector<float> first_source_x;
    std::set<std::uint64_t> live{77}; // A different live scene must survive.

    GraphicsApi api() const noexcept override { return GraphicsApi::Filament; }
    std::string device_name() const override { return "staged upload host test"; }
    std::string api_version() const override { return {}; }
    RendererFeatureStats renderer_feature_stats() const override { return {}; }
    GpuMesh upload_mesh(const Mesh& mesh) override {
        ++static_uploads;
        return upload(mesh.name, mesh.path, mesh.surfaces);
    }
    GpuMesh upload_skinned(const SkinnedMesh& mesh) override {
        ++skinned_uploads;
        return upload(mesh.name, mesh.path, mesh.surfaces);
    }
    void release_mesh(const GpuMesh& mesh) override {
        for (const auto& surface : mesh.surfaces) {
            synthetic_released |= (surface.handle & kSynthetic) != 0;
            releases += live.erase(surface.handle);
        }
    }
    std::uint64_t load_texture(const std::string& /*path*/, TextureSemantic /*semantic*/) override { return 1; }
    Result<void> init_pipelines(const AppConfig& /*config*/) override { return {}; }
    FrameResult render_frame(std::span<const Instance> /*instances*/,
            std::span<const int> /*dirty_indices*/, std::span<const GpuMesh> /*meshes*/,
            const MaterialResolver& /*resolve*/, const Camera& /*camera*/,
            const LightingParams& /*lighting*/, const WeatherState& /*weather*/,
            const TonemapParams& /*tonemap*/, int /*width*/, int /*height*/) override { return {}; }
    void present() override {}
    Result<void> update_mesh_vertices(std::uint64_t /*handle*/,
            const Vertex* /*vertices*/, std::uint32_t /*count*/) override { return {}; }

private:
    template<class Surfaces>
    GpuMesh upload(const std::string& name, const std::string& path, const Surfaces& surfaces) {
        GpuMesh result;
        result.name = name; result.path = path;
        for (const auto& surface : surfaces) {
            if (surface.vertices.empty() || surface.indices.empty()) continue;
            first_source_x.push_back(surface.vertices.front().px);
            const auto handle = next_handle_++;
            live.insert(handle);
            result.surfaces.push_back({handle, static_cast<std::uint32_t>(surface.indices.size()), surface.name});
        }
        if (fail_upload == static_uploads + skinned_uploads && !result.surfaces.empty())
            result.surfaces.front().index_count = 1;
        return result;
    }
    std::uint64_t next_handle_ = 100;
};

RhiScene plan() {
    RhiScene scene;
    Mesh mesh;
    mesh.name = "static"; mesh.path = "relative/static.mesh";
    mesh.surfaces.push_back({}); // Upload omits empty surfaces.
    mesh.surfaces.push_back(triangle("duplicate"));
    mesh.surfaces.push_back(triangle("duplicate"));
    scene.cpu_meshes.push_back(mesh);
    GpuMesh gpu;
    gpu.name = mesh.name; gpu.path = mesh.path; gpu.cast_shadows = false;
    gpu.surfaces = {{kSynthetic | 1, 0, "duplicate"}, {kSynthetic | 2, 3, "duplicate"}};
    scene.gpu_meshes.push_back(gpu);

    AnimatedObject animation;
    animation.mesh_index = 1;
    animation.smesh.name = "skinned"; animation.smesh.path = "absolute/source.smesh";
    animation.smesh.num_frames = 2;
    SkinnedSurface skinned;
    skinned.name = "moving";
    skinned.vertices.resize(3); skinned.indices = {0, 1, 2};
    skinned.vertices.front().px = 17;
    animation.smesh.surfaces.push_back(std::move(skinned));
    Mesh bind;
    bind.name = "skinned"; bind.path = "relative/source.smesh";
    bind.surfaces.push_back(triangle("moving"));
    bind.surfaces.front().vertices.front().px = 23;
    scene.cpu_meshes.push_back(std::move(bind));
    gpu.name = "skinned"; gpu.path = "relative/source.smesh";
    gpu.surfaces = {{kSynthetic | 3, 3, "moving"}};
    scene.gpu_meshes.push_back(std::move(gpu));
    scene.animated.push_back(std::move(animation));
    return scene;
}

bool uploads_preserve_source_roles_and_aliases() {
    auto scene = plan();
    AppConfig config;
    config.features.gpu_skinning = false;
    UploadBackend backend;
    std::vector<float> progress_values;
    const auto result = upload_staged_scene(scene, backend, config,
        [&](std::string_view /*stage*/, float fraction) { progress_values.push_back(fraction); });
    if (!require(result.has_value(), "valid plan must upload") ||
            !require(backend.static_uploads == 1 && backend.skinned_uploads == 1 &&
                     backend.first_source_x.back() == 17,
                     "GPU-skinning-disabled upload must retain the ordinary loader's skinned source type") ||
            !require(scene.gpu_meshes[0].surfaces.size() == 2 &&
                     scene.gpu_meshes[0].surfaces[0].index_count == 0 &&
                     scene.gpu_meshes[0].surfaces[1].index_count == 3,
                     "duplicate-name surfaces must retain ordered hiding after an omitted empty source") ||
            !require(!scene.gpu_meshes[0].cast_shadows && !scene.gpu_meshes[1].cast_shadows &&
                     scene.gpu_meshes[1].path == "relative/source.smesh",
                     "caster and relative material-path policies changed") ||
            !require(scene.animated[0].gpu_mesh.surfaces[0].handle == scene.gpu_meshes[1].surfaces[0].handle &&
                     !scene.animated[0].gpu_skin_resolved,
                     "animation alias must contain the actual final handle") ||
            !require(std::ranges::is_sorted(progress_values) && progress_values.front() == 0 &&
                     progress_values.back() == 1,
                     "per-mesh owner-thread progress must reach one monotonically")) return false;
    const auto before = backend.live;
    if (!require(!upload_staged_scene(scene, backend, config) && backend.live == before,
                 "a second upload must reject live handles without releasing them")) return false;
    for (const auto& mesh : scene.gpu_meshes) backend.release_mesh(mesh);
    if (!require(backend.live == std::set<std::uint64_t>{77} && !backend.synthetic_released,
                 "caller retirement must preserve the unrelated owner")) return false;
    auto gpu_scene = plan();
    UploadBackend gpu_backend;
    config.features.gpu_skinning = true;
    const auto gpu_result = upload_staged_scene(gpu_scene, gpu_backend, config);
    if (!require(gpu_result && gpu_backend.static_uploads == 1 && gpu_backend.skinned_uploads == 1 &&
                 gpu_backend.first_source_x.back() == 17,
                 "GPU-skinning-enabled upload must consume the immutable skinned source")) return false;
    for (const auto& mesh : gpu_scene.gpu_meshes) gpu_backend.release_mesh(mesh);
    return require(gpu_backend.live == std::set<std::uint64_t>{77}, "GPU source owners did not retire");
}

bool cancellation_and_partial_upload_retire_only_new_owners() {
    for (const bool cancel : {false, true}) {
        auto scene = plan();
        UploadBackend backend;
        AppConfig config;
        config.render_test_scene = "asset-animation";
        backend.fail_upload = cancel ? 0 : 2;
        bool caught = false;
        bool returned_failure = false;
        try {
            const auto result = upload_staged_scene(scene, backend, config,
                [&](std::string_view /*stage*/, float fraction) {
                    if (cancel && fraction == 0.5f) throw std::runtime_error("cancelled test upload");
                });
            returned_failure = !result;
        } catch (const std::runtime_error&) { caught = true; }
        if (!require(cancel ? caught : returned_failure, "cancellation/failure did not propagate") ||
                !require(backend.live == std::set<std::uint64_t>{77} && !backend.synthetic_released,
                         "failed/cancelled upload must release new actual owners only") ||
                !require(scene.gpu_meshes[0].surfaces[0].handle == (kSynthetic | 1) &&
                         scene.gpu_meshes[1].surfaces[0].handle == (kSynthetic | 3),
                         "failure must leave the CPU plan reusable") ||
                !require(backend.releases == (cancel ? 2u : 3u), "partial upload owner accounting is incomplete"))
            return false;
    }
    return true;
}

bool cancellation_at_begin_and_final_boundary_is_transactional() {
    for (const float cancelled_fraction : {0.0f, 1.0f}) {
        auto scene = plan();
        UploadBackend backend;
        bool caught = false;
        try {
            (void)upload_staged_scene(scene, backend, AppConfig{},
                [&](std::string_view /*stage*/, float fraction) {
                    if (fraction == cancelled_fraction) throw std::runtime_error("cancelled at boundary");
                });
        } catch (const std::runtime_error&) { caught = true; }
        if (!require(caught && backend.live == std::set<std::uint64_t>{77} &&
                     !backend.synthetic_released,
                     "boundary cancellation must retire actual owners without touching the old scene") ||
                !require(scene.gpu_meshes[0].surfaces[0].handle == (kSynthetic | 1),
                         "final upload cancellation must precede scene commit") ||
                !require(backend.releases == (cancelled_fraction == 0 ? 0u : 3u),
                         "boundary cancellation owner count is wrong")) return false;
    }
    return true;
}

// A real tiny mi08/node/material namespace exercises the ordinary loader on
// the worker-side backend; upload tests above deliberately do not copy its math.
using Bytes = std::vector<std::uint8_t>;
template<class T> void append(Bytes& bytes, T value) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), begin, begin + sizeof(value));
}
void text(Bytes& bytes, const std::string& value) {
    append(bytes, static_cast<std::uint32_t>(value.size() + 1));
    bytes.insert(bytes.end(), value.begin(), value.end()); bytes.push_back(0);
}
struct Element {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string value;
    std::vector<Element> children;
};
Bytes xml(const Element& root) {
    Bytes tree;
    std::vector<std::string> tags, attributes, values;
    const auto intern = [](std::vector<std::string>& pool, const std::string& value) {
        const auto found = std::ranges::find(pool, value);
        if (found != pool.end()) return static_cast<std::uint8_t>(found - pool.begin());
        pool.push_back(value); return static_cast<std::uint8_t>(pool.size() - 1);
    };
    const std::function<void(const Element&)> write = [&](const Element& element) {
        tree.push_back(intern(tags, element.tag));
        tree.push_back(static_cast<std::uint8_t>(element.attributes.size())); tree.push_back(0);
        for (const auto& [key, value] : element.attributes) {
            tree.push_back(intern(attributes, key)); tree.push_back(intern(values, value));
        }
        tree.push_back(element.value.empty() ? 0 : 1);
        if (!element.value.empty()) text(tree, element.value);
        else {
            tree.push_back(static_cast<std::uint8_t>(element.children.size()));
            for (const auto& child : element.children) {
                tree.push_back(0); write(child); tree.push_back(0);
            }
        }
    };
    write(root);
    Bytes bytes{'x', 'm', 'l', '0'}; append(bytes, std::uint32_t{0});
    for (const auto& tag : tags) text(bytes, tag);
    append(bytes, static_cast<std::uint32_t>(attributes.size()));
    for (const auto& attribute : attributes) text(bytes, attribute);
    append(bytes, static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values) text(bytes, value);
    bytes.insert(bytes.end(), tree.begin(), tree.end()); return bytes;
}
Bytes mi08() {
    Bytes bytes{'m', 'i', '0', '8'};
    for (int i = 0; i < 10; ++i) append(bytes, 0.0f);
    append(bytes, std::int32_t{1}); text(bytes, "visible");
    for (int i = 0; i < 10; ++i) append(bytes, 0.0f);
    append(bytes, std::int32_t{3});
    for (const auto& vertex : triangle("visible").vertices) {
        append(bytes, vertex.px); append(bytes, vertex.py); append(bytes, vertex.pz);
        append(bytes, std::uint16_t{32768}); append(bytes, std::uint16_t{32768}); append(bytes, std::uint16_t{65535});
    }
    append(bytes, std::int32_t{3});
    for (const auto uv : {glm::vec2{0, 0}, glm::vec2{1, 0}, glm::vec2{0, 1}}) {
        append(bytes, uv.x); append(bytes, uv.y);
    }
    append(bytes, std::int32_t{0}); append(bytes, std::int32_t{1});
    append(bytes, std::uint16_t{0}); append(bytes, std::uint16_t{1}); append(bytes, std::uint16_t{2});
    return bytes;
}

bool cpu_stage_uses_real_loader_and_strict_material_validation() {
    auto assets = AssetFileSystem::create_memory("staged_scene_memory");
    if (!assets) return false;
    constexpr std::string_view mesh_path = "heaven/meshes/fixture.mesh";
    const auto object = Element{"node", {{"type", "ObjectMesh"}}, "", {
        {"mesh", {}, std::string(mesh_path), {}},
        {"surface", {{"name", "visible"}, {"material", "fixture_material"}}, "", {}}}};
    const auto root_bytes = xml({"nodes", {}, "", {object}});
    for (const auto path : render_test_scene_asset_nodes("asset-contact-shadow"))
        if (!assets->mount_memory(std::string(path), std::make_shared<const Bytes>(root_bytes))) return false;
    if (!assets->mount_memory(std::string(mesh_path), std::make_shared<const Bytes>(mi08())) ||
            !assets->mount_memory("heaven/materials/fixture.mat", std::make_shared<const Bytes>(
                xml({"materials", {}, "", {{"material", {{"name", "fixture_material"}}, "", {}}}})))) return false;
    AppConfig config;
    config.data_dir = assets->data_root(); config.render_test_scene = "asset-contact-shadow";
    config.tessellation.enabled = false;
    const auto paths = PathResolver::create(config.data_dir);
    if (!paths) return false;
    RhiMaterialDatabase materials;
    materials.build(*paths, nullptr, std::cref(*assets));
    bool cancelled = false;
    try {
        (void)stage_scene_load(config, materials, "", nullptr, std::cref(*assets),
            [](std::string_view /*stage*/, float /*fraction*/) {
                throw std::runtime_error("cancelled CPU load");
            });
    } catch (const std::runtime_error&) { cancelled = true; }
    if (!require(cancelled, "CPU stage callback cancellation must propagate")) return false;
    // A successful retry also proves cancellation released the loader mutex.
    auto staged = stage_scene_load(config, materials, "", nullptr, std::cref(*assets));
    if (!require(staged && staged->cpu_meshes.size() == 1 && staged->instances.size() == 2 &&
                 staged->gpu_meshes[0].surfaces[0].index_count == 3 &&
                 (staged->gpu_meshes[0].surfaces[0].handle & kSynthetic) != 0,
                 "CPU staging must preserve real authored loading/deduplication without real handles")) return false;
    RhiMaterialDatabase missing;
    return require(!stage_scene_load(config, missing, "", nullptr, std::cref(*assets)),
                   "CPU staging must not bypass unresolved authored material validation");
}

const heaven::testing::Register roles(uploads_preserve_source_roles_and_aliases);
const heaven::testing::Register rollback(cancellation_and_partial_upload_retire_only_new_owners);
const heaven::testing::Register boundaries(cancellation_at_begin_and_final_boundary_is_transactional);
const heaven::testing::Register loader(cpu_stage_uses_real_loader_and_strict_material_validation);
} // namespace
