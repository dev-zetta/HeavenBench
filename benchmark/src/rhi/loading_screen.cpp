/*
 * HeavenBench — startup loading splash
 */
#include "rhi/loading_screen.h"
#include "rhi/hdr_output.h"
#include "engine/path_resolver.h"
#include "rhi/filament_bounds.h"
#include "rhi/materials_data.h"

#include <backend/PixelBufferDescriptor.h>

#include <filagui/ImGuiHelper.h>
#include <imgui.h>

#include <filament/RenderableManager.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/Viewport.h>

#include <math/mat4.h>
#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <utils/EntityManager.h>
#include <utils/Path.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace heaven {
namespace {

// Filagui selects its own context during command generation. Loading must not
// redirect the application's overlay/event processing to that private context.
class RestoreImGuiContext {
public:
    RestoreImGuiContext() : context_(ImGui::GetCurrentContext()) {}
    RestoreImGuiContext(const RestoreImGuiContext&) = delete;
    RestoreImGuiContext& operator=(const RestoreImGuiContext&) = delete;
    ~RestoreImGuiContext() { ImGui::SetCurrentContext(context_); }

private:
    ImGuiContext* context_;
};

} // namespace

LoadingScreen::LoadingScreen(filament::Engine& engine) : engine_(engine) {}

LoadingScreen::~LoadingScreen() {
    destroy();
}

void LoadingScreen::destroy() {
    ready_ = false;
    output_target_ = nullptr;
    if (view_) view_->setRenderTarget(nullptr);
    if (progress_view_) progress_view_->setRenderTarget(nullptr);
    auto* previous_context = ImGui::GetCurrentContext();
    if (previous_context == progress_context_) previous_context = nullptr;
    if (progress_ui_) progress_ui_.reset();
    else if (progress_context_) ImGui::DestroyContext(progress_context_);
    progress_context_ = nullptr;
    ImGui::SetCurrentContext(previous_context);
    if (progress_view_) {
        engine_.destroy(progress_view_);
        progress_view_ = nullptr;
    }
    if (quad_entity_) {
        scene_->remove(quad_entity_);
        engine_.destroy(quad_entity_);
        utils::EntityManager::get().destroy(quad_entity_);
        quad_entity_.clear();
    }
    if (camera_) {
        engine_.destroyCameraComponent(camera_entity_);
        camera_ = nullptr;
    }
    if (camera_entity_) {
        utils::EntityManager::get().destroy(camera_entity_);
        camera_entity_.clear();
    }
    if (view_) { engine_.destroy(view_); view_ = nullptr; }
    if (scene_) { engine_.destroy(scene_); scene_ = nullptr; }
    if (ib_) { engine_.destroy(ib_); ib_ = nullptr; }
    if (vb_) { engine_.destroy(vb_); vb_ = nullptr; }
    if (texture_) { engine_.destroy(texture_); texture_ = nullptr; }
    if (material_instance_) {
        engine_.destroy(material_instance_);
        material_instance_ = nullptr;
    }
    if (material_) { engine_.destroy(material_); material_ = nullptr; }
    if (renderer_) { engine_.destroy(renderer_); renderer_ = nullptr; }
}

void LoadingScreen::show(filament::SwapChain& swapchain,
                         const std::string& data_dir, std::string_view status,
                         int viewport_w, int viewport_h,
                         float progress, AssetFileSystemRef assets,
                         HdrOutput* hdr_output) {
    assets_ = assets;
    if (viewport_w <= 0 || viewport_h <= 0) return;
    if (!ensure(data_dir)) return;

    view_->setViewport({0, 0,
        static_cast<uint32_t>(viewport_w),
        static_cast<uint32_t>(viewport_h)});
    view_->setRenderTarget(output_target_);
    camera_->setProjection(filament::Camera::Projection::ORTHO,
        -1.0, 1.0, -1.0, 1.0, 0.0, 1.0);
    camera_->setModelMatrix(filament::math::mat4f{});
    update_progress_ui(status, progress, viewport_w, viewport_h);
    const auto previous_blend_mode = view_->getBlendMode();

    if (hdr_output) {
        if (!hdr_output->ensure(viewport_w, viewport_h)) {
            throw std::runtime_error("HDR loading-screen encoding could not be initialized");
        }
        hdr_output->prepare(viewport_w, viewport_h, false, true);
        hdr_output->bind_sdr_layer(*view_);
    }
    // The splash replaces/clears the SDR layer once, then the progress widgets
    // blend over it. Encoding the combined layer once preserves HDR UI colors.
    progress_view_->setRenderTarget(view_->getRenderTarget());

    if (renderer_->beginFrame(&swapchain)) {
        renderer_->render(view_);
        renderer_->render(progress_view_);
        if (hdr_output) hdr_output->encode(*renderer_);
        renderer_->endFrame();
    }
    // HdrOutput owns this target and can resize or destroy it independently.
    view_->setRenderTarget(nullptr);
    progress_view_->setRenderTarget(nullptr);
    view_->setBlendMode(previous_blend_mode);
}

Result<void> LoadingScreen::ensure(const std::string& data_dir) {
    if (ready_) return {};

    if (!renderer_) {
        renderer_ = engine_.createRenderer();
        if (!renderer_) return std::unexpected(Error("loading screen renderer setup failed"));
        renderer_->setDisplayInfo({.refreshRate = 0.0f});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
    }
    build_pipeline();
    build_progress_ui();
    if (!material_instance_ || !vb_ || !ib_ || !scene_ || !view_ || !camera_ ||
            !progress_ui_ || !progress_view_) {
        return std::unexpected(Error("loading screen pipeline setup failed"));
    }

    namespace fs = std::filesystem;
    const fs::path path = fs::path(data_dir) / std::string(kLoadingScreenRelPath);
    Result<PngImage> image = load_splash_png(path.generic_string());
    if (!image) {
        std::fprintf(stderr, "Loading screen: %s\n",
                     image.error().message.c_str());
        // Generated test scenes and incomplete asset imports still need visible
        // loading feedback. A neutral background replaces the optional artwork.
        image = PngImage{1, 1, {12, 16, 24, 255}};
    }

    upload_texture(*image);
    if (!texture_) {
        return std::unexpected(Error("loading screen texture upload failed"));
    }

    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    material_instance_->setParameter("baseColorMap", texture_, sampler);
    material_instance_->setParameter("hasTexture", true);

    ready_ = true;
    loaded_path_ = path.generic_string();
    return {};
}

void LoadingScreen::build_progress_ui() {
    if (progress_ui_) return;
    RestoreImGuiContext restore_context;
    progress_view_ = engine_.createView();
    progress_view_->setName("loading-progress");
    progress_context_ = ImGui::CreateContext();
    // CreateContext restores an existing caller context. ImGuiHelper initializes
    // the font atlas through GetIO(), so select our private context first.
    ImGui::SetCurrentContext(progress_context_);
    progress_ui_ = std::make_unique<filagui::ImGuiHelper>(
        &engine_, progress_view_, utils::Path{}, progress_context_);
    ImGui::GetIO().IniFilename = nullptr;
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.Colors[ImGuiCol_WindowBg] = {0.025f, 0.035f, 0.055f, 0.94f};
    style.Colors[ImGuiCol_PlotHistogram] = {0.08f, 0.65f, 0.95f, 1.0f};
}

void LoadingScreen::update_progress_ui(std::string_view status, float progress,
                                      int viewport_w, int viewport_h) {
    RestoreImGuiContext restore_context;
    ImGui::SetCurrentContext(progress_context_);
    progress_view_->setViewport({0, 0,
        static_cast<uint32_t>(viewport_w),
        static_cast<uint32_t>(viewport_h)});
    progress_ui_->setDisplaySize(viewport_w, viewport_h);
    const float scale = std::clamp(static_cast<float>(viewport_h) / 720.0f, 1.0f, 2.5f);
    auto& style = ImGui::GetStyle();
    style.FontScaleMain = scale;
    style.WindowPadding = {16.0f * scale, 14.0f * scale};
    style.ItemSpacing = {8.0f * scale, 8.0f * scale};
    const bool first_layout = ImGui::GetFrameCount() == 0;
    const auto draw_widgets = [&](filament::Engine*, filament::View*) {
            const float width = std::max(32.0f,
                std::min(620.0f * scale, static_cast<float>(viewport_w) - 32.0f));
            ImGui::SetNextWindowPos({static_cast<float>(viewport_w) * 0.5f,
                static_cast<float>(viewport_h) - 24.0f}, ImGuiCond_Always, {0.5f, 1.0f});
            ImGui::SetNextWindowSize({width, 0.0f}, ImGuiCond_Always);
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
            ImGui::Begin("##loading-progress", nullptr, flags);
            ImGui::TextUnformatted("Loading HeavenBench");
            if (status.empty()) status = "Preparing scene";
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(status.data(), status.data() + status.size());
            ImGui::PopTextWrapPos();
            if (std::isfinite(progress) && progress >= 0.0f) {
                ImGui::ProgressBar(std::clamp(progress, 0.0f, 1.0f),
                                   {-1.0f, 24.0f * scale});
            } else {
                ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()),
                                   {-1.0f, 24.0f * scale}, "Working...");
            }
            ImGui::End();
        };
    progress_ui_->render(1.0f / 60.0f, draw_widgets);
    // ImGui hides a new auto-sized window for its first layout frame. Settle
    // that private CPU layout once so the first GPU splash already has text
    // and progress, including accurately wrapped stage names.
    if (first_layout) progress_ui_->render(1.0f / 60.0f, draw_widgets);
}

void LoadingScreen::build_pipeline() {
    if (material_) return;

    material_ = filament::Material::Builder()
        .package(k_heaven_unlit_filamat, k_heaven_unlit_filamat_size)
        .build(engine_);
    if (!material_) return;
    material_instance_ = material_->createInstance();

    struct QuadVertex {
        filament::math::float3 position;
        filament::math::float2 uv;
        filament::math::ubyte4 color;
    };
    static constexpr std::array<QuadVertex, 4> quad_verts = {{
        {{-1, -1, -0.5f}, {0, 1}, {255, 255, 255, 255}},
        {{ 1, -1, -0.5f}, {1, 1}, {255, 255, 255, 255}},
        {{ 1,  1, -0.5f}, {1, 0}, {255, 255, 255, 255}},
        {{-1,  1, -0.5f}, {0, 0}, {255, 255, 255, 255}},
    }};
    static constexpr std::array<std::uint16_t, 6> quad_indices = {
        0, 1, 2, 0, 2, 3};

    vb_ = filament::VertexBuffer::Builder()
        .vertexCount(4)
        .bufferCount(2)
        .attribute(filament::VertexAttribute::POSITION, 0,
                   filament::VertexBuffer::AttributeType::FLOAT3,
                   0, sizeof(QuadVertex))
        .attribute(filament::VertexAttribute::UV0, 0,
                   filament::VertexBuffer::AttributeType::FLOAT2,
                   sizeof(filament::math::float3), sizeof(QuadVertex))
        .attribute(filament::VertexAttribute::COLOR, 0,
                   filament::VertexBuffer::AttributeType::UBYTE4,
                   sizeof(filament::math::float3) +
                       sizeof(filament::math::float2),
                   sizeof(QuadVertex))
        .normalized(filament::VertexAttribute::COLOR)
        .attribute(filament::VertexAttribute::TANGENTS, 1,
                   filament::VertexBuffer::AttributeType::SHORT4, 0, 0)
        .normalized(filament::VertexAttribute::TANGENTS)
        .build(engine_);
    if (!vb_) return;
    vb_->setBufferAt(engine_, 0,
        filament::VertexBuffer::BufferDescriptor(
            quad_verts.data(), sizeof(quad_verts)));
    static constexpr std::array<filament::math::short4, 4> dummy_tangents = {{
        {0, 0, 16383, 16383}, {0, 0, 16383, 16383},
        {0, 0, 16383, 16383}, {0, 0, 16383, 16383}
    }};
    vb_->setBufferAt(engine_, 1,
        filament::VertexBuffer::BufferDescriptor(dummy_tangents.data(),
                                                 sizeof(dummy_tangents)));

    ib_ = filament::IndexBuffer::Builder()
        .indexCount(6)
        .bufferType(filament::IndexBuffer::IndexType::USHORT)
        .build(engine_);
    if (!ib_) return;
    ib_->setBuffer(engine_,
        filament::IndexBuffer::BufferDescriptor(quad_indices.data(),
                                                sizeof(quad_indices)));

    scene_ = engine_.createScene();
    view_ = engine_.createView();
    camera_entity_ = utils::EntityManager::get().create();
    camera_ = engine_.createCamera(camera_entity_);
    if (!scene_ || !view_ || !camera_) return;

    quad_entity_ = utils::EntityManager::get().create();
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  vb_, ib_)
        .material(0, material_instance_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(false)
        .priority(7)
        .boundingBox(box_from_min_max({-2, -2, -2}, {2, 2, 2}))
        .build(engine_, quad_entity_);
    engine_.getTransformManager().create(quad_entity_);
    scene_->addEntity(quad_entity_);

    view_->setScene(scene_);
    view_->setCamera(camera_);
    view_->setPostProcessingEnabled(false);
    view_->setShadowingEnabled(false);
    view_->setBlendMode(filament::View::BlendMode::OPAQUE);
}

void LoadingScreen::upload_texture(const PngImage& image) {
    if (texture_) return;

    texture_ = filament::Texture::Builder()
        .width(image.width)
        .height(image.height)
        .levels(1)
        .sampler(filament::Texture::Sampler::SAMPLER_2D)
        .format(filament::Texture::InternalFormat::RGBA8)
        .build(engine_);
    if (!texture_) return;

    const size_t size = image.rgba.size();
    auto* pixels = new uint8_t[size];
    std::memcpy(pixels, image.rgba.data(), size);
    auto cleanup = [](void* p, size_t, void*) {
        delete[] static_cast<uint8_t*>(p);
    };
    texture_->setImage(engine_, 0,
        filament::Texture::PixelBufferDescriptor(
            pixels, size, filament::Texture::Format::RGBA,
            filament::Texture::Type::UBYTE, cleanup));
}

Result<PngImage>
LoadingScreen::load_splash_png(const std::string& path) {
    Result<PngImage> decoded = load_png_rgba8(path, assets_);
    if (!decoded) return decoded;

    uint32_t panel_h = decoded->height;
    size_t panel_src_y = 0;
    if (decoded->height >= decoded->width && (decoded->height % 2u) == 0u) {
        panel_h = decoded->height / 2u;
        panel_src_y = panel_h;
    }

    PngImage image;
    image.width = decoded->width;
    image.height = panel_h;
    image.rgba.resize(size_t(decoded->width) * panel_h * 4u);
    const size_t stride = size_t(decoded->width) * 4u;
    for (uint32_t y = 0; y < panel_h; ++y) {
        const uint8_t* src = decoded->rgba.data() + (panel_src_y + y) * stride;
        uint8_t* dst = image.rgba.data() + size_t(y) * stride;
        std::memcpy(dst, src, stride);
    }
    return image;
}

} // namespace heaven
