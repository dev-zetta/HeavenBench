#include "rhi/frame_generation/temporal_scene.h"
#include "rhi/frame_generation/temporal_camera.h"

#include "engine/camera.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"
#include "rhi/geometry_displacement.h"
#include "rhi/instance_bounds.h"
#include "rhi/mesh_store.h"
#include "rhi/scene_builder.h"
#include "rhi/texture_store.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace heaven {
namespace {
bool finite(const glm::mat4& matrix) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(matrix[column][row])) return false;
    return true;
}

void upload_rgba(filament::Engine& engine, filament::Texture& texture,
                 std::shared_ptr<std::vector<glm::vec4>> pixels) {
    const auto* data = pixels->data();
    const size_t bytes = pixels->size() * sizeof(glm::vec4);
    texture.setImage(engine, 0, filament::Texture::PixelBufferDescriptor::make(
        data, bytes, filament::Texture::Format::RGBA, filament::Texture::Type::FLOAT,
        [pixels = std::move(pixels)](const void*, size_t) mutable { pixels.reset(); }));
}

filament::TextureSampler nearest_sampler() {
    return {filament::TextureSampler::MinFilter::NEAREST,
            filament::TextureSampler::MagFilter::NEAREST,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE};
}
} // namespace

TemporalScene::~TemporalScene() {
    if (!core_.engine) return;
    clear();
    destroy_targets();
    if (view_) core_.engine->destroy(view_);
    if (skybox_) core_.engine->destroy(skybox_);
    if (scene_) core_.engine->destroy(scene_);
    if (neutral_) core_.engine->destroy(neutral_);
    if (camera_) {
        core_.engine->destroyCameraComponent(camera_entity_);
        utils::EntityManager::get().destroy(camera_entity_);
    }
}

void TemporalScene::destroy_targets() {
    if (view_) view_->setRenderTarget(nullptr);
    if (target_) core_.engine->destroy(target_);
    if (packed_) core_.engine->destroy(packed_);
    if (depth_) core_.engine->destroy(depth_);
    target_ = nullptr;
    packed_ = depth_ = nullptr;
    width_ = height_ = 0;
}

bool TemporalScene::ensure(int width, int height) {
    if (!core_.engine || width <= 0 || height <= 0) return false;
    auto& engine = *core_.engine;
    if (!scene_) {
        scene_ = engine.createScene();
        skybox_ = filament::Skybox::Builder().color({0.0f, 0.0f, 0.0f, 0.0f}).build(engine);
        if (!scene_ || !skybox_) return false;
        scene_->setSkybox(skybox_);
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine.createCamera(camera_entity_);
        view_ = engine.createView();
        if (!camera_ || !view_) return false;
        view_->setScene(scene_);
        view_->setCamera(camera_);
        view_->setPostProcessingEnabled(false);
        view_->setShadowingEnabled(false);
        neutral_ = filament::Texture::Builder().width(1).height(1).levels(1)
            .format(filament::Texture::InternalFormat::RGBA32F).build(engine);
        if (!neutral_) return false;
        upload_rgba(engine, *neutral_, std::make_shared<std::vector<glm::vec4>>(1, glm::vec4(0.0f)));
    }
    if (!view_ || !camera_ || !neutral_) return false;
    if (width_ == width && height_ == height && target_) return true;
    destroy_targets();
    invalidate_history();
    // Filament's usage enum is a bitmask despite the scoped enum declaration.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto color_usage = static_cast<filament::Texture::Usage>(
        std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
        std::to_underlying(filament::Texture::Usage::SAMPLEABLE) |
        std::to_underlying(filament::Texture::Usage::BLIT_SRC));
    packed_ = filament::Texture::Builder().width(static_cast<uint32_t>(width))
        .height(static_cast<uint32_t>(height)).levels(1).usage(color_usage)
        .format(filament::Texture::InternalFormat::RGBA32F).build(engine);
    depth_ = filament::Texture::Builder().width(static_cast<uint32_t>(width))
        .height(static_cast<uint32_t>(height)).levels(1)
        .usage(filament::Texture::Usage::DEPTH_ATTACHMENT)
        .format(filament::Texture::InternalFormat::DEPTH32F).build(engine);
    if (!packed_ || !depth_) { destroy_targets(); return false; }
    target_ = filament::RenderTarget::Builder()
        .texture(filament::RenderTarget::AttachmentPoint::COLOR0, packed_)
        .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine);
    if (!target_) { destroy_targets(); return false; }
    width_ = width;
    height_ = height;
    view_->setRenderTarget(target_);
    view_->setViewport({0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
    return true;
}

bool TemporalScene::build(MeshStore& meshes, TextureStore& textures,
                          std::span<const Instance> instances,
                          std::span<const GpuMesh> gpu_meshes,
                          const MaterialResolver& resolve_material, const AppConfig& config,
                          filament::Material* motion_material, filament::Material* masked_material) {
    if (built_) return true;
    if (!scene_ || !neutral_ || !motion_material || !masked_material) return false;
    auto& engine = *core_.engine;
    filament::TextureSampler alpha_sampler(filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR, filament::TextureSampler::WrapMode::REPEAT);
    filament::TextureSampler height_sampler(filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR, filament::TextureSampler::WrapMode::REPEAT);
    height_sampler.setAnisotropy(1.0f);
    for (size_t index = 0; index < instances.size(); ++index) {
        const auto& instance = instances[index];
        if (instance.mesh_index < 0 || std::cmp_greater_equal(instance.mesh_index, gpu_meshes.size())) continue;
        const auto& mesh = gpu_meshes[static_cast<size_t>(instance.mesh_index)];
        for (const auto& primitive : select_renderable_prims(meshes, mesh, resolve_material)) {
            const auto& material = primitive.gpu_mat;
            auto* mi = (material.alpha_ref > 0.0f ? masked_material : motion_material)->createInstance();
            if (!mi) { clear(); return false; }
            const bool two_sided = material.two_sided || material.alpha_ref > 0.0f;
            mi->setDoubleSided(two_sided);
            mi->setCullingMode(two_sided ? filament::MaterialInstance::CullingMode::NONE :
                                         filament::MaterialInstance::CullingMode::BACK);
            if (material.alpha_ref > 0.0f) {
                const auto* diffuse = textures.get(material.diffuse);
                mi->setParameter("baseColorMap", diffuse ? diffuse : textures.placeholder(), alpha_sampler);
            }
            const auto* height = textures.get(material.height);
            mi->setParameter("geometryHeightMap", height ? height : textures.placeholder(), height_sampler);
            mi->setParameter("baseTransform", filament::math::float4{
                material.base_transform[0], material.base_transform[1], material.base_transform[2], material.base_transform[3]});
            const auto displacement = geometry_displacement(material, height != nullptr,
                config.tessellation.scale, config.tessellation.distance, config.tessellation.enabled);
            mi->setParameter("geometryDisplacement", displacement.enabled);
            mi->setParameter("displacementParams", filament::math::float4{displacement.amplitude,
                displacement.power, displacement.fade.threshold,
                displacement.enabled ? displacement.fade.inverse_range() : 0.0f});
            mi->setParameter("previousModel", glm_to_filament(instance.transform));
            mi->setParameter("previousClipFromUserWorld", glm_to_filament(glm::mat4(1.0f)));
            mi->setParameter("currentClipFromUserWorld", glm_to_filament(glm::mat4(1.0f)));
            mi->setParameter("previousCamera", filament::math::float3{});
            mi->setParameter("historyValid", false);
            mi->setParameter("hasPreviousBones", false);
            mi->setParameter("previousBones", neutral_, nearest_sampler());
            mi->setParameter("hasPreviousPositions", false);
            mi->setParameter("previousPositions", neutral_, nearest_sampler());
            mi->setParameter("previousPositionWidth", 1);

            const auto skin = meshes.skinned_info(primitive.surface_handle);
            const uint32_t bone_count = config.features.gpu_skinning && skin.skinned ? skin.bone_count : 0;
            const bool prepared = meshes.is_displacement_prepared(primitive.surface_handle);
            const auto bmin = prepared ? primitive.bounds_min : mesh.bbox_min;
            const auto bmax = prepared ? primitive.bounds_max : mesh.bbox_max;
            const auto extent = bmax - bmin;
            const float volume = extent.x * extent.y * extent.z;
            const auto box = box_from_min_max({bmin.x - 1.0f, bmin.y - 1.0f, bmin.z - 1.0f},
                {bmax.x + 1.0f, bmax.y + 1.0f, bmax.z + 1.0f});
            const auto entity = utils::EntityManager::get().create();
            filament::RenderableManager::Builder builder(1);
            builder.geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                primitive.vb, primitive.ib, primitive.index_offset, primitive.index_count)
                .material(0, mi).castShadows(false).receiveShadows(false).boundingBox(box).culling(false);
            if (bone_count > 0) builder.skinning(bone_count);
            if (builder.build(engine, entity) != filament::RenderableManager::Builder::Success) {
                engine.destroy(mi);
                utils::EntityManager::get().destroy(entity);
                clear();
                return false;
            }
            auto& transforms = engine.getTransformManager();
            transforms.create(entity);
            transforms.setTransform(transforms.getInstance(entity), glm_to_filament(instance.transform));
            scene_->addEntity(entity);
            proxies_.push_back({entity, mi, index, instance.mesh_index, primitive.surface_handle,
                bone_count, skin.skinned_source, prepared,
                !skin.skinned_source && std::isfinite(volume) && volume <= 100000.0f,
                {bmin, bmax, {box.center.x, box.center.y, box.center.z},
                            {box.halfExtent.x, box.halfExtent.y, box.halfExtent.z}}});
            ++coverage_.proxy_primitives;
            if (bone_count > 0) ++coverage_.skinned_primitives;
            if (material.glass) ++coverage_.glass_primitives;
        }
    }
    current_models_.resize(instances.size(), glm::mat4(1.0f));
    previous_models_ = current_models_;
    built_ = true;
    invalidate_history();
    return true;
}

bool TemporalScene::set_bones(int mesh_index, std::span<const glm::mat4> matrices) {
    if (mesh_index < 0 || matrices.empty() || matrices.size() > 255 ||
        !std::ranges::all_of(matrices, [](const auto& matrix) { return finite(matrix); })) {
        if (const auto found = bones_.find(mesh_index); found != bones_.end()) {
            found->second.current.clear();
            found->second.previous.clear();
        }
        return false;
    }
    auto& history = bones_[mesh_index];
    if (history.current.size() != matrices.size()) history.previous.clear();
    history.current.assign(matrices.begin(), matrices.end());
    return true;
}

bool TemporalScene::set_positions(uint64_t surface_handle, std::span<const Vertex> vertices) {
    constexpr size_t max_positions = 2048u * 2048u;
    if (surface_handle == 0 || vertices.empty() || vertices.size() > max_positions ||
        !std::ranges::all_of(vertices, [](const auto& vertex) {
        return std::isfinite(vertex.px) && std::isfinite(vertex.py) && std::isfinite(vertex.pz);
    })) {
        if (const auto found = positions_.find(surface_handle); found != positions_.end()) {
            found->second.current.clear();
            found->second.previous.clear();
        }
        return false;
    }
    auto& history = positions_[surface_handle];
    if (history.current.size() != vertices.size()) history.previous.clear();
    history.current.resize(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i)
        history.current[i] = {vertices[i].px, vertices[i].py, vertices[i].pz, 1.0f};
    return true;
}

bool TemporalScene::upload_bone_history(BoneHistory& history) {
    if (history.previous.empty() || history.previous.size() != history.current.size()) return false;
    const auto height = static_cast<uint32_t>(history.previous.size());
    if (!history.texture || history.texture_height != height) {
        if (history.texture) {
            for (const auto& proxy : proxies_) {
                proxy.material->setParameter("previousBones", neutral_, nearest_sampler());
                proxy.material->setParameter("hasPreviousBones", false);
            }
            core_.engine->destroy(history.texture);
        }
        history.texture = filament::Texture::Builder().width(3).height(height).levels(1)
            .format(filament::Texture::InternalFormat::RGBA32F).build(*core_.engine);
        history.texture_height = height;
    }
    if (!history.texture) return false;
    auto pixels = std::make_shared<std::vector<glm::vec4>>(history.previous.size() * 3);
    for (size_t bone = 0; bone < history.previous.size(); ++bone)
        for (size_t row = 0; row < 3; ++row) {
            const auto& matrix = history.previous[bone];
            (*pixels)[bone * 3 + row] = {matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]};
        }
    upload_rgba(*core_.engine, *history.texture, std::move(pixels));
    return true;
}

bool TemporalScene::upload_position_history(PositionHistory& history) {
    if (history.previous.empty() || history.previous.size() != history.current.size()) return false;
    const uint32_t width = static_cast<uint32_t>(std::min<size_t>(2048, history.previous.size()));
    const uint32_t height = static_cast<uint32_t>((history.previous.size() + width - 1) / width);
    if (!history.texture || history.texture_width != width || history.texture_height != height) {
        if (history.texture) {
            for (const auto& proxy : proxies_) {
                proxy.material->setParameter("previousPositions", neutral_, nearest_sampler());
                proxy.material->setParameter("hasPreviousPositions", false);
            }
            core_.engine->destroy(history.texture);
        }
        history.texture = filament::Texture::Builder().width(width).height(height).levels(1)
            .format(filament::Texture::InternalFormat::RGBA32F).build(*core_.engine);
        history.texture_width = width;
        history.texture_height = height;
    }
    if (!history.texture) return false;
    auto pixels = std::make_shared<std::vector<glm::vec4>>(static_cast<size_t>(width) * height, glm::vec4(0.0f));
    std::ranges::copy(history.previous, pixels->begin());
    upload_rgba(*core_.engine, *history.texture, std::move(pixels));
    return true;
}

bool TemporalScene::prepare(const Camera& camera, std::span<const Instance> instances,
                            const glm::mat4& current_clip_from_user_world) {
    prepared_ = false;
    if (!built_ || !target_ || !core_.camera || instances.size() != current_models_.size() ||
        !finite(current_clip_from_user_world)) { invalidate_history(); return false; }
    const auto view = camera.view_matrix();
    const auto model = glm::inverse(view);
    // Rendering keeps the main view's exact infinite-far projection, while
    // culling needs its finite far plane. Normalizing an infinite far plane
    // produces NaNs and can reject every proxy in Filament's optimized culler.
    const auto scaling=core_.camera->getScaling();
    const auto shift=core_.camera->getShift();
    const auto culling_projection=temporal_gl_projection(core_.camera->getCullingProjectionMatrix(),
        {float(scaling.x),float(scaling.y)},{float(shift.x),float(shift.y)});
    camera_->setCustomProjection(filament::math::mat4(glm_to_filament(current_clip_from_user_world * model)),
        filament::math::mat4(glm_to_filament(culling_projection)),core_.camera->getNear(),core_.camera->getCullingFar());
    camera_->setModelMatrix(glm_to_filament(model));
    view_->setMaterialGlobal(1, {camera.position.x, camera.position.y, camera.position.z, 1.0f});
    current_clip_ = current_clip_from_user_world;
    current_camera_ = camera.position;
    for (size_t i = 0; i < instances.size(); ++i) current_models_[i] = instances[i].transform;
    for (auto& [index, history] : bones_) upload_bone_history(history);
    for (auto& [handle, history] : positions_) upload_position_history(history);
    coverage_.incomplete_history_primitives = 0;
    coverage_.missing_current_bones = 0;
    coverage_.missing_current_positions = 0;
    coverage_.cpu_deformed_primitives = 0;
    auto& transforms = core_.engine->getTransformManager();
    auto& renderables = core_.engine->getRenderableManager();
    const auto grid_origin = core_.engine->getFeatureFlag("view.enable_grid_based_world_origin");
    const bool supported_origins = core_.camera && grid_origin && !*grid_origin;
    for (const auto& proxy : proxies_) {
        const auto& current = current_models_[proxy.instance_index];
        const auto& previous = history_valid_ ? previous_models_[proxy.instance_index] : current;
        auto* mi = proxy.material;
        transforms.setTransform(transforms.getInstance(proxy.entity), glm_to_filament(current));
        mi->setParameter("previousModel", glm_to_filament(previous));
        mi->setParameter("currentClipFromUserWorld", glm_to_filament(current_clip_));
        mi->setParameter("previousClipFromUserWorld", glm_to_filament(history_valid_ ? previous_clip_ : current_clip_));
        const auto prior_camera = history_valid_ ? previous_camera_ : current_camera_;
        mi->setParameter("previousCamera", filament::math::float3{prior_camera.x, prior_camera.y, prior_camera.z});
        bool valid = history_valid_ && finite(current) && finite(previous);
        const auto ri = renderables.getInstance(proxy.entity);
        bool previous_bones = false;
        if (proxy.bone_count > 0) {
            const auto found = bones_.find(proxy.mesh_index);
            if (found == bones_.end() || found->second.current.size() != proxy.bone_count) {
                valid = false;
                ++coverage_.missing_current_bones;
            } else {
                std::vector<filament::math::mat4f> matrices;
                matrices.reserve(found->second.current.size());
                for (const auto& matrix : found->second.current) matrices.push_back(glm_to_filament(matrix));
                renderables.setBones(ri, matrices.data(), matrices.size(), 0);
                previous_bones = found->second.texture && found->second.previous.size() == proxy.bone_count;
                valid &= previous_bones;
                mi->setParameter("previousBones", previous_bones ? found->second.texture : neutral_, nearest_sampler());
            }
        }
        mi->setParameter("hasPreviousBones", previous_bones);
        bool previous_positions = false;
        if (const auto found = positions_.find(proxy.surface_handle); found != positions_.end()) {
            ++coverage_.cpu_deformed_primitives;
            if (found->second.current.empty()) ++coverage_.missing_current_positions;
            previous_positions = found->second.texture &&
                found->second.previous.size() == found->second.current.size() && !found->second.previous.empty();
            valid &= previous_positions;
            mi->setParameter("previousPositions", previous_positions ? found->second.texture : neutral_, nearest_sampler());
            mi->setParameter("previousPositionWidth", previous_positions ? static_cast<int>(found->second.texture_width) : 1);
        } else if (proxy.skinned_source && proxy.bone_count == 0) {
            valid = false;
            ++coverage_.missing_current_positions;
        }
        mi->setParameter("hasPreviousPositions", previous_positions);
        mi->setParameter("historyValid", valid);
        if (!valid) ++coverage_.incomplete_history_primitives;
        const bool culling = proxy.prepared ? prepared_surface_culling_enabled(proxy.base_culling, current,
            proxy.culling_bounds, glm::dvec3(camera.position), supported_origins) :
            proxy.base_culling && !positions_.contains(proxy.surface_handle) && instance_transform_supports_culling(current);
        renderables.setCulling(ri, culling);
    }
    prepared_ = true;
    return true;
}

void TemporalScene::commit_accepted() {
    if (!prepared_) return;
    previous_models_ = current_models_;
    previous_clip_ = current_clip_;
    previous_camera_ = current_camera_;
    for (auto& [index, history] : bones_) history.previous = history.current;
    for (auto& [handle, history] : positions_) history.previous = history.current;
    history_valid_ = true;
    prepared_ = false;
}

void TemporalScene::invalidate_history() noexcept {
    history_valid_ = false;
    prepared_ = false;
    for (auto& [index, history] : bones_) history.previous.clear();
    for (auto& [handle, history] : positions_) history.previous.clear();
}

void TemporalScene::clear() {
    if (!core_.engine) return;
    for (const auto& proxy : proxies_) {
        if (scene_) scene_->remove(proxy.entity);
        core_.engine->destroy(proxy.entity);
        utils::EntityManager::get().destroy(proxy.entity);
        core_.engine->destroy(proxy.material);
    }
    proxies_.clear();
    for (auto& [index, history] : bones_) if (history.texture) core_.engine->destroy(history.texture);
    for (auto& [handle, history] : positions_) if (history.texture) core_.engine->destroy(history.texture);
    bones_.clear();
    positions_.clear();
    current_models_.clear();
    previous_models_.clear();
    coverage_ = {};
    built_ = false;
    invalidate_history();
}

} // namespace heaven
