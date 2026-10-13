// Native SDR output oracle, with no scene assets or frame-generation SDK.
// Exercises FrameGenerationOutput and its embedded production materials.
// Generated SDK images / native presentation are covered by the separate
// runtime UI-pair oracle; this fixture validates the renderer's real output.
#include "rhi/frame_generation/frame_generation_output.h"
#include "rhi/materials_data.h"

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
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace heaven {
namespace {
constexpr const char* kTag = "[frame_generation_output_test]";
using Rgba = std::array<double, 4>;
using Pixels = std::vector<std::uint8_t>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Rect {
    double left, top, right, bottom;
};

struct Widget {
    Rect rect;
    Rgba straight_color;
    bool present = true;
};

constexpr Rect kTopLeft{.10, .10, .45, .40};
constexpr Rect kBottomLeft{.10, .60, .45, .90};
constexpr std::array<Widget, 3> kWidgets{{
    {kTopLeft, {.80, .20, .40, .50}},
    {{.55, .60, .90, .90}, {.10, .70, .30, 1.0}},
    {{.55, .10, .90, .40}, {.90, .80, .70, 0.0}}
}};
constexpr Rgba kScene{.20, .40, .60, 1.0};
constexpr Rgba kAlternateScene{.60, .20, .10, 1.0};

filament::math::float4 native_color(const Rgba& value) {
    return {float(value[0]), float(value[1]), float(value[2]), float(value[3])};
}

std::array<std::size_t, 18> owners(filament::Engine& engine) {
    return {engine.getBufferObjectCount(), engine.getViewCount(), engine.getSceneCount(),
        engine.getSwapChainCount(), engine.getIndexBufferCount(), engine.getSkinningBufferCount(),
        engine.getMorphTargetBufferCount(), engine.getInstanceBufferCount(),
        engine.getVertexBufferCount(), engine.getIndirectLightCount(), engine.getMaterialCount(),
        engine.getTextureCount(), engine.getSkyboxeCount(), engine.getColorGradingCount(),
        engine.getRenderTargetCount(), engine.getRenderableManager().getComponentCount(),
        engine.getTransformManager().getComponentCount(), engine.getLightManager().getComponentCount()};
}

void same_owners(const std::array<std::size_t, 18>& actual,
                 const std::array<std::size_t, 18>& expected, const char* label) {
    constexpr std::array<const char*, 18> names{"buffer", "view", "scene", "swapchain", "index",
        "skinning", "morph", "instance", "vertex", "ibl", "material", "texture", "skybox",
        "grading", "target", "renderable", "transform", "light"};
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != expected[i]) {
            std::printf("%s %s %s owners=%zu expected=%zu\n", kTag, label, names[i], actual[i], expected[i]);
            throw std::runtime_error("FrameGenerationOutput retained or replaced an unexpected native owner");
        }
    }
}

void save_pixels(const Pixels& pixels, std::uint32_t width, std::uint32_t height,
                 const std::string& label) {
    const char* directory = std::getenv("HEAVEN_FG_OUTPUT_TEST_DIR");
    if (!directory || !*directory) return;
    const auto path = std::filesystem::path(directory) / (label + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    require(bool(stream), "Cannot open native output pixel evidence");
    stream << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        stream.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
    require(bool(stream), "Cannot finish native output pixel evidence");
}

class OutputFixture {
public:
    explicit OutputFixture(filament::Engine& engine) : engine_(engine) {}
    ~OutputFixture() {
        engine_.flushAndWait();
        if (scene_view_) scene_view_->setRenderTarget(nullptr);
        // Destruction must restore an outstanding UI borrow, too. Do not
        // detach that view before the output owner can restore its sentinel.
        output_.reset();
        if (ui_view_) ui_view_->setRenderTarget(nullptr);
        for (auto entity : widgets_) if (entity) {
            if (ui_scene_) ui_scene_->remove(entity);
            engine_.destroy(entity);
            utils::EntityManager::get().destroy(entity);
        }
        if (scene_view_) { scene_view_->setScene(nullptr); scene_view_->setCamera(nullptr); engine_.destroy(scene_view_); }
        if (ui_view_) { ui_view_->setScene(nullptr); ui_view_->setCamera(nullptr); engine_.destroy(ui_view_); }
        if (scene_) { scene_->setSkybox(nullptr); engine_.destroy(scene_); }
        if (ui_scene_) { ui_scene_->setSkybox(nullptr); engine_.destroy(ui_scene_); }
        if (scene_sky_) engine_.destroy(scene_sky_);
        if (sentinel_sky_) engine_.destroy(sentinel_sky_);
        if (sentinel_target_) engine_.destroy(sentinel_target_);
        if (sentinel_texture_) engine_.destroy(sentinel_texture_);
        if (material_instance_) engine_.destroy(material_instance_);
        if (material_) engine_.destroy(material_);
        if (vertices_) engine_.destroy(vertices_);
        if (indices_) engine_.destroy(indices_);
        if (packed_) engine_.destroy(packed_);
        if (camera_entity_) {
            engine_.destroyCameraComponent(camera_entity_);
            utils::EntityManager::get().destroy(camera_entity_);
        }
        if (renderer_) engine_.destroy(renderer_);
        if (swapchain_) engine_.destroy(swapchain_);
        engine_.flushAndWait();
    }
    OutputFixture(const OutputFixture&) = delete;
    OutputFixture& operator=(const OutputFixture&) = delete;

    void init() {
        renderer_ = engine_.createRenderer();
        scene_ = engine_.createScene();
        ui_scene_ = engine_.createScene();
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        require(renderer_ && scene_ && ui_scene_ && camera_, "Native output core unavailable");
        renderer_->setDisplayInfo({.refreshRate = 0.0f});
        renderer_->setFrameRateOptions({.interval = 0});
        camera_->setProjection(filament::Camera::Projection::ORTHO, -1, 1, -1, 1, .1, 10);
        camera_->lookAt({0, 0, 1}, {0, 0, 0}, {0, 1, 0});
        camera_->setExposure(1);
        scene_sky_ = filament::Skybox::Builder().color(native_color(kScene)).build(engine_);
        sentinel_sky_ = filament::Skybox::Builder().color({.95f, .05f, .90f, .75f}).build(engine_);
        sentinel_texture_ = filament::Texture::Builder().width(1).height(1).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8)
            .usage(filament::Texture::Usage::COLOR_ATTACHMENT).build(engine_);
        require(scene_sky_ && sentinel_sky_ && sentinel_texture_, "Native output sentinel unavailable");
        sentinel_target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, sentinel_texture_).build(engine_);
        require(sentinel_target_, "Native output sentinel target unavailable");
        scene_->setSkybox(scene_sky_);
        ui_scene_->setSkybox(sentinel_sky_);
        scene_view_ = make_view(scene_);
        ui_view_ = make_view(ui_scene_);
        ui_view_->setRenderTarget(sentinel_target_);
        ui_view_->setBlendMode(filament::View::BlendMode::TRANSLUCENT);
        packed_ = filament::Texture::Builder().width(1).height(1).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).build(engine_);
        require(packed_, "Native output packed placeholder unavailable");
        static constexpr std::array<std::uint8_t, 4> packed_pixel{0, 0, 128, 255};
        packed_->setImage(engine_, 0, {packed_pixel.data(), packed_pixel.size(),
            filament::Texture::Format::RGBA, filament::Texture::Type::UBYTE});
        make_widgets();
        output_ = std::make_unique<FrameGenerationOutput>(BackendCore{
            .engine = &engine_, .scene = scene_, .view = scene_view_, .renderer = renderer_, .camera = camera_});
        resize(80, 64, false);
        require(!output_->ensure(0, height_) && output_->valid(), "Invalid extent replaced valid output targets");
    }

    void set_widget(std::size_t index, bool present, Rect rect) {
        require(index < model_.size(), "Invalid oracle widget index");
        model_[index].rect = rect;
        model_[index].present = present;
        if (present) ui_scene_->addEntity(widgets_[index]);
        else ui_scene_->remove(widgets_[index]);
        update_vertices();
    }

    void resize(std::uint32_t width, std::uint32_t height, bool borrow) {
        engine_.flushAndWait();
        scene_view_->setRenderTarget(nullptr);
        if (borrow) {
            output_->bind_ui(*ui_view_);
            verify_borrowed();
        }
        require(output_->ensure(int(width), int(height)), "Native output allocation/resize failed");
        verify_restored();
        if (swapchain_) engine_.destroy(swapchain_);
        swapchain_ = engine_.createSwapChain(width, height, filament::SwapChain::CONFIG_READABLE);
        require(swapchain_, "Readable native headless swapchain unavailable");
        width_ = width; height_ = height;
        scene_view_->setViewport({0, 0, width, height});
        ui_view_->setViewport({0, 0, width, height});
        scene_view_->setRenderTarget(output_->scene_rt());
        engine_.flushAndWait();
        verify_reuse();
    }

    void verify_reuse() {
        const auto identities = target_identities();
        const auto before = owners(engine_);
        require(!output_->needs_resize(int(width_), int(height_)), "Current output extent requires resize");
        require(output_->ensure(int(width_), int(height_)), "Same-size output reuse failed");
        require(target_identities() == identities, "Same-size ensure replaced an output target/texture");
        same_owners(owners(engine_), before, "same-size reuse");
    }

    void leave_borrowed_for_destruction() {
        // The source view must not retain the output target at owner teardown.
        scene_view_->setRenderTarget(nullptr);
        output_->bind_ui(*ui_view_);
        verify_borrowed();
        output_.reset();
        verify_restored();
    }

    void capture(const char* label, bool has_ui = true, const Rgba& scene = kScene,
                 bool break_clear = false) {
        require(output_ && output_->valid(), "Output capture called without valid targets");
        scene_sky_->setColor(native_color(scene));
        output_->prepare(packed_, int(width_), int(height_), has_ui);
        if (has_ui) {
            output_->bind_ui(*ui_view_);
            verify_borrowed();
            // The production binder's OPAQUE view is essential: rendering an
            // empty transparent view must replace last frame's widget pixels.
            // This deliberate integration fault is the bounded negative control.
            if (break_clear) ui_view_->setBlendMode(filament::View::BlendMode::TRANSLUCENT);
        }
        bool accepted = false;
        for (int retry = 0; retry < 8 && !accepted; ++retry) {
            accepted = renderer_->beginFrame(swapchain_);
            if (!accepted) engine_.flushAndWait();
        }
        require(accepted, "Native output frame rejected all bounded retries");
        renderer_->render(scene_view_);
        output_->render_extract();
        if (has_ui) {
            renderer_->render(ui_view_);
            output_->unbind_ui();
        }
        output_->render_composite();
        const std::size_t bytes = static_cast<std::size_t>(width_) * height_ * 4;
        Pixels real(bytes, 0xDD), layer(bytes, 0xDD), background(bytes, 0xDD);
        std::array<std::atomic_bool, 3> done{};
        renderer_->readPixels(0, 0, width_, height_, descriptor(real, done[0]));
        renderer_->readPixels(output_->ui_rt(), 0, 0, width_, height_, descriptor(layer, done[1]));
        renderer_->readPixels(output_->scene_rt(), 0, 0, width_, height_, descriptor(background, done[2]));
        renderer_->endFrame();
        engine_.flushAndWait();
        require(done[0] && done[1] && done[2], "Native output readback callbacks incomplete after drain");
        verify_restored();
        save_pixels(real, width_, height_, std::string(label) + "-real");
        save_pixels(layer, width_, height_, std::string(label) + "-ui");
        check_pixels(label, real, layer, background, has_ui, scene);
        ++completed_;
    }

    [[nodiscard]] std::size_t completed() const { return completed_; }

private:
    struct Vertex {
        filament::math::float3 position;
        filament::math::float2 uv;
        filament::math::float4 color;
    };
    filament::View* make_view(filament::Scene* scene) {
        auto* view = engine_.createView();
        require(view, "Native output view unavailable");
        view->setScene(scene); view->setCamera(camera_);
        view->setPostProcessingEnabled(false); view->setShadowingEnabled(false);
        view->setAntiAliasing(filament::AntiAliasing::NONE);
        view->setMultiSampleAntiAliasingOptions({.enabled = false});
        view->setTemporalAntiAliasingOptions({.enabled = false});
        view->setDithering(filament::Dithering::NONE);
        view->setBlendMode(filament::View::BlendMode::OPAQUE);
        return view;
    }

    void make_widgets() {
        // Use a production transparent material, whose vertex colors are
        // straight alpha and are premultiplied by its actual fragment shader.
        material_ = filament::Material::Builder().package(k_heaven_unlit_filamat,
            k_heaven_unlit_filamat_size).build(engine_);
        require(material_, "Production transparent widget material unavailable");
        material_instance_ = material_->createInstance();
        require(material_instance_, "Production transparent widget instance unavailable");
        material_instance_->setParameter("hasTexture", false);
        material_instance_->setParameter("baseColorMap", packed_, filament::TextureSampler{});
        vertices_ = filament::VertexBuffer::Builder().vertexCount(std::uint32_t(vertex_data_.size())).bufferCount(1)
            .attribute(filament::VertexAttribute::POSITION, 0, filament::VertexBuffer::AttributeType::FLOAT3,
                offsetof(Vertex, position), sizeof(Vertex))
            .attribute(filament::VertexAttribute::UV0, 0, filament::VertexBuffer::AttributeType::FLOAT2,
                offsetof(Vertex, uv), sizeof(Vertex))
            .attribute(filament::VertexAttribute::COLOR, 0, filament::VertexBuffer::AttributeType::FLOAT4,
                offsetof(Vertex, color), sizeof(Vertex)).build(engine_);
        indices_ = filament::IndexBuffer::Builder().indexCount(std::uint32_t(index_data_.size()))
            .bufferType(filament::IndexBuffer::IndexType::USHORT).build(engine_);
        require(vertices_ && indices_, "Native widget buffers unavailable");
        for (std::size_t i = 0; i < model_.size(); ++i) {
            const auto vertex = std::uint16_t(i * 4);
            const std::array<std::uint16_t, 6> card{vertex, std::uint16_t(vertex + 1), std::uint16_t(vertex + 2),
                vertex, std::uint16_t(vertex + 2), std::uint16_t(vertex + 3)};
            std::copy(card.begin(), card.end(), index_data_.begin() + std::ptrdiff_t(i * 6));
        }
        indices_->setBuffer(engine_, {index_data_.data(), sizeof(index_data_)});
        update_vertices();
        for (std::size_t i = 0; i < widgets_.size(); ++i) {
            widgets_[i] = utils::EntityManager::get().create();
            require(filament::RenderableManager::Builder(1)
                .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                    vertices_, indices_, i * 6, 6)
                .material(0, material_instance_).culling(false).castShadows(false).receiveShadows(false)
                .boundingBox({{0, 0, 0}, {1, 1, .01f}}).build(engine_, widgets_[i]) ==
                    filament::RenderableManager::Builder::Success, "Native widget renderable unavailable");
            ui_scene_->addEntity(widgets_[i]);
        }
    }

    void update_vertices() {
        // Model rectangles are written in independently specified top-left
        // image coordinates; the camera is ordinary OpenGL orthographic.
        for (std::size_t i = 0; i < model_.size(); ++i) {
            const auto& rect = model_[i].rect;
            const std::array<std::array<double, 2>, 4> corners{{
                {rect.left, rect.top}, {rect.left, rect.bottom},
                {rect.right, rect.bottom}, {rect.right, rect.top}}};
            for (std::size_t corner = 0; corner < corners.size(); ++corner) {
                auto& v = vertex_data_[i * 4 + corner];
                v.position = {float(2 * corners[corner][0] - 1), float(1 - 2 * corners[corner][1]), 0};
                v.uv = {float(corners[corner][0]), float(corners[corner][1])};
                v.color = native_color(model_[i].straight_color);
            }
        }
        vertices_->setBufferAt(engine_, 0, {vertex_data_.data(), sizeof(vertex_data_)});
        // All mutations occur between drained captures. Finish this upload
        // before the next mutation of its persistent CPU backing array.
        engine_.flushAndWait();
    }

    void verify_borrowed() const {
        require(ui_view_->getRenderTarget() == output_->ui_rt(), "UI binder did not borrow the production layer target");
        require(ui_view_->getBlendMode() == filament::View::BlendMode::OPAQUE,
            "UI binder did not select replacement view composition");
        require(ui_scene_->getSkybox() && ui_scene_->getSkybox() != sentinel_sky_,
            "UI binder did not provide a transparent clearing background");
    }
    void verify_restored() const {
        require(ui_view_->getRenderTarget() == sentinel_target_, "Output owner did not restore the caller's UI target");
        require(ui_view_->getBlendMode() == filament::View::BlendMode::TRANSLUCENT,
            "Output owner did not restore the caller's UI blend mode");
        require(ui_scene_->getSkybox() == sentinel_sky_, "Output owner did not restore the caller's UI skybox");
    }
    std::array<const void*, 6> target_identities() const {
        return std::array<const void*, 6>{output_->scene_rt(), output_->ui_rt(), output_->color_texture(),
            output_->ui_texture(), output_->depth_texture(), output_->motion_texture()};
    }
    static filament::backend::PixelBufferDescriptor descriptor(Pixels& pixels, std::atomic_bool& done) {
        return {pixels.data(), pixels.size(), filament::backend::PixelDataFormat::RGBA,
            filament::backend::PixelDataType::UBYTE,
            [](void*, std::size_t, void* value) { static_cast<std::atomic_bool*>(value)->store(true); }, &done};
    }

    void check_pixels(const char* label, const Pixels& real, const Pixels& layer,
                      const Pixels& background, bool has_ui, const Rgba& scene) const {
        std::size_t checked = 0, partial = 0, opaque = 0, clear = 0;
        double max_real = 0, max_layer = 0, max_scene = 0;
        std::array<std::size_t, 2> worst{};
        // Avoid rasterization edges; test every remaining interior pixel,
        // including the former position of a removed/moved rectangle.
        for (std::uint32_t y = 2; y + 2 < height_; ++y) {
            for (std::uint32_t x = 2; x + 2 < width_; ++x) {
                const double u = (double(x) + .5) / width_, v = (double(y) + .5) / height_;
                bool near_edge = false;
                for (const auto& widget : model_) {
                    const auto& r = widget.rect;
                    near_edge |= (std::abs(u - r.left) < 2.0 / width_ || std::abs(u - r.right) < 2.0 / width_) &&
                        v > r.top - 2.0 / height_ && v < r.bottom + 2.0 / height_;
                    near_edge |= (std::abs(v - r.top) < 2.0 / height_ || std::abs(v - r.bottom) < 2.0 / height_) &&
                        u > r.left - 2.0 / width_ && u < r.right + 2.0 / width_;
                }
                if (near_edge) continue;
                const Widget* painted = nullptr;
                for (const auto& widget : model_) {
                    const auto& r = widget.rect;
                    if (has_ui && widget.present && u > r.left && u < r.right && v > r.top && v < r.bottom)
                        painted = &widget;
                }
                const double alpha = painted ? painted->straight_color[3] : 0;
                // Independent area-coverage paint model: a fraction alpha
                // replaces the known scene color, the rest exposes the scene.
                // The disjoint rectangles have no material ordering ambiguity.
                const Rgba paint = painted ? painted->straight_color : Rgba{};
                const std::size_t index = (static_cast<std::size_t>(y) * width_ + x) * 4;
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    const double expected_layer = channel == 3 ? alpha : paint[channel] * alpha;
                    const double expected_real = channel == 3 ? 1 :
                        paint[channel] * alpha + scene[channel] * (1 - alpha);
                    const double real_error = std::abs(real[index + channel] - 255 * expected_real);
                    const double layer_error = std::abs(layer[index + channel] - 255 * expected_layer);
                    const double scene_error = std::abs(background[index + channel] - 255 * scene[channel]);
                    if (real_error > max_real) { max_real = real_error; worst = {x, y}; }
                    max_layer = std::max(max_layer, layer_error);
                    max_scene = std::max(max_scene, scene_error);
                }
                ++checked;
                if (alpha > 0 && alpha < 1) ++partial;
                else if (alpha == 1) ++opaque;
                else ++clear;
            }
        }
        std::printf("%s %s %ux%u checked=%zu partial=%zu opaque=%zu clear=%zu max_RGBA8_error real=%.6g ui=%.6g scene=%.6g worst=(%zu,%zu)\n",
            kTag, label, width_, height_, checked, partial, opaque, clear,
            max_real, max_layer, max_scene, worst[0], worst[1]);
        require(checked > 1000 && clear > 400, "Insufficient native pixel coverage for output oracle");
        require(max_scene <= 1.5, "SDR scene source differs from independent numeric input");
        require(max_layer <= 2.5, "UI premultiplication/coverage/removed-widget clear differs from independent paint oracle");
        // Two RGBA8 inputs plus the final store can each round once. This is
        // far below straight-alpha, double-premultiply, gamma, or stale pixels.
        require(max_real <= 2.5, "Production real-frame SDR composite differs from independent paint oracle");
    }

    filament::Engine& engine_;
    filament::Renderer* renderer_{};
    filament::Scene* scene_{};
    filament::Scene* ui_scene_{};
    filament::View* scene_view_{};
    filament::View* ui_view_{};
    filament::Camera* camera_{};
    filament::SwapChain* swapchain_{};
    filament::Skybox* scene_sky_{};
    filament::Skybox* sentinel_sky_{};
    filament::Texture* sentinel_texture_{};
    filament::RenderTarget* sentinel_target_{};
    filament::Texture* packed_{};
    filament::Material* material_{};
    filament::MaterialInstance* material_instance_{};
    filament::VertexBuffer* vertices_{};
    filament::IndexBuffer* indices_{};
    utils::Entity camera_entity_;
    std::array<utils::Entity, 3> widgets_{};
    std::array<Widget, 3> model_ = kWidgets;
    std::array<Vertex, 12> vertex_data_{};
    std::array<std::uint16_t, 18> index_data_{};
    std::unique_ptr<FrameGenerationOutput> output_;
    std::uint32_t width_ = 0, height_ = 0;
    std::size_t completed_ = 0;
};

void run(filament::Engine& engine, bool negative_clear) {
    // Warm lazy Engine-owned material/post-process objects before comparing
    // user-owned resources. The full fixture then must return to this baseline.
    {
        OutputFixture warmup(engine);
        warmup.init();
        warmup.capture("warmup");
    }
    engine.flushAndWait();
    const auto baseline = owners(engine);
    {
        OutputFixture fixture(engine);
        fixture.init();
        fixture.capture("partial-opaque-zero-alpha");
        fixture.capture("different-scene-same-ui", true, kAlternateScene);
        fixture.set_widget(0, false, kTopLeft);
        fixture.capture("removed-widget-replacement-clear", true, kScene, negative_clear);
        fixture.set_widget(0, true, kBottomLeft);
        fixture.capture("moved-widget-old-position-clear");
        fixture.capture("no-ui-clears-sdk-input", false);
        fixture.capture("ui-restored-after-no-ui");
        fixture.set_widget(0, false, kBottomLeft);
        fixture.set_widget(1, false, kWidgets[1].rect);
        fixture.set_widget(2, false, kWidgets[2].rect);
        fixture.capture("all-widgets-removed");
        fixture.set_widget(0, true, kTopLeft);
        fixture.set_widget(1, true, kWidgets[1].rect);
        fixture.set_widget(2, true, kWidgets[2].rect);
        fixture.resize(112, 80, true);
        fixture.capture("resize-while-borrowed");
        fixture.verify_reuse();
        fixture.set_widget(0, false, kTopLeft);
        fixture.capture("remove-after-reuse");
        fixture.resize(80, 64, true);
        fixture.capture("resize-back-no-ui", false);
        fixture.set_widget(0, true, kBottomLeft);
        fixture.capture("resize-back-new-widget");
        fixture.leave_borrowed_for_destruction();
        std::printf("%s completed real-frame pixel captures=%zu; generated SDK/native presentation outside this fixture\n",
            kTag, fixture.completed());
    }
    engine.flushAndWait();
    same_owners(owners(engine), baseline, "fixture cleanup");
}
} // namespace
} // namespace heaven

int main(int argc, char** argv) {
    bool negative_clear = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--negative-translucent-ui") negative_clear = true;
        else {
            std::fprintf(stderr, "Usage: %s [--negative-translucent-ui]\n", argv[0]);
            return 2;
        }
    }
    filament::Engine* engine = nullptr;
    int result = 0;
    try {
        engine = filament::Engine::create(filament::Engine::Backend::VULKAN);
        heaven::require(engine, "Vulkan Engine creation failed");
        heaven::run(*engine, negative_clear);
        // The harness expects nonzero for the injected integration fault.
        // Returning zero here would expose an oracle that missed the fault.
        std::puts(negative_clear ? "[frame_generation_output_test] NEGATIVE CONTROL NOT DETECTED" :
            "[frame_generation_output_test] PASS");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[frame_generation_output_test] FAIL: %s\n", error.what());
        result = 1;
    }
    if (engine) filament::Engine::destroy(&engine);
    return result;
}
