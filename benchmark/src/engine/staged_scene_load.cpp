#include "benchmark/microprofiler.h"
#include "engine/staged_scene_load.h"

#include "engine/material_rhi.h"
#include "engine/render_test_scene_catalog.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

namespace heaven {
namespace {

// Real MeshStore handles increase from one; this private namespace makes a
// second upload attempt or an accidental live-scene handoff fail explicitly.
constexpr std::uint64_t kStagedHandleBit = std::uint64_t{1} << 63;
std::mutex staged_loader_mutex;

class StagingBackend final : public IRhiBackend {
public:
    GraphicsApi api() const noexcept override { return GraphicsApi::Filament; }
    std::string device_name() const override { return "CPU scene staging"; }
    std::string api_version() const override { return {}; }
    RendererFeatureStats renderer_feature_stats() const override { return {}; }

    GpuMesh upload_mesh(const Mesh& mesh) override {
        return metadata(mesh.name, mesh.path, mesh.surfaces);
    }
    GpuMesh upload_skinned(const SkinnedMesh& mesh) override {
        return metadata(mesh.name, mesh.path, mesh.surfaces);
    }
    std::uint64_t load_texture(const std::string& /*path*/,
                              TextureSemantic /*semantic*/) override { return 1; }
    Result<void> init_pipelines(const AppConfig& /*config*/) override { return {}; }
    FrameResult render_frame(std::span<const Instance> /*instances*/,
            std::span<const int> /*dirty_instance_indices*/,
            std::span<const GpuMesh> /*meshes*/,
            const MaterialResolver& /*resolve*/, const Camera& /*camera*/,
            const LightingParams& /*lighting*/, const WeatherState& /*weather*/,
            const TonemapParams& /*tonemap*/, int /*width*/, int /*height*/) override {
        return {};
    }
    void present() override {}
    Result<void> update_mesh_vertices(std::uint64_t /*handle*/,
            const Vertex* /*vertices*/, std::uint32_t /*count*/) override { return {}; }

private:
    template<class Surfaces>
    GpuMesh metadata(const std::string& name, const std::string& path,
                     const Surfaces& surfaces) {
        GpuMesh mesh;
        mesh.name = name;
        mesh.path = path;
        bool has_bounds = false;
        for (const auto& source : surfaces) {
            if (source.vertices.empty() || source.indices.empty()) continue;
            GpuSurface surface;
            surface.handle = kStagedHandleBit | next_handle_++;
            surface.index_count = static_cast<std::uint32_t>(source.indices.size());
            surface.name = source.name;
            const auto& first = source.vertices.front();
            surface.bbox_min = {first.px, first.py, first.pz};
            surface.bbox_max = surface.bbox_min;
            for (const auto& vertex : source.vertices) {
                const glm::vec3 position{vertex.px, vertex.py, vertex.pz};
                surface.bbox_min = glm::min(surface.bbox_min, position);
                surface.bbox_max = glm::max(surface.bbox_max, position);
            }
            mesh.bbox_min = has_bounds ? glm::min(mesh.bbox_min, surface.bbox_min)
                                      : surface.bbox_min;
            mesh.bbox_max = has_bounds ? glm::max(mesh.bbox_max, surface.bbox_max)
                                      : surface.bbox_max;
            has_bounds = true;
            mesh.surfaces.push_back(std::move(surface));
        }
        return mesh;
    }
    std::uint64_t next_handle_ = 1;
};

struct UploadRollback {
    IRhiBackend& backend;
    std::vector<GpuMesh>& meshes;
    bool committed = false;
    ~UploadRollback() {
        if (!committed) for (const auto& mesh : meshes) backend.release_mesh(mesh);
    }
};

// Match upload's omission of empty surfaces by ordered source occurrence,
// never by name alone: names can repeat within a mesh.
template<class Surfaces>
Result<void> validate_upload(const Surfaces& source, const GpuMesh& staged,
                             const GpuMesh& actual) {
    std::size_t uploaded_index = 0;
    for (const auto& surface : source) {
        if (surface.indices.empty()) continue;
        if (surface.vertices.empty() ||
                surface.indices.size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(Error{"Staged mesh has invalid source surface: " + staged.path});
        if (uploaded_index >= staged.surfaces.size() ||
                uploaded_index >= actual.surfaces.size())
            return std::unexpected(Error{"Staged mesh upload is incomplete: " + staged.path});
        const auto& plan = staged.surfaces[uploaded_index];
        const auto& result = actual.surfaces[uploaded_index];
        if (!result.handle || (result.handle & kStagedHandleBit) != 0 ||
                result.name != surface.name || plan.name != surface.name ||
                result.index_count != surface.indices.size() ||
                (plan.index_count != 0 && plan.index_count != result.index_count))
            return std::unexpected(Error{"Staged mesh upload disagrees with source: " +
                                         staged.path + " / " + surface.name});
        ++uploaded_index;
    }
    if (uploaded_index != staged.surfaces.size() || uploaded_index != actual.surfaces.size())
        return std::unexpected(Error{"Staged mesh upload has unexpected surfaces: " + staged.path});
    return {};
}

} // namespace

Result<RhiScene> stage_scene_load(const AppConfig& config,
        RhiMaterialDatabase& materials, const std::string& cache_file,
        CameraCollision* collision, AssetFileSystemRef assets,
        const LoadingProgressCallback& progress) {
    profile::Scope profile_scope("Stage CPU scene", "Loading");
    const std::scoped_lock lock(staged_loader_mutex);
    StagingBackend backend;
    return load_scene_rhi(config, backend, materials, cache_file, collision, assets, progress);
}

Result<void> upload_staged_scene(RhiScene& scene, IRhiBackend& backend,
        const AppConfig& config, const LoadingProgressCallback& progress) {
    profile::Scope profile_scope("Upload staged scene", "Upload");
    if (scene.cpu_meshes.size() != scene.gpu_meshes.size())
        return std::unexpected(Error{"Staged CPU/GPU mesh metadata counts disagree"});
    std::vector<const AnimatedObject*> animated(scene.cpu_meshes.size(), nullptr);
    for (const auto& object : scene.animated) {
        if (object.mesh_index < 0 || std::cmp_greater_equal(object.mesh_index, animated.size()) ||
                animated[static_cast<std::size_t>(object.mesh_index)] != nullptr)
            return std::unexpected(Error{"Staged animation mesh index is invalid or duplicated"});
        animated[static_cast<std::size_t>(object.mesh_index)] = &object;
    }
    for (const auto& mesh : scene.gpu_meshes)
        for (const auto& surface : mesh.surfaces)
            if ((surface.handle & kStagedHandleBit) == 0)
                return std::unexpected(Error{"Scene already contains real or invalid mesh handles"});

    std::vector<GpuMesh> uploaded;
    uploaded.reserve(scene.cpu_meshes.size());
    UploadRollback rollback{backend, uploaded};
    const auto total = scene.cpu_meshes.size();
    for (std::size_t index = 0; index < total; ++index) {
        if (progress) progress("Uploading meshes", static_cast<float>(index) / static_cast<float>(total));
        const auto& plan = scene.gpu_meshes[index];
        const auto* animation = animated[index];
        // The ordinary loader retains animated source type in both skinning
        // configurations. CPU fallback updates these same UV tangent frames.
        const bool upload_skinned = animation != nullptr;
        uploaded.push_back(upload_skinned ? backend.upload_skinned(animation->smesh)
                                         : backend.upload_mesh(scene.cpu_meshes[index]));
        auto& actual = uploaded.back();
        auto valid = upload_skinned ? validate_upload(animation->smesh.surfaces, plan, actual)
                                   : validate_upload(scene.cpu_meshes[index].surfaces, plan, actual);
        if (!valid) return valid;
        if (render_test_scene_uses_assets(config.render_test_scene) && actual.surfaces.empty())
            return std::unexpected(Error{"Asset fixture mesh upload has no surfaces: " + plan.path});
        actual.path = plan.path;
        actual.name = plan.name;
        actual.cast_shadows = plan.cast_shadows;
        for (std::size_t surface = 0; surface < plan.surfaces.size(); ++surface)
            if (plan.surfaces[surface].index_count == 0) actual.surfaces[surface].index_count = 0;
        if (progress) progress("Uploading meshes", static_cast<float>(index + 1) / static_cast<float>(total));
    }

    // Allocate/copy animation aliases before committing actual owners. Any
    // allocation failure or callback cancellation still rolls back new handles.
    std::vector<GpuMesh> animation_meshes;
    animation_meshes.reserve(scene.animated.size());
    for (const auto& object : scene.animated)
        animation_meshes.push_back(uploaded[static_cast<std::size_t>(object.mesh_index)]);
    scene.gpu_meshes.swap(uploaded);
    for (std::size_t index = 0; index < scene.animated.size(); ++index) {
        std::swap(scene.animated[index].gpu_mesh, animation_meshes[index]);
        scene.animated[index].gpu_skin_resolved = false;
        scene.animated[index].gpu_skin_active = false;
    }
    rollback.committed = true;
    return {};
}

} // namespace heaven
