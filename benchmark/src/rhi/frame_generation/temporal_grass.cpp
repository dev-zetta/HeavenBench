#include "rhi/frame_generation/temporal_grass.h"

#include "rhi/filament_util.h"

#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <utils/EntityManager.h>
#include <math/mat3.h>

#include <cmath>

namespace heaven {
namespace {
bool finite(const glm::mat4& matrix) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(matrix[column][row])) return false;
    return true;
}
filament::math::float4 vector_parameter(const glm::vec4& value) {
    return {value.x, value.y, value.z, value.w};
}
filament::math::mat3f matrix_parameter(const glm::mat3& matrix) {
    return {filament::math::float3{matrix[0].x, matrix[0].y, matrix[0].z},
            filament::math::float3{matrix[1].x, matrix[1].y, matrix[1].z},
            filament::math::float3{matrix[2].x, matrix[2].y, matrix[2].z}};
}
} // namespace

TemporalGrass::~TemporalGrass() { clear(); }

bool TemporalGrass::matching_ranges(std::span<const TemporalGrassPatch> patches) const {
    if (patches.size() != proxies_.size()) return false;
    for (size_t i = 0; i < patches.size(); ++i) {
        const auto& source = proxies_[i].source;
        const auto& patch = patches[i];
        if (source.source_entity != patch.source_entity || source.vertices != patch.vertices ||
            source.indices != patch.indices || source.instance_index != patch.instance_index ||
            source.index_offset != patch.index_offset || source.index_count != patch.index_count)
            return false;
    }
    return true;
}

bool TemporalGrass::rebuild(std::span<const TemporalGrassPatch> patches) {
    clear();
    if (!core_.engine || !scene_ || !material_) return false;
    auto& engine = *core_.engine;
    proxies_.reserve(patches.size());
    for (const auto& patch : patches) {
        if (!patch.source_entity || !patch.vertices || !patch.indices || !patch.diffuse ||
            patch.index_count == 0) { clear(); return false; }
        auto* mi = material_->createInstance();
        if (!mi) { clear(); return false; }
        // All uniforms are explicitly initialized before the first submission.
        mi->setParameter("time", 0.0f);
        mi->setParameter("previousTime", 0.0f);
        mi->setParameter("previousModel", glm_to_filament(glm::mat4(1.0f)));
        mi->setParameter("currentClipFromUserWorld", glm_to_filament(glm::mat4(1.0f)));
        mi->setParameter("previousClipFromUserWorld", glm_to_filament(glm::mat4(1.0f)));
        mi->setParameter("historyValid", false);
        mi->setParameter("currentWindToUser", filament::math::mat3f{});
        mi->setParameter("previousWindToUser", filament::math::mat3f{});
        const auto entity = utils::EntityManager::get().create();
        filament::RenderableManager::Builder builder(1);
        builder.geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                patch.vertices, patch.indices, patch.index_offset, patch.index_count)
            .material(0, mi).boundingBox(patch.bounds).culling(patch.culling)
            .castShadows(false).receiveShadows(false);
        if (builder.build(engine, entity) != filament::RenderableManager::Builder::Success) {
            engine.destroy(mi);
            utils::EntityManager::get().destroy(entity);
            clear();
            return false;
        }
        engine.getTransformManager().create(entity);
        proxies_.push_back({patch, entity, mi});
    }
    return true;
}

bool TemporalGrass::prepare(std::span<const TemporalGrassPatch> patches,
        std::span<const Instance> instances, float time, const glm::mat4& clip) {
    prepared_ = false;
    if (!core_.engine || !scene_ || !material_ || !std::isfinite(time) || !finite(clip)) {
        invalidate_history();
        return false;
    }
    if (!matching_ranges(patches) && !rebuild(patches)) return false;
    current_clip_ = clip;
    current_time_ = time;
    // FView::computeCameraInfo rotates Filament world by transpose(IBL
    // rotation). Visible grass adds its wind in that world, so convert it back
    // to user world and retain the accepted basis across IBL/time changes.
    current_wind_to_user_ = glm::mat3(1.0f);
    if (core_.scene) {
        if (const auto* ibl = core_.scene->getIndirectLight()) {
            const auto rotation = transpose(orthogonalize(
                filament::math::mat3(transpose(ibl->getRotation()))));
            for (int column = 0; column < 3; ++column)
                for (int row = 0; row < 3; ++row)
                    current_wind_to_user_[column][row] = static_cast<float>(rotation[column][row]);
        }
    }
    coverage_ = {.ranges = proxies_.size()};
    auto& transforms = core_.engine->getTransformManager();
    auto& renderables = core_.engine->getRenderableManager();
    const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR, filament::TextureSampler::WrapMode::REPEAT);
    for (size_t i = 0; i < proxies_.size(); ++i) {
        auto& proxy = proxies_[i];
        const auto& patch = patches[i];
        const bool valid_model = patch.instance_index < instances.size() &&
            finite(instances[patch.instance_index].transform);
        const bool visible = patch.visible && valid_model && patch.diffuse;
        const bool valid = history_valid_ && proxy.previous_visible && visible;
        if (proxy.visible != visible) {
            if (visible) scene_->addEntity(proxy.entity);
            else scene_->remove(proxy.entity);
            proxy.visible = visible;
        }
        if (!valid_model || !patch.diffuse) {
            proxy.material->setParameter("historyValid", false);
            if (patch.visible) ++coverage_.unavailable_ranges;
            continue;
        }
        proxy.source = patch;
        proxy.current_model = instances[patch.instance_index].transform;
        transforms.setTransform(transforms.getInstance(proxy.entity), glm_to_filament(proxy.current_model));
        const auto renderable = renderables.getInstance(proxy.entity);
        renderables.setAxisAlignedBoundingBox(renderable, patch.bounds);
        renderables.setCulling(renderable, patch.culling);
        proxy.material->setParameter("grassDiffuse", patch.diffuse, sampler);
        proxy.material->setParameter("grassDiffuseUv", vector_parameter(patch.diffuse_uv));
        proxy.material->setParameter("grassWindTuning", vector_parameter(patch.wind_tuning));
        proxy.material->setParameter("time", time);
        proxy.material->setParameter("previousTime", previous_time_);
        proxy.material->setParameter("currentWindToUser", matrix_parameter(current_wind_to_user_));
        proxy.material->setParameter("previousWindToUser", matrix_parameter(previous_wind_to_user_));
        proxy.material->setParameter("previousModel", glm_to_filament(proxy.previous_model));
        proxy.material->setParameter("currentClipFromUserWorld", glm_to_filament(clip));
        proxy.material->setParameter("previousClipFromUserWorld", glm_to_filament(previous_clip_));
        proxy.material->setParameter("historyValid", valid);
        if (visible) {
            ++coverage_.visible_ranges;
            if (!valid) ++coverage_.incomplete_history_ranges;
        }
    }
    prepared_ = true;
    return true;
}

void TemporalGrass::commit_accepted() {
    if (!prepared_) return;
    for (auto& proxy : proxies_) {
        proxy.previous_model = proxy.current_model;
        proxy.previous_visible = proxy.visible;
    }
    previous_clip_ = current_clip_;
    previous_time_ = current_time_;
    previous_wind_to_user_ = current_wind_to_user_;
    history_valid_ = true;
    prepared_ = false;
}

void TemporalGrass::invalidate_history() noexcept {
    prepared_ = false;
    history_valid_ = false;
    for (auto& proxy : proxies_) proxy.previous_visible = false;
}

void TemporalGrass::clear() {
    if (core_.engine) {
        for (auto& proxy : proxies_) {
            if (scene_ && proxy.visible) scene_->remove(proxy.entity);
            core_.engine->destroy(proxy.entity);
            utils::EntityManager::get().destroy(proxy.entity);
            core_.engine->destroy(proxy.material);
        }
    }
    proxies_.clear();
    coverage_ = {};
    invalidate_history();
}

} // namespace heaven
