#include "frame_generation_output.h"
#include "rhi/filament_util.h"
#include "rhi/materials_data.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <type_traits>
#include <cstdint>
#include <utility>

namespace heaven {

struct FrameGenerationOutput::Impl {
    struct Pass {
        filament::Material* material{};
        filament::MaterialInstance* instance{};
        filament::Scene* scene{};
        filament::View* view{};
        utils::Entity entity;
        bool ready = false;
    };
    explicit Impl(const BackendCore& value) : core(value) {}
    BackendCore core;
    // The last two passes and their targets are lazy: field packing, scene warp.
    std::array<Pass, 5> passes{};
    filament::VertexBuffer* vertices{};
    filament::IndexBuffer* indices{};
    utils::Entity camera_entity;
    filament::Camera* camera{};
    filament::Skybox* transparent_background{};
    filament::View* ui_clear_view{};
    filament::Scene* ui_clear_scene{};
    filament::View* borrowed_ui_view{};
    filament::RenderTarget* previous_ui_target{};
    filament::Skybox* previous_ui_background{};
    filament::View::BlendMode previous_ui_blend = filament::View::BlendMode::OPAQUE;
    filament::Texture* placeholder{};
    filament::Texture* color{};
    filament::Texture* scene_depth{};
    filament::Texture* ui{};
    filament::Texture* ui_depth{};
    filament::Texture* depth{};
    filament::Texture* motion{};
    filament::Texture* undistorted_color{};
    filament::Texture* distortion{};
    filament::RenderTarget* scene_target{};
    filament::RenderTarget* ui_target{};
    filament::RenderTarget* depth_target{};
    filament::RenderTarget* motion_target{};
    filament::RenderTarget* undistorted_target{};
    filament::RenderTarget* distortion_target{};
    int width = 0, height = 0;
    bool prepared = false;
    bool has_ui = false;
    bool distortion_active = false;

    ~Impl() {
        if (!core.engine) return;
        destroy_targets();
        destroy_pipeline();
    }
    filament::TextureSampler sampler() const {
        return {filament::TextureSampler::MinFilter::NEAREST,
            filament::TextureSampler::MagFilter::NEAREST,
            filament::TextureSampler::WrapMode::CLAMP_TO_EDGE};
    }
    void unbind_targets() {
        prepared = false;
        distortion_active = false;
        unbind_ui();
        if (ui_clear_view) ui_clear_view->setRenderTarget(nullptr);
        for (auto& pass : passes) if (pass.view) pass.view->setRenderTarget(nullptr);
        if (!placeholder) return;
        for (size_t i = 0; i < 2; ++i)
            if (passes[i].instance) {
                passes[i].instance->setParameter("packedTexture", placeholder, sampler());
                passes[i].instance->setParameter("effectTexture", placeholder, sampler());
            }
        if (passes[2].instance) {
            passes[2].instance->setParameter("sceneTexture", placeholder, sampler());
            passes[2].instance->setParameter("uiTexture", placeholder, sampler());
        }
        for (size_t i = 3; i < passes.size(); ++i) if (passes[i].instance) {
            passes[i].instance->setParameter("coarseField", placeholder, sampler());
            passes[i].instance->setParameter("distortionField", placeholder, sampler());
            passes[i].instance->setParameter("sceneTexture", placeholder, sampler());
        }
    }
    void unbind_ui() {
        if (!borrowed_ui_view) return;
        // The owner may already have restored these values after its draw.
        if (borrowed_ui_view->getRenderTarget() == ui_target) {
            borrowed_ui_view->setRenderTarget(previous_ui_target);
            borrowed_ui_view->setBlendMode(previous_ui_blend);
        }
        auto* scene = borrowed_ui_view->getScene();
        if (scene && scene->getSkybox() == transparent_background) scene->setSkybox(previous_ui_background);
        borrowed_ui_view = nullptr; previous_ui_target = nullptr; previous_ui_background = nullptr;
    }
    void destroy_targets() {
        unbind_targets();
        auto& engine = *core.engine;
        for (auto** target : {&scene_target, &ui_target, &depth_target, &motion_target,
                &undistorted_target, &distortion_target}) {
            if (*target) engine.destroy(*target);
            *target = nullptr;
        }
        for (auto** texture : {&color, &scene_depth, &ui, &ui_depth, &depth, &motion,
                &undistorted_color, &distortion}) {
            if (*texture) engine.destroy(*texture);
            *texture = nullptr;
        }
    }
    void destroy_pipeline() {
        auto& engine = *core.engine;
        if (ui_clear_view) { ui_clear_view->setScene(nullptr); ui_clear_view->setCamera(nullptr); engine.destroy(ui_clear_view); }
        if (ui_clear_scene) { ui_clear_scene->setSkybox(nullptr); engine.destroy(ui_clear_scene); }
        ui_clear_view = nullptr; ui_clear_scene = nullptr;
        for (auto& pass : passes) {
            if (pass.view) { pass.view->setCamera(nullptr); pass.view->setScene(nullptr); }
            if (pass.entity) {
                if (pass.scene) pass.scene->remove(pass.entity);
                engine.destroy(pass.entity);
                utils::EntityManager::get().destroy(pass.entity);
            }
            if (pass.view) engine.destroy(pass.view);
            if (pass.scene) engine.destroy(pass.scene);
            if (pass.instance) engine.destroy(pass.instance);
            if (pass.material) engine.destroy(pass.material);
            pass = {};
        }
        if (vertices) engine.destroy(vertices);
        if (indices) engine.destroy(indices);
        if (transparent_background) engine.destroy(transparent_background);
        if (placeholder) engine.destroy(placeholder);
        if (camera_entity) {
            engine.destroyCameraComponent(camera_entity);
            utils::EntityManager::get().destroy(camera_entity);
        }
        vertices = nullptr; indices = nullptr; transparent_background = nullptr;
        placeholder = nullptr; camera = nullptr; camera_entity = {};
    }
    bool build_pipeline() {
        if (passes[2].view) return true;
        auto& engine = *core.engine;
        struct Vertex { filament::math::float3 position; };
        static constexpr std::array<Vertex, 4> quad{{{{-1,-1,0}}, {{1,-1,0}}, {{1,1,0}}, {{-1,1,0}}}};
        static constexpr std::array<uint16_t, 6> triangles{0,1,2,0,2,3};
        vertices = filament::VertexBuffer::Builder().vertexCount(4).bufferCount(1)
            .attribute(filament::VertexAttribute::POSITION, 0, filament::VertexBuffer::AttributeType::FLOAT3, 0, sizeof(Vertex)).build(engine);
        indices = filament::IndexBuffer::Builder().indexCount(6)
            .bufferType(filament::IndexBuffer::IndexType::USHORT).build(engine);
        camera_entity = utils::EntityManager::get().create();
        camera = engine.createCamera(camera_entity);
        transparent_background = filament::Skybox::Builder().color({0,0,0,0}).build(engine);
        placeholder = filament::Texture::Builder().width(1).height(1).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).build(engine);
        if (!vertices || !indices || !camera || !transparent_background || !placeholder) {
            destroy_pipeline(); return false;
        }
        static constexpr std::array<uint8_t,4> zero{0,0,0,0};
        placeholder->setImage(engine,0,{zero.data(),zero.size(),
            filament::Texture::Format::RGBA,filament::Texture::Type::UBYTE});
        vertices->setBufferAt(engine, 0, {quad.data(), sizeof(quad)});
        indices->setBuffer(engine, {triangles.data(), sizeof(triangles)});
        camera->setProjection(filament::Camera::Projection::ORTHO, -1, 1, -1, 1, .1, 10);
        camera->lookAt({0,0,1}, {0,0,0}, {0,1,0});
        camera->setExposure(1);
        ui_clear_scene = engine.createScene(); ui_clear_view = engine.createView();
        if (!ui_clear_scene || !ui_clear_view) { destroy_pipeline(); return false; }
        ui_clear_scene->setSkybox(transparent_background);
        ui_clear_view->setScene(ui_clear_scene); ui_clear_view->setCamera(camera);
        ui_clear_view->setPostProcessingEnabled(false); ui_clear_view->setShadowingEnabled(false);
        ui_clear_view->setAntiAliasing(filament::AntiAliasing::NONE);
        ui_clear_view->setMultiSampleAntiAliasingOptions({.enabled = false});
        ui_clear_view->setBlendMode(filament::View::BlendMode::OPAQUE);
        for (size_t i = 0; i < 3; ++i) {
            auto& pass = passes[i];
            pass.material = filament::Material::Builder().package(
                i == 2 ? k_heaven_fg_composite_filamat : k_heaven_temporal_extract_filamat,
                i == 2 ? k_heaven_fg_composite_filamat_size : k_heaven_temporal_extract_filamat_size).build(engine);
            if (!pass.material) { destroy_pipeline(); return false; }
            pass.instance = pass.material->createInstance();
            pass.scene = engine.createScene();
            pass.view = engine.createView();
            if (!pass.instance || !pass.scene || !pass.view) { destroy_pipeline(); return false; }
            pass.entity = utils::EntityManager::get().create();
            if (filament::RenderableManager::Builder(1).geometry(0,
                    filament::RenderableManager::PrimitiveType::TRIANGLES, vertices, indices)
                    .material(0, pass.instance).culling(false).castShadows(false).receiveShadows(false)
                    .boundingBox({{0,0,0}, {1,1,1}}).build(engine, pass.entity) != filament::RenderableManager::Builder::Success) {
                destroy_pipeline(); return false;
            }
            pass.scene->addEntity(pass.entity);
            pass.view->setScene(pass.scene); pass.view->setCamera(camera);
            pass.view->setPostProcessingEnabled(false); pass.view->setShadowingEnabled(false);
            pass.view->setAntiAliasing(filament::AntiAliasing::NONE);
            pass.view->setMultiSampleAntiAliasingOptions({.enabled = false});
            pass.view->setBlendMode(filament::View::BlendMode::OPAQUE);
            if (i != 2) pass.instance->setParameter("extractDepth", i == 0);
        }
        unbind_targets();
        return true;
    }
    bool valid() const {
        return scene_target && ui_target && depth_target && motion_target && passes[2].view;
    }
    bool build_distortion() {
        if (passes[3].ready && passes[4].ready && undistorted_target && distortion_target) return true;
        auto& engine = *core.engine;
        // Partial construction is kept for ordinary owner teardown after an
        // allocation failure; never claim an incomplete pipeline is usable.
        for (size_t i = 3; i < passes.size(); ++i) {
            auto& pass = passes[i];
            if (pass.ready) continue;
            if (pass.material || pass.instance || pass.scene || pass.view || pass.entity) return false;
            pass.material = filament::Material::Builder().package(
                k_heaven_fg_distortion_filamat, k_heaven_fg_distortion_filamat_size).build(engine);
            if (!pass.material) return false;
            pass.instance = pass.material->createInstance();
            pass.scene = engine.createScene(); pass.view = engine.createView();
            if (!pass.instance || !pass.scene || !pass.view) return false;
            pass.entity = utils::EntityManager::get().create();
            if (filament::RenderableManager::Builder(1).geometry(0,
                    filament::RenderableManager::PrimitiveType::TRIANGLES, vertices, indices)
                    .material(0, pass.instance).culling(false).castShadows(false).receiveShadows(false)
                    .boundingBox({{0,0,0}, {1,1,1}}).build(engine, pass.entity) != filament::RenderableManager::Builder::Success)
                return false;
            pass.scene->addEntity(pass.entity);
            pass.view->setScene(pass.scene); pass.view->setCamera(camera);
            pass.view->setPostProcessingEnabled(false); pass.view->setShadowingEnabled(false);
            pass.view->setAntiAliasing(filament::AntiAliasing::NONE);
            pass.view->setMultiSampleAntiAliasingOptions({.enabled = false});
            pass.view->setBlendMode(filament::View::BlendMode::OPAQUE);
            pass.instance->setParameter("extractField", i == 3);
            pass.ready = true;
        }
        using Usage = filament::Texture::Usage;
        using Bits = std::underlying_type_t<Usage>;
        constexpr auto usage = std::bit_cast<Usage>(static_cast<Bits>(
            std::to_underlying(Usage::COLOR_ATTACHMENT) | std::to_underlying(Usage::SAMPLEABLE) |
            std::to_underlying(Usage::BLIT_SRC)));
        if (!undistorted_color) undistorted_color = filament::Texture::Builder()
            .width(width).height(height).levels(1).format(filament::Texture::InternalFormat::RGBA8)
            .usage(usage).build(engine);
        if (!distortion) distortion = filament::Texture::Builder()
            .width(width).height(height).levels(1).format(filament::Texture::InternalFormat::RGBA16F)
            .usage(usage).build(engine);
        if (!undistorted_color || !distortion) return false;
        if (!undistorted_target) undistorted_target = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, undistorted_color)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, scene_depth).build(engine);
        if (!distortion_target) distortion_target = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, distortion).build(engine);
        return undistorted_target && distortion_target;
    }
};

FrameGenerationOutput::FrameGenerationOutput(const BackendCore& core) : impl_(std::make_unique<Impl>(core)) {}
FrameGenerationOutput::~FrameGenerationOutput() = default;
bool FrameGenerationOutput::needs_resize(int width, int height) const noexcept { return width != impl_->width || height != impl_->height; }
bool FrameGenerationOutput::valid() const noexcept { return impl_->valid(); }

bool FrameGenerationOutput::ensure(int width, int height) {
    auto& p = *impl_;
    if (!p.core.engine || width <= 0 || height <= 0 || !p.build_pipeline()) return false;
    if (!needs_resize(width, height) && valid()) return true;
    p.destroy_targets();
    p.width = width; p.height = height;
    auto& engine = *p.core.engine;
    // TextureUsage has a fixed underlying bitmask representation. Preserve
    // those bits without Clang 21's false invalid-enumerator enum-cast finding.
    using Usage = filament::Texture::Usage;
    using UsageBits = std::underlying_type_t<Usage>;
    constexpr auto sampled_color_bits = static_cast<UsageBits>(
        std::to_underlying(Usage::COLOR_ATTACHMENT) |
        std::to_underlying(Usage::SAMPLEABLE) | std::to_underlying(Usage::BLIT_SRC));
    constexpr auto sampled_color = std::bit_cast<Usage>(sampled_color_bits);
    auto texture = [&](filament::Texture::InternalFormat format, filament::Texture::Usage usage) {
        return filament::Texture::Builder().width(width).height(height).levels(1).format(format).usage(usage).build(engine);
    };
    p.color = texture(filament::Texture::InternalFormat::RGBA8, sampled_color);
    p.ui = texture(filament::Texture::InternalFormat::RGBA8, sampled_color);
    p.depth = texture(filament::Texture::InternalFormat::R32F, sampled_color);
    p.motion = texture(filament::Texture::InternalFormat::RG16F, sampled_color);
    p.scene_depth = texture(filament::Texture::InternalFormat::DEPTH32F, filament::Texture::Usage::DEPTH_ATTACHMENT);
    p.ui_depth = texture(filament::Texture::InternalFormat::DEPTH32F, filament::Texture::Usage::DEPTH_ATTACHMENT);
    if (!p.color || !p.ui || !p.depth || !p.motion || !p.scene_depth || !p.ui_depth) { p.destroy_targets(); return false; }
    auto target = [&](filament::Texture* color, filament::Texture* depth = nullptr) {
        auto builder = filament::RenderTarget::Builder().texture(filament::RenderTarget::AttachmentPoint::COLOR0, color);
        if (depth) builder.texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth);
        return builder.build(engine);
    };
    p.scene_target = target(p.color, p.scene_depth); p.ui_target = target(p.ui, p.ui_depth);
    p.depth_target = target(p.depth); p.motion_target = target(p.motion);
    if (!valid()) { p.destroy_targets(); return false; }
    return true;
}

filament::RenderTarget* FrameGenerationOutput::scene_rt() const noexcept {
    return impl_->distortion_active ? impl_->undistorted_target : impl_->scene_target;
}
filament::RenderTarget* FrameGenerationOutput::ui_rt() const noexcept { return impl_->ui_target; }
filament::Texture* FrameGenerationOutput::color_texture() const noexcept { return impl_->color; }
filament::Texture* FrameGenerationOutput::depth_texture() const noexcept { return impl_->depth; }
filament::Texture* FrameGenerationOutput::motion_texture() const noexcept { return impl_->motion; }
filament::Texture* FrameGenerationOutput::ui_texture() const noexcept { return impl_->ui; }
filament::Texture* FrameGenerationOutput::distortion_texture() const noexcept {
    return impl_->distortion_active ? impl_->distortion : nullptr;
}

bool FrameGenerationOutput::prepare_distortion(filament::Texture* coarse_field) {
    auto& p = *impl_;
    p.distortion_active = false;
    for (size_t i = 3; i < p.passes.size(); ++i) if (p.passes[i].instance)
        p.passes[i].instance->setParameter("coarseField", p.placeholder, p.sampler());
    if (!coarse_field) return true;
    using Format = filament::Texture::InternalFormat;
    if (!p.prepared || !p.valid() || coarse_field->getWidth() == 0 || coarse_field->getHeight() == 0 ||
        coarse_field->getWidth() > 64 || coarse_field->getHeight() > 64 ||
        (coarse_field->getFormat() != Format::RGBA16F && coarse_field->getFormat() != Format::RGBA32F) ||
        !p.build_distortion()) return false;
    const filament::TextureSampler linear(filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR, filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    for (size_t i = 3; i < p.passes.size(); ++i) {
        auto& pass = p.passes[i];
        pass.view->setViewport({0,0,static_cast<uint32_t>(p.width),static_cast<uint32_t>(p.height)});
        // Both branches exist in the shader. Never bind an attachment as a
        // sampled input even when its branch is disabled by a uniform.
        pass.instance->setParameter("coarseField", i == 3 ? coarse_field : p.placeholder, linear);
        pass.instance->setParameter("distortionField", i == 4 ? p.distortion : p.placeholder, p.sampler());
        pass.instance->setParameter("sceneTexture", i == 4 ? p.undistorted_color : p.placeholder, linear);
        pass.instance->setParameter("fieldBound", 0.1f / static_cast<float>(
            std::max(coarse_field->getWidth(), coarse_field->getHeight())));
    }
    p.passes[3].view->setRenderTarget(p.distortion_target);
    p.passes[4].view->setRenderTarget(p.scene_target);
    p.distortion_active = true;
    return true;
}

void FrameGenerationOutput::bind_ui(filament::View& view) {
    impl_->unbind_ui();
    impl_->borrowed_ui_view = &view;
    impl_->previous_ui_target = view.getRenderTarget();
    impl_->previous_ui_blend = view.getBlendMode();
    impl_->previous_ui_background = view.getScene() ? view.getScene()->getSkybox() : nullptr;
    view.setRenderTarget(impl_->ui_target);
    if (view.getScene()) view.getScene()->setSkybox(impl_->transparent_background);
    view.setBlendMode(filament::View::BlendMode::OPAQUE);
}
void FrameGenerationOutput::unbind_ui() { impl_->unbind_ui(); }

void FrameGenerationOutput::prepare(filament::Texture* packed, int width, int height, bool has_ui,
        const TemporalCameraHistory& camera_history, filament::Texture* effects) {
    auto& p = *impl_;
    if (!valid() || !packed || needs_resize(width, height)) { p.prepared = false; return; }
    if (effects && (effects->getWidth() != static_cast<uint32_t>(width) ||
                    effects->getHeight() != static_cast<uint32_t>(height))) {
        p.prepared = false;
        return;
    }
    for (auto& pass : p.passes) if(pass.view) pass.view->setViewport({0,0,static_cast<uint32_t>(width),static_cast<uint32_t>(height)});
    p.ui_clear_view->setViewport({0,0,static_cast<uint32_t>(width),static_cast<uint32_t>(height)});
    p.ui_clear_view->setRenderTarget(p.ui_target);
    p.passes[0].view->setRenderTarget(p.depth_target);
    p.passes[1].view->setRenderTarget(p.motion_target);
    const auto world_from_current_clip = glm::inverse(camera_history.current_clip_from_world);
    bool finite_history = camera_history.valid;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            finite_history &= std::isfinite(world_from_current_clip[column][row]) &&
                std::isfinite(camera_history.previous_clip_from_world[column][row]);
    for (size_t i = 0; i < 2; ++i) {
        auto* instance = p.passes[i].instance;
        instance->setParameter("packedTexture", packed, p.sampler());
        instance->setParameter("effectTexture", effects ? effects : p.placeholder, p.sampler());
        instance->setParameter("hasEffects", effects != nullptr);
        // Invalid inverses are never uploaded, even while history is disabled.
        instance->setParameter("worldFromCurrentClip", glm_to_filament(
            finite_history ? world_from_current_clip : glm::mat4(1.0f)));
        instance->setParameter("previousClipFromWorld", glm_to_filament(
            finite_history ? camera_history.previous_clip_from_world : glm::mat4(1.0f)));
        instance->setParameter("cameraHistoryValid", finite_history);
    }
    p.passes[2].instance->setParameter("sceneTexture", p.color, p.sampler());
    p.passes[2].instance->setParameter("uiTexture", p.ui, p.sampler());
    p.passes[2].instance->setParameter("hasUi", has_ui);
    p.has_ui = has_ui;
    p.prepared = true;
}
void FrameGenerationOutput::render_extract() {
    if (!impl_->prepared || !impl_->core.renderer) return;
    impl_->core.renderer->render(impl_->passes[0].view);
    impl_->core.renderer->render(impl_->passes[1].view);
    if (impl_->distortion_active) impl_->core.renderer->render(impl_->passes[3].view);
    if (!impl_->has_ui) impl_->core.renderer->render(impl_->ui_clear_view);
}
void FrameGenerationOutput::render_composite() {
    if (!impl_->prepared || !impl_->core.renderer) return;
    if (impl_->distortion_active) impl_->core.renderer->render(impl_->passes[4].view);
    impl_->core.renderer->render(impl_->passes[2].view);
}

} // namespace heaven
