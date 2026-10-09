/*
 * HeavenBench — Grass depth-proxy helper implementation
 */
#include "rhi/grass_depth_proxy.h"
#include "engine/feature_assets.h"
#include "rhi/filament_bounds.h"
#include "rhi/grass_patch_bounds.h"
#include "rhi/filament_util.h"          // glm_to_filament
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"         // k_heaven_grass_depth_filamat
#include "render_tuning.h"
#include "rhi/render_feature.h"         // IResourceProvider
#include "engine/path_resolver.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/TextureSampler.h>
#include <filament/Texture.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>

#include <utils/EntityManager.h>

#include <cstdio>
#include <stdexcept>

namespace heaven {

void GrassDepthProxy::init(filament::Engine* engine, IResourceProvider& resources,
                           const PathResolver& paths) {
    uint64_t h_diffuse = resources.load_texture(
        feature_asset_path(paths, FeatureAsset::GrassDiffuse),
        TextureSemantic::ColorSRGB);
    const filament::Texture* diffuse = resources.get_filament_texture(h_diffuse);
    if (!diffuse) diffuse = resources.placeholder_texture();
    init(engine, diffuse);
}

void GrassDepthProxy::init(filament::Engine* engine, const filament::Texture* diffuse) {
    material_ = filament::Material::Builder()
        .package(k_heaven_grass_depth_filamat, k_heaven_grass_depth_filamat_size)
        .build(*engine);
    if (!material_) {
        std::printf("  GrassDepthProxy: heaven_grass_depth material failed to load — "
                    "grass will not occlude screen-space depth effects\n");
        return;
    }

    mi_ = material_->createInstance();
    if (!mi_) {
        engine->destroy(material_);
        material_ = nullptr;
        return;
    }
    material_tuning::bind_grass_depth(*mi_);

    // Read the same diffuse alpha as the visible grass.
    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
    mi_->setParameter("grassDiffuse", diffuse, sampler);
    mi_->setParameter("time", 0.0f);
}

void GrassDepthProxy::add_patch(filament::Engine* engine, filament::Scene* depth_scene,
                                filament::VertexBuffer* vb, filament::IndexBuffer* ib,
                                std::uint32_t index_offset, std::uint32_t index_count,
                                const glm::mat4& transform, const GrassPatchBounds& bounds) {
    if (!ready() || !depth_scene || !vb || !ib || !index_count) return;
    depth_scene_ = depth_scene;

    auto entity = utils::EntityManager::get().create();

    // One primitive sharing the visible patch's VB/IB and the shared depth
    // instance and the exact same wind-expanded bounds as the visible patch.
    const auto built = filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  vb, ib, index_offset, index_count)
        .material(0, mi_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(bounds.local.has_value())
        .boundingBox(bounds.box())
        .build(*engine, entity);
    if (built != filament::RenderableManager::Builder::Success) {
        engine->destroy(entity);
        utils::EntityManager::get().destroy(entity);
        throw std::runtime_error("Grass depth-proxy range could not be built");
    }

    auto& tcm = engine->getTransformManager();
    tcm.create(entity);
    tcm.setTransform(tcm.getInstance(entity), glm_to_filament(transform));

    depth_scene->addEntity(entity);
    entities_.push_back(entity);
    visible_.push_back(true);
}

void GrassDepthProxy::set_time(float time) {
    if (mi_) mi_->setParameter("time", time);
}

void GrassDepthProxy::set_transform(filament::Engine* engine, size_t i,
                                    const glm::mat4& transform) {
    if (i >= entities_.size()) return;
    auto& tcm = engine->getTransformManager();
    auto ti = tcm.getInstance(entities_[i]);
    if (ti) tcm.setTransform(ti, glm_to_filament(transform));
}

void GrassDepthProxy::set_bounds(filament::Engine* engine, size_t i,
                                  const GrassPatchBounds& bounds) {
    if (i < entities_.size()) bounds.apply(engine->getRenderableManager(), entities_[i]);
}

void GrassDepthProxy::set_visible(size_t i, bool visible) {
    if (!depth_scene_ || i >= entities_.size() || i >= visible_.size() ||
        visible_[i] == visible) return;
    if (visible) depth_scene_->addEntity(entities_[i]);
    else         depth_scene_->remove(entities_[i]);
    visible_[i] = visible;
}

void GrassDepthProxy::destroy(filament::Engine* engine) {
    if (!engine) return;
    for (auto& e : entities_) {
        if (depth_scene_) depth_scene_->remove(e);
        engine->destroy(e);
        utils::EntityManager::get().destroy(e);
    }
    entities_.clear();
    visible_.clear();
    if (mi_) { engine->destroy(mi_); mi_ = nullptr; }
    if (material_) { engine->destroy(material_); material_ = nullptr; }
    depth_scene_ = nullptr;
}

} // namespace heaven
