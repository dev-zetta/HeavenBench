#include "frame_generation_output.h"
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
#include <cstdint>

namespace heaven {

struct FrameGenerationOutput::Impl {
    struct Pass {
        filament::Material* material{};
        filament::MaterialInstance* instance{};
        filament::Scene* scene{};
        filament::View* view{};
        utils::Entity entity;
    };
    explicit Impl(const BackendCore& value) : core(value) {}
    BackendCore core;
    std::array<Pass, 3> passes{}; // normalized depth, UV motion, real-frame UI composite
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
    filament::RenderTarget* scene_target{};
    filament::RenderTarget* ui_target{};
    filament::RenderTarget* depth_target{};
    filament::RenderTarget* motion_target{};
    int width = 0, height = 0;
    bool prepared = false;
    bool has_ui = false;

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
        unbind_ui();
        if (ui_clear_view) ui_clear_view->setRenderTarget(nullptr);
        for (auto& pass : passes) if (pass.view) pass.view->setRenderTarget(nullptr);
        if (!placeholder) return;
        for (size_t i = 0; i < 2; ++i)
            if (passes[i].instance) passes[i].instance->setParameter("packedTexture", placeholder, sampler());
        if (passes[2].instance) {
            passes[2].instance->setParameter("sceneTexture", placeholder, sampler());
            passes[2].instance->setParameter("uiTexture", placeholder, sampler());
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
        for (auto** target : {&scene_target, &ui_target, &depth_target, &motion_target}) {
            if (*target) engine.destroy(*target);
            *target = nullptr;
        }
        for (auto** texture : {&color, &scene_depth, &ui, &ui_depth, &depth, &motion}) {
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
        for (size_t i = 0; i < passes.size(); ++i) {
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
    const auto sampled_color = filament::Texture::Usage::COLOR_ATTACHMENT | filament::Texture::Usage::SAMPLEABLE | filament::Texture::Usage::BLIT_SRC;
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

filament::RenderTarget* FrameGenerationOutput::scene_rt() const noexcept { return impl_->scene_target; }
filament::RenderTarget* FrameGenerationOutput::ui_rt() const noexcept { return impl_->ui_target; }
filament::Texture* FrameGenerationOutput::color_texture() const noexcept { return impl_->color; }
filament::Texture* FrameGenerationOutput::depth_texture() const noexcept { return impl_->depth; }
filament::Texture* FrameGenerationOutput::motion_texture() const noexcept { return impl_->motion; }
filament::Texture* FrameGenerationOutput::ui_texture() const noexcept { return impl_->ui; }

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

void FrameGenerationOutput::prepare(filament::Texture* packed, int width, int height, bool has_ui) {
    auto& p = *impl_;
    if (!valid() || !packed || needs_resize(width, height)) { p.prepared = false; return; }
    for (auto& pass : p.passes) pass.view->setViewport({0,0,static_cast<uint32_t>(width),static_cast<uint32_t>(height)});
    p.ui_clear_view->setViewport({0,0,static_cast<uint32_t>(width),static_cast<uint32_t>(height)});
    p.ui_clear_view->setRenderTarget(p.ui_target);
    p.passes[0].view->setRenderTarget(p.depth_target);
    p.passes[1].view->setRenderTarget(p.motion_target);
    for (size_t i = 0; i < 2; ++i) p.passes[i].instance->setParameter("packedTexture", packed, p.sampler());
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
    if (!impl_->has_ui) impl_->core.renderer->render(impl_->ui_clear_view);
}
void FrameGenerationOutput::render_composite() {
    if (impl_->prepared && impl_->core.renderer) impl_->core.renderer->render(impl_->passes[2].view);
}

} // namespace heaven
