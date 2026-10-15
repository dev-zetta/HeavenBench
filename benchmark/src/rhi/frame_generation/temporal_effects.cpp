#include "rhi/frame_generation/temporal_effects.h"

#include "rhi/filament_util.h"
#include "rhi/frame_generation/temporal_camera.h"
#include "rhi/materials_data.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/Texture.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace heaven {
namespace {
bool finite(const glm::mat4& matrix) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(matrix[column][row])) return false;
    return true;
}

template<class Matrix>
glm::mat4 glm_matrix(const Matrix& value) {
    glm::mat4 result;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            result[column][row] = static_cast<float>(value[column][row]);
    return result;
}

filament::math::mat3f matrix_parameter(const glm::mat3& value) {
    return {filament::math::float3{value[0].x, value[0].y, value[0].z},
            filament::math::float3{value[1].x, value[1].y, value[1].z},
            filament::math::float3{value[2].x, value[2].y, value[2].z}};
}

filament::math::float3 vector_parameter(const glm::vec3& value) {
    return {value.x, value.y, value.z};
}

// These affect geometric placement or particle identity. Illumination, opacity
// and current camera/time are photometric/current inputs, not a global reset.
bool geometric_parameter(std::string_view name) {
    constexpr std::array names = {"spawnInterval", "particleMotionTuning", "particleShapeTuning",
        "seed", "streakLenMin", "streakLenMax", "fallSpeed", "volumeExtent", "activeStreaks",
        "rainShaderTuning", "cloudHeight", "cloudBottom", "cloudTop", "noiseScale",
        "cloudMotionTuning", "fadeFar", "fadeNear", "maxRadius"};
    return std::ranges::find(names, name) != names.end();
}

template<class Value>
void copy_parameter(filament::MaterialInstance& destination,
        const filament::MaterialInstance& source, const char* name, uint64_t& signature) {
    const auto value = source.getParameter<Value>(name);
    destination.setParameter(name, value);
    if (geometric_parameter(name)) {
        for (const auto byte : std::as_bytes(std::span(&value, 1))) {
            signature ^= std::to_integer<uint8_t>(byte);
            signature *= 1099511628211ULL;
        }
    }
}

bool copy_parameters(filament::MaterialInstance& destination,
        const filament::MaterialInstance& source,
        std::span<const filament::Material::ParameterInfo> parameters, uint64_t& signature) {
    using Type = filament::Material::ParameterType;
    signature = 1469598103934665603ULL;
    for (const auto& parameter : parameters) {
        if (parameter.isSampler || parameter.isSubpass) continue;
        if (parameter.count != 1 || !destination.getMaterial()->hasParameter(parameter.name)) return false;
        // Filament reflects this injected BOOL alongside authored parameters,
        // but exposes its value through the supported render-state API.
        if (parameter.type == Type::BOOL && std::string_view(parameter.name) == "_doubleSided") {
            const bool double_sided = source.isDoubleSided();
            destination.setDoubleSided(double_sided);
            signature ^= static_cast<uint64_t>(double_sided);
            signature *= 1099511628211ULL;
            continue;
        }
        switch (parameter.type) {
            case Type::INT: copy_parameter<int32_t>(destination, source, parameter.name, signature); break;
            case Type::UINT: copy_parameter<uint32_t>(destination, source, parameter.name, signature); break;
            case Type::FLOAT: copy_parameter<float>(destination, source, parameter.name, signature); break;
            case Type::FLOAT2: copy_parameter<filament::math::float2>(destination, source, parameter.name, signature); break;
            case Type::FLOAT3: copy_parameter<filament::math::float3>(destination, source, parameter.name, signature); break;
            case Type::FLOAT4: copy_parameter<filament::math::float4>(destination, source, parameter.name, signature); break;
            case Type::MAT3: copy_parameter<filament::math::mat3f>(destination, source, parameter.name, signature); break;
            case Type::MAT4: copy_parameter<filament::math::mat4f>(destination, source, parameter.name, signature); break;
            // Pinned Filament does not instantiate getParameter<bool>.
            // Unsupported authored inputs must still fail closed.
            default: return false;
        }
    }
    return true;
}

struct Package { const uint8_t* data; size_t size; };
Package effect_package(TemporalEffectKind kind) {
    switch (kind) {
        case TemporalEffectKind::Smoke: return {k_heaven_temporal_smoke_filamat, k_heaven_temporal_smoke_filamat_size};
        case TemporalEffectKind::Flame: return {k_heaven_temporal_flame_filamat, k_heaven_temporal_flame_filamat_size};
        case TemporalEffectKind::Glow: return {k_heaven_temporal_glow_filamat, k_heaven_temporal_glow_filamat_size};
        case TemporalEffectKind::Firefly: return {k_heaven_temporal_firefly_filamat, k_heaven_temporal_firefly_filamat_size};
        case TemporalEffectKind::Rain: return {k_heaven_temporal_rain_filamat, k_heaven_temporal_rain_filamat_size};
        case TemporalEffectKind::LensFlare: return {k_heaven_temporal_lens_flare_filamat, k_heaven_temporal_lens_flare_filamat_size};
        case TemporalEffectKind::CloudPlane: return {k_heaven_temporal_clouds_filamat, k_heaven_temporal_clouds_filamat_size};
        case TemporalEffectKind::VolumetricCloud: return {k_heaven_temporal_volumetric_clouds_filamat, k_heaven_temporal_volumetric_clouds_filamat_size};
    }
    return {};
}
} // namespace

struct TemporalEffects::Impl {
    explicit Impl(const BackendCore& value) : core(value) {}
    struct Proxy {
        TemporalEffectDraw source;
        utils::Entity entity;
        filament::MaterialInstance* material = nullptr;
        std::vector<filament::Material::ParameterInfo> parameters;
        glm::mat4 current_model{1.0f}, previous_model{1.0f};
        uint64_t current_signature = 0, previous_signature = 0;
        bool visible = false, previous_visible = false, present = false;
    };
    BackendCore core;
    filament::Scene* scene = nullptr;
    filament::View* view = nullptr;
    filament::Camera* camera = nullptr;
    filament::Skybox* skybox = nullptr;
    utils::Entity camera_entity;
    filament::Texture* packed = nullptr;
    filament::Texture* depth = nullptr;
    filament::RenderTarget* target = nullptr;
    std::array<filament::Material*, 8> materials{};
    std::unordered_map<uint64_t, Proxy> proxies;
    TemporalEffectCoverage coverage;
    int width = 0, height = 0;
    glm::mat4 current_clip{1.0f}, previous_clip{1.0f};
    glm::vec3 current_camera{}, previous_camera{}, current_right{}, previous_right{}, current_up{}, previous_up{};
    glm::mat3 current_world_to_user{1.0f}, previous_world_to_user{1.0f};
    float current_time = 0.0f, previous_time = 0.0f;
    bool prepared = false, history_valid = false;

    void destroy_proxy(Proxy& proxy) {
        if (proxy.visible && scene) scene->remove(proxy.entity);
        core.engine->destroy(proxy.entity);
        utils::EntityManager::get().destroy(proxy.entity);
        core.engine->destroy(proxy.material);
    }
    void destroy_targets() {
        if (view) view->setRenderTarget(nullptr);
        if (target) core.engine->destroy(target);
        if (packed) core.engine->destroy(packed);
        if (depth) core.engine->destroy(depth);
        target = nullptr;
        packed = depth = nullptr;
        width = height = 0;
    }
    void invalidate() {
        prepared = false;
        history_valid = false;
        for (auto& [identity, proxy] : proxies) proxy.previous_visible = false;
    }
    bool create_proxy(Proxy& proxy, const TemporalEffectDraw& draw) {
        auto& engine = *core.engine;
        const auto kind = static_cast<size_t>(draw.kind);
        if (kind >= materials.size()) return false;
        if (!materials[kind]) {
            const auto package = effect_package(draw.kind);
            materials[kind] = filament::Material::Builder().package(package.data, package.size).build(engine);
            if (!materials[kind]) return false;
        }
        proxy.source = draw;
        proxy.material = materials[kind]->createInstance();
        if (!proxy.material) return false;
        const auto* original = draw.source->getMaterial();
        proxy.parameters.resize(original->getParameterCount());
        original->getParameters(proxy.parameters.data(), proxy.parameters.size());
        proxy.entity = utils::EntityManager::get().create();
        const auto source_renderable = engine.getRenderableManager().getInstance(draw.source_entity);
        const auto box = engine.getRenderableManager().getAxisAlignedBoundingBox(source_renderable);
        proxy.material->setDepthCulling(true);
        proxy.material->setDepthWrite(true);
        const auto result = filament::RenderableManager::Builder(1)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, draw.vertices, draw.indices)
            .material(0, proxy.material).culling(false).boundingBox(box)
            .castShadows(false).receiveShadows(false).build(engine, proxy.entity);
        if (result != filament::RenderableManager::Builder::Success) {
            engine.destroy(proxy.material);
            proxy.material = nullptr;
            utils::EntityManager::get().destroy(proxy.entity);
            proxy.entity = {};
            return false;
        }
        engine.getTransformManager().create(proxy.entity);
        return true;
    }
};

TemporalEffects::TemporalEffects(const BackendCore& core) : impl_(std::make_unique<Impl>(core)) {}
TemporalEffects::~TemporalEffects() {
    auto& state = *impl_;
    if (!state.core.engine) return;
    for (auto& [identity, proxy] : state.proxies) state.destroy_proxy(proxy);
    state.destroy_targets();
    if (state.view) state.core.engine->destroy(state.view);
    if (state.scene) state.scene->setIndirectLight(nullptr);
    if (state.skybox) state.core.engine->destroy(state.skybox);
    if (state.scene) state.core.engine->destroy(state.scene);
    for (auto* material : state.materials) if (material) state.core.engine->destroy(material);
    if (state.camera) state.core.engine->destroyCameraComponent(state.camera_entity);
    if (state.camera_entity) utils::EntityManager::get().destroy(state.camera_entity);
}

bool TemporalEffects::ensure(int width, int height) {
    auto& state = *impl_;
    if (!state.core.engine || width <= 0 || height <= 0) return false;
    auto& engine = *state.core.engine;
    if (!state.scene) {
        state.scene = engine.createScene();
        state.skybox = filament::Skybox::Builder().color({0.0f, 0.0f, 0.0f, 0.0f}).build(engine);
        state.camera_entity = utils::EntityManager::get().create();
        state.camera = engine.createCamera(state.camera_entity);
        state.view = engine.createView();
        if (!state.scene || !state.skybox || !state.camera || !state.view) return false;
        state.scene->setSkybox(state.skybox);
        state.view->setScene(state.scene);
        state.view->setCamera(state.camera);
        state.view->setPostProcessingEnabled(false);
        state.view->setShadowingEnabled(false);
    }
    if (!state.scene || !state.skybox || !state.camera || !state.view) return false;
    if (state.width == width && state.height == height && state.target) return true;
    state.destroy_targets();
    state.invalidate();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto usage = static_cast<filament::Texture::Usage>(
        std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
        std::to_underlying(filament::Texture::Usage::SAMPLEABLE) |
        std::to_underlying(filament::Texture::Usage::BLIT_SRC));
    state.packed = filament::Texture::Builder().width(static_cast<uint32_t>(width))
        .height(static_cast<uint32_t>(height)).levels(1).usage(usage)
        .format(filament::Texture::InternalFormat::RGBA32F).build(engine);
    state.depth = filament::Texture::Builder().width(static_cast<uint32_t>(width))
        .height(static_cast<uint32_t>(height)).levels(1).usage(filament::Texture::Usage::DEPTH_ATTACHMENT)
        .format(filament::Texture::InternalFormat::DEPTH32F).build(engine);
    if (!state.packed || !state.depth) { state.destroy_targets(); return false; }
    state.target = filament::RenderTarget::Builder()
        .texture(filament::RenderTarget::AttachmentPoint::COLOR0, state.packed)
        .texture(filament::RenderTarget::AttachmentPoint::DEPTH, state.depth).build(engine);
    if (!state.target) { state.destroy_targets(); return false; }
    state.width = width;
    state.height = height;
    state.view->setRenderTarget(state.target);
    state.view->setViewport({0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
    return true;
}

bool TemporalEffects::prepare(std::span<const TemporalEffectDraw> draws, const Camera& camera,
        float time, const glm::mat4& current_clip_from_user_world) {
    auto& state = *impl_;
    state.prepared = false;
    if (!state.target || !state.core.camera || !finite(current_clip_from_user_world) || !std::isfinite(time)) {
        state.invalidate();
        return false;
    }
    const auto model = glm::inverse(camera.view_matrix());
    const auto scaling = state.core.camera->getScaling();
    const auto shift = state.core.camera->getShift();
    const auto culling = temporal_gl_projection(state.core.camera->getCullingProjectionMatrix(),
        {float(scaling.x), float(scaling.y)}, {float(shift.x), float(shift.y)});
    state.camera->setCustomProjection(filament::math::mat4(glm_to_filament(current_clip_from_user_world * model)),
        filament::math::mat4(glm_to_filament(culling)), state.core.camera->getNear(),
        state.core.camera->getCullingFar());
    state.camera->setModelMatrix(glm_to_filament(model));
    state.current_clip = current_clip_from_user_world;
    state.current_camera = camera.position;
    state.current_right = glm::vec3(model[0]);
    state.current_up = glm::vec3(model[1]);
    state.current_time = time;
    state.current_world_to_user = glm::mat3(1.0f);
    auto* ibl = state.core.scene ? state.core.scene->getIndirectLight() : nullptr;
    state.scene->setIndirectLight(ibl);
    if (ibl) {
        const auto rotation = transpose(orthogonalize(filament::math::mat3(transpose(ibl->getRotation()))));
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row)
                state.current_world_to_user[column][row] = static_cast<float>(rotation[column][row]);
    }
    state.coverage = {};
    auto& engine = *state.core.engine;
    auto& transforms = engine.getTransformManager();
    auto& renderables = engine.getRenderableManager();
    for (auto& [identity, proxy] : state.proxies) proxy.present = false;
    for (const auto& draw : draws) {
        if (!draw.identity || !draw.source_entity || !draw.vertices || !draw.indices || !draw.source ||
            !transforms.hasComponent(draw.source_entity) || !renderables.hasComponent(draw.source_entity)) {
            ++state.coverage.unavailable_draws;
            continue;
        }
        auto [entry, inserted] = state.proxies.try_emplace(draw.identity);
        auto& proxy = entry->second;
        if (!inserted && proxy.present) { ++state.coverage.unavailable_draws; continue; }
        const bool changed = !inserted && (proxy.source.source_entity != draw.source_entity ||
            proxy.source.kind != draw.kind || proxy.source.vertices != draw.vertices ||
            proxy.source.indices != draw.indices || proxy.source.source != draw.source);
        if (changed) { state.destroy_proxy(proxy); proxy = {}; }
        if ((inserted || changed) && !state.create_proxy(proxy, draw)) {
            state.proxies.erase(entry);
            ++state.coverage.unavailable_draws;
            continue;
        }
        proxy.present = true;
        proxy.current_model = glm_matrix(transforms.getWorldTransform(transforms.getInstance(draw.source_entity)));
        bool usable = finite(proxy.current_model) &&
            copy_parameters(*proxy.material, *draw.source, proxy.parameters, proxy.current_signature);
        for (const auto& texture : draw.textures) {
            if (!texture.name) continue;
            if (!texture.texture || !proxy.material->getMaterial()->hasParameter(texture.name)) usable = false;
            else proxy.material->setParameter(texture.name, texture.texture, texture.sampler);
        }
        if (proxy.visible != usable) {
            if (usable) state.scene->addEntity(proxy.entity);
            else state.scene->remove(proxy.entity);
            proxy.visible = usable;
        }
        if (!usable) { ++state.coverage.unavailable_draws; continue; }
        proxy.source = draw;
        const bool valid = state.history_valid && proxy.previous_visible &&
            proxy.current_signature == proxy.previous_signature;
        transforms.setTransform(transforms.getInstance(proxy.entity), glm_to_filament(proxy.current_model));
        proxy.material->setParameter("previousModel", glm_to_filament(proxy.previous_model));
        proxy.material->setParameter("currentClipFromUserWorld", glm_to_filament(current_clip_from_user_world));
        proxy.material->setParameter("previousClipFromUserWorld", glm_to_filament(state.previous_clip));
        proxy.material->setParameter("previousCamera", vector_parameter(state.previous_camera));
        proxy.material->setParameter("previousRight", vector_parameter(state.previous_right));
        proxy.material->setParameter("previousUp", vector_parameter(state.previous_up));
        proxy.material->setParameter("previousWorldToUser", matrix_parameter(state.previous_world_to_user));
        proxy.material->setParameter("previousTime", state.previous_time);
        proxy.material->setParameter("historyValid", valid);
        ++state.coverage.draws;
        if (!valid) ++state.coverage.new_or_changed_draws;
    }
    for (auto entry = state.proxies.begin(); entry != state.proxies.end();) {
        if (entry->second.present) ++entry;
        else { state.destroy_proxy(entry->second); entry = state.proxies.erase(entry); }
    }
    state.prepared = state.coverage.unavailable_draws == 0;
    return state.prepared;
}

void TemporalEffects::commit_accepted() {
    auto& state = *impl_;
    if (!state.prepared) return;
    for (auto& [identity, proxy] : state.proxies) {
        proxy.previous_model = proxy.current_model;
        proxy.previous_signature = proxy.current_signature;
        proxy.previous_visible = proxy.visible;
    }
    state.previous_clip = state.current_clip;
    state.previous_camera = state.current_camera;
    state.previous_right = state.current_right;
    state.previous_up = state.current_up;
    state.previous_world_to_user = state.current_world_to_user;
    state.previous_time = state.current_time;
    state.history_valid = true;
    state.prepared = false;
}

void TemporalEffects::invalidate_history() { impl_->invalidate(); }
filament::View* TemporalEffects::view() const noexcept { return impl_->view; }
filament::Texture* TemporalEffects::packed_texture() const noexcept { return impl_->packed; }
const TemporalEffectCoverage& TemporalEffects::coverage() const noexcept { return impl_->coverage; }

} // namespace heaven
