// Native contributor oracle: real production effect packages, no SDK or assets.
#include "rhi/frame_generation/temporal_effects.h"
#include "rhi/frame_generation/temporal_camera.h"
#include "rhi/frame_generation/frame_generation_output.h"
#include "rhi/features/particle_feature.h"
#include "rhi/filament_util.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "rhi/render_test_scene_assets.h"
#include "engine/weather_shader_ref.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <utils/EntityManager.h>
#include <backend/PixelBufferDescriptor.h>
#include <math/half.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace heaven {
namespace {
constexpr uint32_t kExtent = 192;
using Pixels = std::vector<glm::vec4>;
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::array<size_t, 18> owners(filament::Engine& engine) {
    return {engine.getBufferObjectCount(), engine.getViewCount(), engine.getSceneCount(),
        engine.getSwapChainCount(), engine.getIndexBufferCount(), engine.getSkinningBufferCount(),
        engine.getMorphTargetBufferCount(), engine.getInstanceBufferCount(), engine.getVertexBufferCount(),
        engine.getIndirectLightCount(), engine.getMaterialCount(), engine.getTextureCount(),
        engine.getSkyboxeCount(), engine.getColorGradingCount(), engine.getRenderTargetCount(),
        engine.getRenderableManager().getComponentCount(), engine.getTransformManager().getComponentCount(),
        engine.getLightManager().getComponentCount()};
}

struct Source {
    utils::Entity entity;
    filament::VertexBuffer* vertices = nullptr;
    filament::IndexBuffer* indices = nullptr;
    filament::Material* material = nullptr;
    filament::MaterialInstance* instance = nullptr;
    TemporalEffectKind kind = TemporalEffectKind::Flame;
    std::array<TemporalEffectTexture, 3> textures{};
    std::vector<filament::Texture*> owned_textures;
};

class EffectsFixture {
public:
    explicit EffectsFixture(filament::Engine& engine) : engine_(engine) {}
    ~EffectsFixture() {
        engine_.flushAndWait();
        output_.reset();
        effects_.reset();
        if (blocker_) engine_.destroy(blocker_);
        for (auto& source : sources_) {
            engine_.destroy(source.entity);
            utils::EntityManager::get().destroy(source.entity);
            engine_.destroy(source.instance);
            engine_.destroy(source.material);
            engine_.destroy(source.vertices);
            engine_.destroy(source.indices);
            for (auto* texture : source.owned_textures) engine_.destroy(texture);
        }
        if (view_) { view_->setScene(nullptr); view_->setCamera(nullptr); engine_.destroy(view_); }
        if (scene_) engine_.destroy(scene_);
        if (camera_) engine_.destroyCameraComponent(camera_entity_);
        if (camera_entity_) utils::EntityManager::get().destroy(camera_entity_);
        if (renderer_) engine_.destroy(renderer_);
        if (swapchain_) engine_.destroy(swapchain_);
        engine_.flushAndWait();
    }
    void init() {
        renderer_ = engine_.createRenderer();
        scene_ = engine_.createScene();
        view_ = engine_.createView();
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        require(renderer_ && scene_ && view_ && camera_, "Effect fixture core unavailable");
        renderer_->setDisplayInfo({.refreshRate = 0.0f});
        renderer_->setFrameRateOptions({.interval = 0});
        camera.position = {0, -6, .8f}; camera.yaw = 90; camera.pitch = 0; camera.fov = 60;
        update_camera();
        view_->setScene(scene_); view_->setCamera(camera_);
        view_->setPostProcessingEnabled(false); view_->setShadowingEnabled(false);
        swapchain_ = engine_.createSwapChain(kExtent, kExtent, filament::SwapChain::CONFIG_READABLE);
        require(swapchain_, "Effect fixture headless swapchain unavailable");
        effects_ = std::make_unique<TemporalEffects>(core());
        require(effects_->ensure(kExtent, kExtent), "Effect target allocation failed");
    }
    BackendCore core() const { return {&engine_, scene_, view_, renderer_, camera_}; }
    void update_camera() {
        camera_->setProjection(camera.fov, 1.0, .5, 2000, filament::Camera::Fov::VERTICAL);
        camera_->setModelMatrix(glm_to_filament(glm::inverse(camera.view_matrix())));
    }
    glm::mat4 clip() const {
        return temporal_gl_projection(camera_->getProjectionMatrix()) * camera.view_matrix();
    }
    size_t add(TemporalEffectKind kind, glm::vec3 translation = {}) {
        Source source;
        source.kind = kind;
        const uint8_t* package = nullptr; size_t package_size = 0;
        switch (kind) {
            case TemporalEffectKind::Smoke: package=k_heaven_smoke_filamat; package_size=k_heaven_smoke_filamat_size; break;
            case TemporalEffectKind::Flame: package=k_heaven_flame_filamat; package_size=k_heaven_flame_filamat_size; break;
            case TemporalEffectKind::Glow: package=k_heaven_glow_filamat; package_size=k_heaven_glow_filamat_size; break;
            case TemporalEffectKind::Firefly: package=k_heaven_firefly_filamat; package_size=k_heaven_firefly_filamat_size; break;
            case TemporalEffectKind::Rain: package=k_heaven_rain_filamat; package_size=k_heaven_rain_filamat_size; break;
            case TemporalEffectKind::LensFlare: package=k_heaven_lens_flare_filamat; package_size=k_heaven_lens_flare_filamat_size; break;
            case TemporalEffectKind::CloudPlane: package=k_heaven_clouds_filamat; package_size=k_heaven_clouds_filamat_size; break;
            case TemporalEffectKind::VolumetricCloud: package=k_heaven_volumetric_clouds_filamat; package_size=k_heaven_volumetric_clouds_filamat_size; break;
        }
        source.material = filament::Material::Builder().package(package, package_size).build(engine_);
        require(source.material, "Visible effect source package unavailable");
        source.instance = source.material->createInstance();
        require(source.instance, "Visible effect source instance unavailable");
        if (kind == TemporalEffectKind::Flame) {
            std::vector<filament::Material::ParameterInfo> parameters(source.material->getParameterCount());
            source.material->getParameters(parameters.data(), parameters.size());
            require(std::ranges::any_of(parameters, [](const auto& parameter) {
                return !parameter.isSampler && !parameter.isSubpass &&
                    parameter.type == filament::Material::ParameterType::BOOL &&
                    std::string_view(parameter.name) == "_doubleSided";
            }), "Flame fixture must exercise Filament's injected double-sided BOOL");
        }
        switch (kind) {
            case TemporalEffectKind::Smoke: material_tuning::bind_smoke(*source.instance); break;
            case TemporalEffectKind::Flame: material_tuning::bind_flame(*source.instance); break;
            case TemporalEffectKind::Glow: material_tuning::bind_glow(*source.instance); break;
            case TemporalEffectKind::Firefly: material_tuning::bind_firefly(*source.instance); break;
            case TemporalEffectKind::Rain: material_tuning::bind_rain(*source.instance); break;
            case TemporalEffectKind::LensFlare: material_tuning::bind_lens_flare(*source.instance); break;
            case TemporalEffectKind::CloudPlane: material_tuning::bind_clouds(*source.instance); break;
            case TemporalEffectKind::VolumetricCloud: material_tuning::bind_volumetric_clouds(*source.instance); break;
        }
        if (source.material->hasParameter("time")) source.instance->setParameter("time", .5f);
        if (source.material->hasParameter("tint")) source.instance->setParameter("tint", filament::math::float3{1,1,1});
        if (source.material->hasParameter("alphaScale")) source.instance->setParameter("alphaScale", 1.0f);
        if (source.material->hasParameter("spawnInterval")) source.instance->setParameter("spawnInterval", .1f);
        const auto texture = [&](size_t index, const char* name, glm::vec4 pixel) {
            auto* value = filament::Texture::Builder().width(1).height(1).levels(1)
                .format(filament::Texture::InternalFormat::RGBA32F).build(engine_);
            require(value, "Controlled effect sampler unavailable");
            value->setImage(engine_,0,filament::Texture::PixelBufferDescriptor::make(
                &pixel,sizeof(pixel),filament::Texture::Format::RGBA,filament::Texture::Type::FLOAT,
                [](const void*,size_t) {}));
            // setImage borrows input until its callback: finish before pixel dies.
            engine_.flushAndWait();
            const filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR,
                filament::TextureSampler::MagFilter::LINEAR,filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
            source.instance->setParameter(name,value,sampler);
            source.textures[index]={name,value,sampler}; source.owned_textures.push_back(value);
        };
        if (kind == TemporalEffectKind::Flame) {
            source.instance->setParameter("seed", .13f);
            source.instance->setParameter("particleShapeTuning",filament::math::float4{1,2,.2f,1});
        } else if (kind == TemporalEffectKind::Glow || kind == TemporalEffectKind::Firefly) {
            texture(0,"glowTex",glm::vec4(1));
            if (kind == TemporalEffectKind::Firefly) {
                source.instance->setParameter("particleMotionTuning",filament::math::float4{.6f,0,.2f,4});
                source.instance->setParameter("alphaScale",4.0f);
            }
        } else if (kind == TemporalEffectKind::LensFlare) {
            texture(0,"flareTex",glm::vec4(1)); texture(1,"depthTexture",glm::vec4(2000));
            source.instance->setParameter("fadeFar",20.0f); source.instance->setParameter("fadeNear",10.0f);
            source.instance->setParameter("maxRadius",3.0f); source.instance->setParameter("brightness",1.0f);
        } else if (kind == TemporalEffectKind::Rain) {
            source.instance->setParameter("activeStreaks",1); source.instance->setParameter("rainIntensity",1.0f);
            source.instance->setParameter("volumeExtent",4.0f); source.instance->setParameter("fallSpeed",1.0f);
            source.instance->setParameter("streakLenMin",1.5f); source.instance->setParameter("streakLenMax",1.5f);
            source.instance->setParameter("rainShaderTuning",filament::math::float4{.3f,2,.52f,.68f});
        } else if (kind == TemporalEffectKind::CloudPlane || kind == TemporalEffectKind::VolumetricCloud) {
            source.instance->setParameter("sunDir",filament::math::float3{0,0,1});
            source.instance->setParameter("sunColor",filament::math::float3{1,1,1});
            source.instance->setParameter("skyColor",filament::math::float3{1,1,1});
            if (kind == TemporalEffectKind::CloudPlane) {
                texture(0,"cloudTex1",glm::vec4(1)); texture(1,"cloudTex2",glm::vec4(1)); texture(2,"cloudMask",glm::vec4(1));
                source.instance->setParameter("cloudHeight",0.0f);
                source.instance->setParameter("cloudMotionTuning",filament::math::float4{0,0,.1f,.8f});
            } else {
                texture(0,"depthTexture",glm::vec4(2000));
                source.instance->setParameter("cloudBottom",0.0f); source.instance->setParameter("cloudTop",3.0f);
                source.instance->setParameter("coverage",1.0f); source.instance->setParameter("densityScale",4.0f);
                source.instance->setParameter("noiseScale",.1f); source.instance->setParameter("maxMarchDistance",30.0f);
                source.instance->setParameter("numSteps",32); source.instance->setParameter("temporalJitter",.5f);
                source.instance->setParameter("cloudMotionTuning",filament::math::float4{0,1,0,0});
            }
        }
        struct Vertex { glm::vec3 position; glm::vec2 uv; glm::vec4 custom0, custom1, tangent; };
        auto data = std::make_shared<std::array<Vertex, 4>>();
        constexpr std::array corners{glm::vec2{0, 0}, glm::vec2{1, 0}, glm::vec2{1, 1}, glm::vec2{0, 1}};
        for (size_t i = 0; i < data->size(); ++i)
            (*data)[i] = {{0, 0, 0}, corners[i], {0, 1, 0, 0}, {.8f, 0, 0, 0}, {0, 0, 0, 1}};
        if (kind == TemporalEffectKind::Rain)
            for (auto& vertex : *data) vertex.custom0 = glm::vec4(0);
        if (kind == TemporalEffectKind::CloudPlane || kind == TemporalEffectKind::VolumetricCloud)
            for (auto& vertex : *data) vertex.position = {vertex.uv.x*2-1, vertex.uv.y*2-1, 0};
        using VB = filament::VertexBuffer;
        source.vertices = VB::Builder().vertexCount(4).bufferCount(1)
            .attribute(filament::VertexAttribute::POSITION, 0, VB::AttributeType::FLOAT3, offsetof(Vertex, position), sizeof(Vertex))
            .attribute(filament::VertexAttribute::UV0, 0, VB::AttributeType::FLOAT2, offsetof(Vertex, uv), sizeof(Vertex))
            .attribute(filament::VertexAttribute::CUSTOM0, 0, VB::AttributeType::FLOAT4, offsetof(Vertex, custom0), sizeof(Vertex))
            .attribute(filament::VertexAttribute::CUSTOM1, 0, VB::AttributeType::FLOAT4, offsetof(Vertex, custom1), sizeof(Vertex))
            .attribute(filament::VertexAttribute::TANGENTS, 0, VB::AttributeType::FLOAT4, offsetof(Vertex, tangent), sizeof(Vertex)).build(engine_);
        require(source.vertices, "Effect fixture vertices unavailable");
        source.vertices->setBufferAt(engine_, 0, filament::VertexBuffer::BufferDescriptor::make(
            data->data(), sizeof(*data), [data](const void*, size_t) {}));
        constexpr static std::array<uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
        source.indices = filament::IndexBuffer::Builder().indexCount(6)
            .bufferType(filament::IndexBuffer::IndexType::UINT).build(engine_);
        require(source.indices, "Effect fixture indices unavailable");
        source.indices->setBuffer(engine_, {indices.data(), sizeof(indices)});
        source.entity = utils::EntityManager::get().create();
        const auto result = filament::RenderableManager::Builder(1)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, source.vertices, source.indices)
            .material(0, source.instance).boundingBox({{0, 0, 0}, {4, 4, 4}}).culling(false)
            .castShadows(false).receiveShadows(false).build(engine_, source.entity);
        require(result == filament::RenderableManager::Builder::Success, "Visible effect source renderable unavailable");
        engine_.getTransformManager().create(source.entity);
        sources_.push_back(source);
        set_translation(sources_.size() - 1, translation);
        return sources_.size() - 1;
    }
    void set_translation(size_t index, glm::vec3 translation) {
        auto& transforms = engine_.getTransformManager();
        transforms.setTransform(transforms.getInstance(sources_.at(index).entity),
            glm_to_filament(glm::translate(glm::mat4(1), translation)));
    }
    filament::MaterialInstance& parameters(size_t index) { return *sources_.at(index).instance; }
    void set_model(size_t index, const glm::mat4& matrix) {
        auto& transforms = engine_.getTransformManager();
        transforms.setTransform(transforms.getInstance(sources_.at(index).entity),glm_to_filament(matrix));
    }
    void alpha(size_t index, float alpha) { sources_.at(index).instance->setParameter("alphaScale", alpha); }
    std::vector<TemporalEffectDraw> draws(size_t count = SIZE_MAX) {
        std::vector<TemporalEffectDraw> result;
        for (size_t i = 0; i < std::min(count, sources_.size()); ++i) {
            const auto& source = sources_[i];
            result.push_back({.identity = source.entity.getId(), .kind = source.kind,
                .source_entity = source.entity, .vertices = source.vertices,
                .indices = source.indices, .source = source.instance, .textures = source.textures});
        }
        return result;
    }
    void prepare(float time, size_t count = SIZE_MAX) {
        for (auto& source : sources_) {
            if (source.material->hasParameter("time")) source.instance->setParameter("time", time);
            if (source.material->hasParameter("cameraPos")) source.instance->setParameter("cameraPos",
                filament::math::float3{camera.position.x,camera.position.y,camera.position.z});
        }
        update_camera();
        const auto descriptors = draws(count);
        require(effects_->prepare(descriptors, camera, time, clip()), "Effect preparation incomplete");
        require(effects_->coverage().unavailable_draws == 0 && effects_->coverage().draws == descriptors.size(),
            "Effect coverage counters differ from actual descriptors");
    }
    Pixels capture(float time, size_t count = SIZE_MAX) {
        prepare(time, count);
        return render(effects_->view(), effects_->packed_texture());
    }
    Pixels render(filament::View* view, filament::Texture* texture) {
        auto* read = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, texture).build(engine_);
        require(read, "Effect readback target unavailable");
        bool accepted = false;
        for (int attempt = 0; attempt < 32 && !accepted; ++attempt) {
            accepted = renderer_->beginFrame(swapchain_);
            if (!accepted) engine_.flushAndWait();
        }
        require(accepted, "Effect fixture rejected all bounded submissions");
        renderer_->render(view);
        Pixels pixels(size_t(texture->getWidth()) * texture->getHeight(), glm::vec4(999));
        std::atomic_bool done{false};
        renderer_->readPixels(read, 0, 0, texture->getWidth(), texture->getHeight(),
            filament::backend::PixelBufferDescriptor(pixels.data(), pixels.size() * sizeof(glm::vec4),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::FLOAT,
                [](void*, size_t, void* user) { *static_cast<std::atomic_bool*>(user) = true; }, &done));
        renderer_->endFrame();
        engine_.flushAndWait();
        engine_.destroy(read);
        require(done, "Effect readback callback incomplete after drain");
        for (const auto& pixel : pixels)
            for (int channel = 0; channel < 4; ++channel)
                require(std::isfinite(pixel[channel]), "Nonfinite effect contribution");
        return pixels;
    }
    void verify_opaque_blocker(float time) {
        if (!output_) output_ = std::make_unique<FrameGenerationOutput>(core());
        require(output_->ensure(kExtent, kExtent), "Opaque blocker output allocation failed");
        if (!blocker_) blocker_ = filament::Texture::Builder().width(1).height(1).levels(1)
            .format(filament::Texture::InternalFormat::RGBA32F).build(engine_);
        require(blocker_, "Opaque blocker packed input unavailable");
        // Independent plane at y=-1 lies in front of both effect planes. Its
        // nonzero marker distinguishes preserving scene motion from zeroing it.
        const auto plane_clip = clip() * glm::vec4(0, -1, .8f, 1);
        const float expected_depth = .5f - .5f * plane_clip.z / plane_clip.w;
        constexpr glm::vec2 marker{.03125f, -.015625f};
        const glm::vec4 packed{marker, expected_depth, 1};
        blocker_->setImage(engine_, 0, {&packed, sizeof(packed), filament::Texture::Format::RGBA,
            filament::Texture::Type::FLOAT});
        engine_.flushAndWait();
        prepare(time);
        output_->prepare(blocker_, kExtent, kExtent, false,
            {.current_clip_from_world = clip(), .previous_clip_from_world = clip(), .valid = true},
            effects_->packed_texture());
        auto* depth_read = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, output_->depth_texture()).build(engine_);
        auto* motion_read = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, output_->motion_texture()).build(engine_);
        require(depth_read && motion_read, "Opaque blocker readback target unavailable");
        bool accepted = false;
        for (int attempt = 0; attempt < 32 && !accepted; ++attempt) {
            accepted = renderer_->beginFrame(swapchain_);
            if (!accepted) engine_.flushAndWait();
        }
        require(accepted, "Opaque blocker rejected all bounded submissions");
        renderer_->render(effects_->view());
        output_->render_extract();
        std::vector<float> depths(size_t(kExtent) * kExtent, -1);
        std::vector<std::array<uint16_t, 2>> motion(size_t(kExtent) * kExtent);
        std::array<std::atomic_bool, 2> done{};
        const auto callback = [](void*, size_t, void* user) { *static_cast<std::atomic_bool*>(user) = true; };
        renderer_->readPixels(depth_read, 0, 0, kExtent, kExtent,
            filament::backend::PixelBufferDescriptor(depths.data(), depths.size() * sizeof(float),
                filament::backend::PixelDataFormat::R, filament::backend::PixelDataType::FLOAT, callback, done.data()));
        renderer_->readPixels(motion_read, 0, 0, kExtent, kExtent,
            filament::backend::PixelBufferDescriptor(motion.data(), motion.size() * sizeof(motion[0]),
                filament::backend::PixelDataFormat::RG, filament::backend::PixelDataType::HALF, callback, &done[1]));
        renderer_->endFrame(); engine_.flushAndWait();
        engine_.destroy(depth_read); engine_.destroy(motion_read);
        require(done[0] && done[1], "Opaque blocker readback callbacks incomplete");
        for (size_t index = 0; index < depths.size(); ++index) {
            require(std::abs(depths[index] - expected_depth) < 1e-6f,
                "Effect behind opaque scene replaced the blocker depth");
            require(float(filament::math::makeHalf(motion[index][0])) == marker.x &&
                    float(filament::math::makeHalf(motion[index][1])) == marker.y,
                "Effect behind opaque scene replaced the source-specific scene motion");
        }
        output_.reset(); // Detach effect samplers before later effect-target resize.
        std::puts("[temporal_effects_test] opaque blocker retained independent scene depth/motion marker PASS");
    }
    void accept() { effects_->commit_accepted(); }
    void reset() { effects_->invalidate_history(); }
    void resize(int size) { engine_.flushAndWait(); require(effects_->ensure(size, size), "Effect resize failed"); }
    Camera camera;
    filament::Engine& engine() { return engine_; }
    filament::Renderer* renderer() { return renderer_; }
    filament::SwapChain* swapchain() { return swapchain_; }
private:
    filament::Engine& engine_;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Camera* camera_ = nullptr;
    utils::Entity camera_entity_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    std::vector<Source> sources_;
    std::unique_ptr<TemporalEffects> effects_;
    std::unique_ptr<FrameGenerationOutput> output_;
    filament::Texture* blocker_ = nullptr;
};

size_t verify_motion(const Pixels& pixels, const Camera& camera, const glm::mat4& current_clip,
        const glm::mat4& previous_clip, glm::vec3 current_translation, glm::vec3 previous_translation,
        bool valid, bool wrong_previous) {
    const auto inverse = glm::inverse(glm::dmat4(current_clip));
    size_t measured = 0;
    double maximum = 0;
    for (size_t index = 0; index < pixels.size(); ++index) {
        const auto& pixel = pixels[index];
        if (std::abs(pixel.a) <= .004f) continue;
        ++measured;
        require(valid ? pixel.a > 0 : pixel.a < 0, "Effect signed correspondence differs from accepted history");
        const glm::dvec2 uv{(double(index % kExtent) + .5) / kExtent,
            (double(index / kExtent) + .5) / kExtent};
        const auto far_clip = inverse * glm::dvec4(2 * uv.x - 1, 1 - 2 * uv.y, .5, 1);
        const auto direction = glm::dvec3(far_clip) / far_clip.w - glm::dvec3(camera.position);
        const double distance = (double(current_translation.y) - camera.position.y) / direction.y;
        const auto current = glm::dvec3(camera.position) + direction * distance;
        const auto previous = current - glm::dvec3(current_translation) + glm::dvec3(previous_translation);
        const auto pc = glm::dmat4(previous_clip) * glm::dvec4(previous, 1);
        const auto cc = glm::dmat4(current_clip) * glm::dvec4(current, 1);
        const glm::dvec2 previous_uv = glm::dvec2(pc) / pc.w * glm::dvec2(.5, -.5) + .5;
        const auto expected_motion = valid ? previous_uv - uv : glm::dvec2(0);
        const double depth = .5 - .5 * cc.z / cc.w;
        maximum = std::max({maximum, glm::length(glm::dvec2(pixel) - expected_motion), std::abs(double(pixel.b) - depth)});
    }
    std::printf("[temporal_effects_test] analytic planar samples=%zu max_error=%.9g valid=%d\n", measured, maximum, valid);
    require(measured >= 48, "Effect oracle has insufficient actual nonzero coverage");
    require(maximum <= .00005, wrong_previous ? "Injected wrong-prior motion rejected" : "Effect geometry motion/depth mismatch");
    return measured;
}

void run_contributors(filament::Engine& engine, bool wrong_previous, bool rejected_history) {
    EffectsFixture fixture(engine); fixture.init();
    fixture.add(TemporalEffectKind::Flame, {});
    fixture.add(TemporalEffectKind::Flame, {0, 1, 0});
    const auto previous_clip = fixture.clip();
    // This oracle describes only the near plane; rear-only pixels have a
    // different correct depth and are covered by the isolated rear oracle.
    fixture.alpha(1, 0);
    const auto first = fixture.capture(.5f);
    verify_motion(first, fixture.camera, previous_clip, previous_clip, {}, {}, false, false);
    fixture.alpha(1, 1);
    fixture.accept();
    const glm::vec3 near_current{.08f, 0, .03f}, far_current{-.09f, 1, -.02f};
    fixture.set_translation(0, near_current); fixture.set_translation(1, far_current);
    fixture.alpha(1, 0); const auto near = fixture.capture(.6f);
    verify_motion(near, fixture.camera, fixture.clip(), previous_clip, near_current,
        wrong_previous ? glm::vec3{.4f, 0, 0} : glm::vec3{}, true, wrong_previous);
    fixture.alpha(0, 0); fixture.alpha(1, 1); const auto far = fixture.capture(.6f);
    verify_motion(far, fixture.camera, fixture.clip(), previous_clip, far_current, {0, 1, 0}, true, false);
    fixture.alpha(0, .1f); fixture.alpha(1, 1); const auto thin_front = fixture.capture(.6f);
    for (size_t index = 0; index < far.size(); ++index)
        require(glm::length(thin_front[index] - far[index]) < .00002f,
            "Thin foreground depth write hid a dominant rear contributor");
    fixture.alpha(0, 1); fixture.alpha(1, .1f); const auto thin_rear = fixture.capture(.6f);
    for (size_t index = 0; index < near.size(); ++index)
        require(glm::length(thin_rear[index] - near[index]) < .00002f,
            "Thin rear contributor replaced a dominant foreground layer");
    fixture.alpha(1, 1); const auto combined = fixture.capture(.6f);
    size_t overlap = 0, separated = 0;
    for (size_t index = 0; index < combined.size(); ++index) {
        const bool has_near = std::abs(near[index].a) > .004f;
        const bool has_far = std::abs(far[index].a) > .004f;
        const auto expected = has_near ? near[index] : far[index];
        if (has_near && has_far) {
            ++overlap;
            if (glm::length(glm::vec2(near[index] - far[index])) > .003f) ++separated;
        }
        require(glm::length(combined[index] - expected) < .00002f,
            "Overlap resolve mixed signed coverage/vectors instead of nearest actual contributor");
    }
    require(overlap >= 32 && separated >= 32, "Overlap does not expose distinct layer motion");
    fixture.verify_opaque_blocker(.6f);
    fixture.accept();
    const auto accepted_clip = fixture.clip();
    fixture.set_translation(0, {.5f, 0, .2f}); fixture.prepare(.7f); // Rejected; no accepted history advance.
    if (rejected_history) fixture.accept(); // Source-aware injected integration fault.
    fixture.set_translation(0, {.12f, 0, .04f}); fixture.alpha(1, 0);
    const auto after_rejection = fixture.capture(.8f);
    verify_motion(after_rejection, fixture.camera, fixture.clip(), accepted_clip,
        {.12f, 0, .04f}, near_current, true, false);
    fixture.alpha(1, 1); fixture.add(TemporalEffectKind::Flame, {0, -.5f, 0});
    fixture.alpha(0, 0); fixture.alpha(1, 0); const auto new_only = fixture.capture(.8f);
    fixture.alpha(0, 1); fixture.alpha(1, 1); const auto new_overlap = fixture.capture(.8f);
    size_t new_winners = 0;
    for (size_t index = 0; index < new_only.size(); ++index) if (new_only[index].a < -.004f) {
        ++new_winners;
        require(glm::length(new_overlap[index] - new_only[index]) < .00002f,
            "New foreground layer failed explicit negative-validity nearest resolve");
    }
    require(new_winners >= 48, "New overlapping layer has insufficient coverage");
    fixture.reset(); const auto reset = fixture.capture(.8f);
    for (const auto& pixel : reset) if (std::abs(pixel.a) > .004f)
        require(pixel.a < 0 && glm::length(glm::vec2(pixel)) < 1e-7f, "Reset kept effect history");
    const auto before = owners(engine); fixture.resize(kExtent);
    require(owners(engine) == before, "Same-size effect ensure allocated new native owners");
    fixture.resize(128); fixture.resize(kExtent);
    const auto resized = fixture.capture(.8f);
    for (const auto& pixel : resized) if (std::abs(pixel.a) > .004f) require(pixel.a < 0, "Resize retained old effect correspondence");
    std::printf("[temporal_effects_test] overlap=%zu distinct_motion=%zu new_winners=%zu rejected/reset/resize PASS\n", overlap, separated, new_winners);
}

void run_recycled_smoke(filament::Engine& engine) {
    EffectsFixture fixture(engine); fixture.init(); fixture.add(TemporalEffectKind::Smoke);
    fixture.capture(.25f); fixture.accept();
    const auto within_cycle = fixture.capture(.3f);
    size_t positive = 0;
    for (const auto& pixel : within_cycle) if (pixel.a > .004f) ++positive;
    require(positive >= 48, "Smoke same-particle accepted correspondence unavailable");
    const auto recycled = fixture.capture(1.15f);
    size_t negative = 0;
    for (const auto& pixel : recycled) if (std::abs(pixel.a) > .004f) {
        ++negative;
        require(pixel.a < 0 && glm::length(glm::vec2(pixel)) < 1e-7f, "Recycled smoke slot inherited unrelated prior particle motion");
    }
    require(negative >= 48, "Recycled smoke control has insufficient visible coverage");
    std::printf("[temporal_effects_test] smoke_same_cycle=%zu recycled_slot=%zu PASS\n", positive, negative);
}

using Quad = std::array<glm::dvec3,4>;
Quad billboard(glm::dvec3 center, glm::dvec3 right, glm::dvec3 up) {
    return {center-right-up,center+right-up,center+right+up,center-right+up};
}

void verify_quad(const char* label, const Pixels& pixels, const Quad& current, const Quad& previous,
        const glm::mat4& current_projection, const glm::mat4& previous_projection, bool valid,
        double tolerance = .00005) {
    constexpr std::array<std::array<size_t,3>,2> triangles{{{0,1,2},{0,2,3}}};
    std::array<glm::dvec4,4> clips{}, prior{};
    std::array<glm::dvec2,4> screen{};
    for(size_t i=0;i<4;++i) {
        clips[i]=glm::dmat4(current_projection)*glm::dvec4(current[i],1);
        prior[i]=glm::dmat4(previous_projection)*glm::dvec4(previous[i],1);
        require(clips[i].w>0 && prior[i].w>0,"Independent effect quad behind camera");
        screen[i]=glm::dvec2(clips[i])/clips[i].w*glm::dvec2(.5,-.5)+.5;
    }
    const auto cross=[](glm::dvec2 a,glm::dvec2 b){return a.x*b.y-a.y*b.x;};
    size_t count=0, worst_index=0; double maximum=0;
    glm::dvec3 worst_expected{}, worst_world{};
    for(size_t index=0;index<pixels.size();++index) {
        const auto& pixel=pixels[index]; if(std::abs(pixel.a)<.5f) continue;
        ++count;
        require(valid?pixel.a>0:pixel.a<0,"Effect quad signed history mismatch");
        const glm::dvec2 uv{(double(index%kExtent)+.5)/kExtent,(double(index/kExtent)+.5)/kExtent};
        bool found=false;
        for(const auto& triangle:triangles) {
            const auto a=screen[triangle[0]],b=screen[triangle[1]],c=screen[triangle[2]];
            const double area=cross(b-a,c-a);
            if(std::abs(area)<1e-10) continue;
            const std::array weights{cross(b-uv,c-uv)/area,cross(c-uv,a-uv)/area,cross(a-uv,b-uv)/area};
            if(*std::min_element(weights.begin(),weights.end())<-.002) continue;
            glm::dvec4 cc{},pc{},world{};
            for(size_t corner=0;corner<3;++corner) {
                const auto vertex=triangle[corner]; const double w=weights[corner]/clips[vertex].w;
                cc+=clips[vertex]*w; pc+=prior[vertex]*w;
                world+=glm::dvec4(current[vertex],1)*w;
            }
            const glm::dvec2 motion=valid?glm::dvec2(pc)/pc.w*glm::dvec2(.5,-.5)+.5-uv:glm::dvec2(0);
            const double depth=.5-.5*cc.z/cc.w;
            const double error=std::max(glm::length(glm::dvec2(pixel)-motion),std::abs(double(pixel.b)-depth));
            if(error>maximum) {
                maximum=error; worst_index=index;
                worst_expected=glm::dvec3(motion,depth); worst_world=glm::dvec3(world)/world.w;
            }
            found=true; break;
        }
        require(found,"Actual effect pixel lies outside its independent projected quad");
    }
    std::printf("[temporal_effects_test] %s actual_pixels=%zu independent_quad_error=%.9g valid=%d\n",label,count,maximum,valid);
    const auto& worst=pixels[worst_index];
    std::printf("[temporal_effects_test] %s worst_pixel=(%zu,%zu) actual=(%.9g,%.9g,%.9g) expected=(%.9g,%.9g,%.9g) world=(%.9g,%.9g,%.9g)\n",
        label,worst_index%kExtent,worst_index/kExtent,double(worst.r),double(worst.g),double(worst.b),
        worst_expected.x,worst_expected.y,worst_expected.z,worst_world.x,worst_world.y,worst_world.z);
    require(count>=32,"Effect package has insufficient actual dominant pixel coverage");
    require(maximum<=tolerance,"Effect package motion/depth differs from independent projected quad");
}

void require_absent(const Pixels& pixels,const char* label) {
    for(const auto& pixel:pixels) require(glm::length(pixel)<1e-7f,label);
}

float particle_hash(float x) {
    x=glm::fract(x*.1031f); x*=x+33.33f; x*=x+x; return glm::fract(x);
}
Quad glow_quad(glm::vec3 translation) {
    // Slot zero has fixed authored XY jitter, even with zero rise/growth.
    const glm::dvec3 center=glm::dvec3(translation)+glm::dvec3(-.025,(double(particle_hash(4))-.5)*.05,0);
    return billboard(center,{-1,0,0},{0,0,1});
}
Quad firefly_quad(float time,glm::vec3 translation) {
    // Explicit controlled slot zero, radius1, orbit(.6,.6), height.2; no
    // shader helper or observed pixels supply these source positions.
    const double phase=double(particle_hash(9))*6.28318530718;
    const double t=double(time)*.6+phase;
    const glm::dvec3 center=glm::dvec3(translation)+glm::dvec3(std::cos(t)*.6,std::sin(t)*.6,.2*std::sin(t*.7+phase));
    return billboard(center,{-1,0,0},{0,0,1});
}

Quad flare_quad(const Camera& camera,glm::vec3 anchor) {
    const glm::dvec3 to_camera=glm::dvec3(camera.position)-glm::dvec3(anchor);
    const double distance=glm::length(to_camera),d=std::max(distance,.55);
    const double close=std::clamp((20-distance)/10,0.0,1.0),near=close*close;
    const double angular=.0045+(.070-.0045)*near;
    const double radius=std::min({angular*distance,3*(.45+(1.85-.45)*near),d*.85});
    const auto model=glm::inverse(glm::dmat4(camera.view_matrix()));
    const auto center=glm::dvec3(camera.position)-to_camera/distance*d;
    return billboard(center,glm::dvec3(model[0])*radius,glm::dvec3(model[1])*radius);
}

void run_billboards(filament::Engine& engine) {
    for(const auto kind:{TemporalEffectKind::Glow,TemporalEffectKind::Firefly,TemporalEffectKind::LensFlare}) {
        EffectsFixture fixture(engine); fixture.init(); fixture.add(kind);
        const bool fly=kind==TemporalEffectKind::Firefly,flare=kind==TemporalEffectKind::LensFlare;
        const auto previous_clip=fixture.clip();
        const auto prior=fly?firefly_quad(.25f,{}):flare?flare_quad(fixture.camera,{}):glow_quad({});
        auto first=fixture.capture(.25f);
        verify_quad(flare?"lens-flare-birth":fly?"firefly-birth":"glow-birth",first,prior,prior,fixture.clip(),previous_clip,false,fly?.0004:.00005);
        fixture.accept();
        const glm::vec3 translation{.1f,0,.05f}; fixture.set_translation(0,translation);
        const auto current=fly?firefly_quad(.3f,translation):flare?flare_quad(fixture.camera,translation):glow_quad(translation);
        auto next=fixture.capture(.3f);
        verify_quad(flare?"lens-flare-accepted":fly?"firefly-orbit":"glow-accepted",next,current,prior,fixture.clip(),previous_clip,true,fly?.0004:.00005);
        auto& parameters=fixture.parameters(0);
        if(flare) parameters.setParameter("brightness",0.0f); else parameters.setParameter("alphaScale",.01f);
        require_absent(fixture.capture(.3f),"Power-off/subdominant billboard left old temporal pixels");
        if(flare) {
            parameters.setParameter("brightness",1.0f);
            fixture.set_translation(0,{0,30,0});
            require_absent(fixture.capture(.3f),"Distant flare beyond fade range exported motion");
        } else {
            parameters.setParameter("alphaScale",fly?4.0f:1.0f);
            const float recycled_time=fly?1.55f:1.15f;
            const auto recycled=fixture.capture(recycled_time);
            const auto recycled_quad=fly?firefly_quad(recycled_time,translation):current;
            verify_quad(fly?"firefly-recycled-slot":"glow-recycled-slot",recycled,recycled_quad,prior,
                fixture.clip(),previous_clip,false,fly?.0004:.00005);
            if(!fly) require_absent(fixture.capture(.79f),"Glow lifetime fade retained dominant motion");
        }
    }
}

Quad rain_quad(const Camera& camera,float time) {
    constexpr glm::vec3 seed{}; constexpr float extent=4,fall_speed=1;
    const auto cell=weather_ref::streak_seed_cell(seed,camera.position,extent);
    const auto center=weather_ref::streak_world_position(seed,camera.position,extent,time,fall_speed);
    const float speed=weather_ref::drop_fall_speed(cell,extent,fall_speed);
    const glm::vec2 wind=glm::normalize(glm::vec2(1,.4f));
    const auto velocity=glm::normalize(glm::vec3(wind*(weather_ref::wind_strength(time)*speed),-speed));
    const auto right=glm::normalize(glm::cross(velocity,camera.position-center));
    return billboard(glm::dvec3(center),glm::dvec3(right)*(.3*(.7+.6*weather_ref::hash1(7.9f))),glm::dvec3(velocity)*.75);
}

void run_rain(filament::Engine& engine) {
    EffectsFixture fixture(engine); fixture.init(); fixture.camera.position.x=-3;
    fixture.update_camera(); fixture.add(TemporalEffectKind::Rain);
    constexpr glm::vec3 seed{};
    const auto cell=weather_ref::streak_seed_cell(seed,fixture.camera.position,4);
    const float period=weather_ref::drop_fall_period(cell,4,1);
    const float phase=weather_ref::drop_phase(cell,period);
    const float previous_time=phase+period*.5f,current_time=previous_time+.02f;
    const auto previous=rain_quad(fixture.camera,previous_time),current=rain_quad(fixture.camera,current_time);
    const auto previous_clip=fixture.clip();
    verify_quad("rain-birth",fixture.capture(previous_time),previous,previous,fixture.clip(),previous_clip,false,.0004);
    fixture.accept();
    verify_quad("rain-fall-and-wind",fixture.capture(current_time),current,previous,fixture.clip(),previous_clip,true,.0004);
    const float wrapped_time=previous_time+period;
    verify_quad("rain-fall-cycle-wrap",fixture.capture(wrapped_time),rain_quad(fixture.camera,wrapped_time),previous,
        fixture.clip(),previous_clip,false,.0004);
    fixture.parameters(0).setParameter("rainIntensity",.1f);
    require_absent(fixture.capture(current_time),"Thin rain became an opaque temporal blocker");
    fixture.parameters(0).setParameter("rainIntensity",0.0f);
    require_absent(fixture.capture(current_time),"Stopped rain left temporal pixels");
}

void verify_cloud(const char* label,const Pixels& pixels,const Camera& camera,const glm::mat4& current_clip,
        const glm::mat4& previous_clip,bool valid,bool plane,glm::dvec3 prior_advection={}) {
    const auto inverse=glm::inverse(glm::dmat4(current_clip));
    size_t count=0; double maximum=0,minimum_distance=1e30,maximum_distance=0;
    for(size_t index=0;index<pixels.size();++index) {
        const auto& pixel=pixels[index]; if(std::abs(pixel.a)<.5f) continue;
        ++count; require(valid?pixel.a>0:pixel.a<0,"Cloud accepted history sign mismatch");
        const glm::dvec2 uv{(double(index%kExtent)+.5)/kExtent,(double(index/kExtent)+.5)/kExtent};
        glm::dvec4 world=inverse*glm::dvec4(2*uv.x-1,1-2*uv.y,1-2*double(pixel.b),1);
        world/=world.w;
        if(plane) {
            auto ray=inverse*glm::dvec4(2*uv.x-1,1-2*uv.y,.5,1); ray/=ray.w;
            const auto direction=glm::dvec3(ray)-glm::dvec3(camera.position);
            const double t=-double(camera.position.z)/direction.z;
            const auto expected=glm::dvec3(camera.position)+direction*t;
            const auto projection=glm::dmat4(current_clip)*glm::dvec4(expected,1);
            maximum=std::max(maximum,std::abs(double(pixel.b)-(.5-.5*projection.z/projection.w)));
            require(std::abs(world.z)<.0001,"Cloud plane exported fullscreen-quad depth instead of its authored height");
        } else {
            const double distance=glm::length(glm::dvec3(world)-glm::dvec3(camera.position));
            minimum_distance=std::min(minimum_distance,distance); maximum_distance=std::max(maximum_distance,distance);
            require(world.z>=-.001 && world.z<=3.001 && distance<30.01 && distance>.1,
                "Cloud extinction centroid lies outside its actual marched slab/range");
        }
        const auto pc=glm::dmat4(previous_clip)*glm::dvec4(glm::dvec3(world)+prior_advection,1);
        const glm::dvec2 expected=valid?glm::dvec2(pc)/pc.w*glm::dvec2(.5,-.5)+.5-uv:glm::dvec2(0);
        maximum=std::max(maximum,glm::length(glm::dvec2(pixel)-expected));
    }
    std::printf("[temporal_effects_test] %s pixels=%zu geometric_error=%.9g centroid_distance=[%.7g,%.7g] valid=%d\n",
        label,count,maximum,minimum_distance,maximum_distance,valid);
    require(count>=64,"Cloud package has insufficient actual dominant coverage");
    require(maximum<=.00005,"Cloud representative depth/camera/advection geometry mismatch");
}

void run_clouds(filament::Engine& engine) {
    for(const bool plane:{true,false}) {
        EffectsFixture fixture(engine); fixture.init();
        fixture.add(plane?TemporalEffectKind::CloudPlane:TemporalEffectKind::VolumetricCloud);
        if(!plane) {
            const double half=100*std::tan(glm::radians(30.0));
            glm::mat4 model(1); model[0]=glm::vec4(-half,0,0,0); model[1]=glm::vec4(0,0,half,0);
            model[2]=glm::vec4(0,1,0,0); model[3]=glm::vec4(0,94,.8f,1); fixture.set_model(0,model);
        }
        const auto previous_clip=fixture.clip();
        verify_cloud(plane?"cloud-plane-birth":"volume-centroid-birth",fixture.capture(.25f),fixture.camera,
            fixture.clip(),previous_clip,false,plane);
        fixture.accept(); fixture.camera.position.x=.2f; fixture.update_camera();
        verify_cloud(plane?"cloud-plane-camera":"volume-centroid-camera",fixture.capture(.3f),fixture.camera,
            fixture.clip(),previous_clip,true,plane);
        if(plane) {
            fixture.parameters(0).setParameter("cloudMotionTuning",filament::math::float4{0,0,.1f,0});
            require_absent(fixture.capture(.3f),"Transparent cloud plane retained temporal depth");
        } else {
            fixture.parameters(0).setParameter("cloudMotionTuning",filament::math::float4{.02f,1,0,0});
            fixture.capture(.35f); fixture.accept(); const auto wind_clip=fixture.clip();
            verify_cloud("volume-detail-advection",fixture.capture(.4f),fixture.camera,fixture.clip(),wind_clip,
                true,false,{.01,0,0});
            fixture.parameters(0).setParameter("densityScale",0.0f);
            require_absent(fixture.capture(.4f),"Empty cloud volume retained centroid depth");
        }
    }
}

class FixtureResources final : public IResourceProvider {
public:
    FixtureResources(filament::Texture* white, filament::Texture* depth) : white_(white), depth_(depth) {}
    uint64_t load_texture([[maybe_unused]] const std::string& path, [[maybe_unused]] TextureSemantic semantic) override { return 0; }
    filament::Texture* get_filament_texture([[maybe_unused]] uint64_t handle) override { return white_; }
    filament::Texture* placeholder_texture() override { return white_; }
    filament::Texture* placeholder_normal_texture() override { return white_; }
    filament::Texture* placeholder_depth_texture() override { return depth_; }
private:
    filament::Texture* white_;
    filament::Texture* depth_;
};

void run_heat_field(filament::Engine& engine) {
    EffectsFixture fixture(engine); fixture.init();
    auto result = RenderTestSceneAssets::create(engine);
    require(result.has_value(), "Procedural feature inputs unavailable");
    auto assets = std::move(*result);
    auto* depth = filament::Texture::Builder().width(96).height(96).levels(1)
        .format(filament::Texture::InternalFormat::R32F).build(engine);
    require(depth, "Heat fixture depth placeholder unavailable");
    const auto destroy_depth = [&engine](filament::Texture* texture) { engine.destroy(texture); };
    std::unique_ptr<filament::Texture, decltype(destroy_depth)> owned_depth(depth, destroy_depth);
    std::vector<float> depth_pixels(size_t(96)*96,2000);
    depth->setImage(engine,0,{depth_pixels.data(),depth_pixels.size()*sizeof(float),
        filament::Texture::Format::R,filament::Texture::Type::FLOAT});
    engine.flushAndWait();
    FixtureResources resources(assets->feature_texture(FeatureAsset::Glow), depth);
    const auto core = fixture.core();
    RenderContext context{.engine = &engine, .scene = core.scene, .view = core.view,
        .renderer = core.renderer, .camera = core.camera, .resources = &resources, .test_scene_assets = assets.get()};
    ParticleFeature feature;
    AppConfig config; config.features.particles = true; config.features.heat_haze = true;
    feature.init(context, config);
    struct FeatureCleanup {
        ParticleFeature* feature;
        RenderContext& context;
        ~FeatureCleanup() { destroy(); }
        void destroy() {
            if (!feature) return;
            context.engine->flushAndWait();
            feature->destroy(context);
            feature = nullptr;
        }
    } cleanup{&feature, context};
    ParticleEmitter emitter; emitter.material_name = "fire_smoke"; emitter.floating_group = 7;
    emitter.life_mean = 1; emitter.life_spread = 0; emitter.spawn_rate = 1;
    emitter.radius_mean = .8f; emitter.radius_spread = 0; emitter.growth_mean = 0;
    emitter.velocity_mean = .2f; emitter.velocity_spread = 0;
    feature.set_emitters(std::span(&emitter, 1));
    const auto lighting = lighting_for_time_of_day(12);
    std::array<std::pair<int, float>, 1> offsets{{{7, 0}}};
    FrameContext frame{.camera = &fixture.camera, .lighting = &lighting, .time = .4f,
        .viewport_w = int(kExtent), .viewport_h = int(kExtent), .depth_texture = depth, .floating_offsets = offsets};
    feature.update(context, frame);
    const auto before_off = owners(engine);
    require(!feature.prepare_temporal_distortion(context, kExtent, kExtent) && !feature.temporal_distortion_texture(),
        "Off allocated a coherent distortion field");
    require(owners(engine) == before_off, "Off changed native resource counts");
    feature.set_temporal_distortion_enabled(true);
    require(feature.prepare_temporal_distortion(context, kExtent, kExtent), "Heat field prepare failed");
    auto* field = feature.temporal_distortion_texture(); require(field && field->getWidth() == 64 && field->getHeight() == 64, "Heat field grid is not bounded");
    auto capture_field = [&]() {
        auto* read = filament::RenderTarget::Builder().texture(filament::RenderTarget::AttachmentPoint::COLOR0, field).build(engine);
        require(read, "Heat field readback target unavailable");
        bool accepted = false;
        for (int retry = 0; retry < 32 && !accepted; ++retry) {
            accepted = core.renderer->beginFrame(fixture.swapchain());
            if (!accepted) engine.flushAndWait();
        }
        require(accepted, "Heat field rejected all bounded submissions");
        feature.render_temporal_distortion(context);
        Pixels pixels(size_t(64) * 64, glm::vec4(999)); std::atomic_bool done{false};
        core.renderer->readPixels(read, 0, 0, 64, 64, filament::backend::PixelBufferDescriptor(
            pixels.data(), pixels.size() * sizeof(glm::vec4), filament::backend::PixelDataFormat::RGBA,
            filament::backend::PixelDataType::FLOAT,
            [](void*, size_t, void* user) { *static_cast<std::atomic_bool*>(user) = true; }, &done));
        core.renderer->endFrame(); engine.flushAndWait(); engine.destroy(read);
        require(done, "Heat field readback did not complete");
        return pixels;
    };
    const auto first = capture_field();
    offsets[0].second = .7f; feature.update(context, frame);
    require(feature.prepare_temporal_distortion(context, kExtent, kExtent), "Floating heat field prepare failed");
    const auto moved = capture_field();
    const auto center = [](const Pixels& pixels) {
        double mass = 0, moment = 0; size_t support = 0;
        for (size_t index = 0; index < pixels.size(); ++index) {
            const auto& pixel = pixels[index];
            for (int channel = 0; channel < 4; ++channel) require(std::isfinite(pixel[channel]), "Nonfinite heat field node");
            require(std::abs(pixel.x) <= .1 / 64 + 1e-7 && std::abs(pixel.y) <= .1 / 64 + 1e-7,
                "Heat field violates global component bound");
            require(pixel.a >= 0 && pixel.a <= 1.0001f, "Heat field uses invalid signed blend coverage");
            if (pixel.a > .0001f) ++support;
            mass += pixel.a; moment += pixel.a * (double(index / 64) + .5);
        }
        require(support >= 8 && mass > .01, "Heat field/anchor oracle has insufficient actual contribution");
        return moment / mass;
    };
    const double actual_shift = center(moved) - center(first);
    const double expected_shift = -.7 * 32 / (6 * std::tan(glm::radians(30.0)));
    require(std::abs(actual_shift - expected_shift) < 1.25 && actual_shift < -4,
        "Floating chimney heat field failed independently projected Z anchor movement");
    std::vector<TemporalEffectDraw> descriptors; feature.append_temporal_draws(descriptors);
    require(!descriptors.empty() && descriptors.front().kind == TemporalEffectKind::Smoke, "Particle feature did not export actual source geometry");
    const auto transform = engine.getTransformManager().getWorldTransform(
        engine.getTransformManager().getInstance(descriptors.front().source_entity));
    require(std::abs(transform[3].z - .7f) < 1e-6f, "Smoke did not follow authored floating group");
    // The wall is a single source-depth texel between coarse node centers:
    // a point-only producer misses it, while interpolated offsets enter it.
    for(size_t y=0;y<96;++y) depth_pixels[y*96+49]=1.0f;
    depth->setImage(engine,0,{depth_pixels.data(),depth_pixels.size()*sizeof(float),
        filament::Texture::Format::R,filament::Texture::Type::FLOAT});
    engine.flushAndWait();
    require(feature.prepare_temporal_distortion(context,kExtent,kExtent),"Occluded heat field prepare failed");
    const auto wall=capture_field();
    const auto sample=[](const Pixels& pixels,double u,double v) {
        const double sx=u*64-.5,sy=v*64-.5;
        const auto x=static_cast<int>(std::floor(sx)),y=static_cast<int>(std::floor(sy));
        const double fx=sx-x,fy=sy-y;
        const auto at=[&](int column,int row){return glm::dvec4(pixels[size_t(std::clamp(row,0,63))*64+size_t(std::clamp(column,0,63))]);};
        return glm::mix(glm::mix(at(x,y),at(x+1,y),fx),glm::mix(at(x,y+1),at(x+1,y+1),fx),fy);
    };
    size_t protected_samples=0,visible_samples=0;
    for(size_t y=0;y<kExtent;++y) for(size_t x=0;x<kExtent;++x) {
        const double u=(double(x)+.5)/kExtent,v=(double(y)+.5)/kExtent;
        const auto baseline=sample(moved,u,v),actual=sample(wall,u,v);
        if(x/2==49 && baseline.a>.001) {
            ++protected_samples;
            require(glm::length(actual)<1e-10,"Bilinear heat field leaked into sharp opaque wall between coarse centers");
        }
        if(x>108 && actual.a>.001) ++visible_samples;
    }
    require(protected_samples>=8 && visible_samples>=8,"Conservative wall oracle lost occluded or visible-side coverage");
    std::printf("[temporal_effects_test] sharp depth strip protected_samples=%zu visible_side=%zu PASS\n",protected_samples,visible_samples);
    const auto before_reuse = owners(engine);
    require(feature.prepare_temporal_distortion(context, kExtent, kExtent) && owners(engine) == before_reuse,
        "Heat field rebuilt native geometry/targets on same-size prepare");
    feature.set_enabled(context, false); feature.prepare_temporal_distortion(context, kExtent, kExtent);
    const auto disabled = capture_field();
    for (const auto& pixel : disabled) require(glm::length(pixel) < 1e-7f, "Disabled heat feature left stale field nodes");
    cleanup.destroy(); owned_depth.reset(); assets.reset();
    std::printf("[temporal_effects_test] bounded heat grid, positive coverage, Off no-allocation, floating shift=%.7g expected=%.7g and clear/reuse PASS\n", actual_shift, expected_shift);
}

void run(filament::Engine& engine, bool wrong_previous, bool rejected_history) {
    // Warm Filament's engine-owned caches before the exact owner baseline.
    { EffectsFixture fixture(engine); fixture.init(); fixture.add(TemporalEffectKind::Flame); fixture.capture(.5f); }
    engine.flushAndWait();
    const auto baseline = owners(engine);
    run_contributors(engine, wrong_previous, rejected_history);
    run_recycled_smoke(engine);
    run_billboards(engine);
    run_rain(engine);
    run_clouds(engine);
    run_heat_field(engine);
    engine.flushAndWait();
    require(owners(engine) == baseline, "Temporal effect borrowers/owners did not return to baseline");
}
} // namespace
} // namespace heaven

int main(int argc, char** argv) {
    const bool wrong_previous = argc == 2 && std::string_view(argv[1]) == "--negative-wrong-prior";
    const bool rejected_history = argc == 2 && std::string_view(argv[1]) == "--negative-rejected-history";
    const bool negative = wrong_previous || rejected_history;
    if (argc > 1 && !negative) { std::fprintf(stderr, "Usage: %s [--negative-wrong-prior|--negative-rejected-history]\n", argv[0]); return 2; }
    filament::Engine* engine = nullptr; int status = 0;
    try {
        engine = filament::Engine::create(filament::Engine::Backend::VULKAN);
        heaven::require(engine, "Vulkan Engine creation failed");
        heaven::run(*engine, wrong_previous, rejected_history);
        std::puts(negative ? "[temporal_effects_test] NEGATIVE NOT DETECTED" : "[temporal_effects_test] PASS");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[temporal_effects_test] FAIL: %s\n", error.what()); status = 1;
    }
    if (engine) filament::Engine::destroy(&engine);
    return status;
}
