/*
 * HeavenBench — startup loading splash
 *
 * Renders the HeavenBench loading image before the full scene is ready.
 */
#ifndef HEAVENBENCH_SRC_RHI_LOADING_SCREEN_H_
#define HEAVENBENCH_SRC_RHI_LOADING_SCREEN_H_

#include "engine/types.h"
#include "rhi/png_image.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>

#include <utils/Entity.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ImGuiContext;
namespace filagui { class ImGuiHelper; }

namespace heaven {

class HdrOutput;

class LoadingScreen {
public:
    explicit LoadingScreen(filament::Engine& engine);
    ~LoadingScreen();

    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    void show(filament::SwapChain& swapchain,
              const std::string& data_dir, std::string_view status,
              int viewport_w, int viewport_h,
              float progress = -1.0f,
              AssetFileSystemRef assets = std::nullopt,
              HdrOutput* hdr_output = nullptr);
    void destroy();

    // Optional caller-owned SDR capture target. Detach before retiring it;
    // show() never retains its attachment on either View between frames.
    // HDR output retains the separate HdrOutput encoding/capture contract.
    void set_output_target(filament::RenderTarget* target) { output_target_ = target; }

private:
    [[nodiscard]] Result<void> ensure(const std::string& data_dir);
    [[nodiscard]] Result<PngImage> load_splash_png(const std::string& path);
    void build_pipeline();
    void build_progress_ui();
    void update_progress_ui(std::string_view status, float progress,
                            int viewport_w, int viewport_h);
    void upload_texture(const PngImage& image);

    filament::Engine&    engine_;
    // UI frames must not advance the benchmark renderer's frame/fence history.
    filament::Renderer*  renderer_ = nullptr;

    filament::Material*         material_ = nullptr;
    filament::MaterialInstance* material_instance_ = nullptr;
    filament::Texture*          texture_ = nullptr;
    filament::VertexBuffer*     vb_ = nullptr;
    filament::IndexBuffer*      ib_ = nullptr;
    filament::Scene*            scene_ = nullptr;
    filament::View*             view_ = nullptr;
    filament::Camera*           camera_ = nullptr;
    filament::View*             progress_view_ = nullptr;
    std::unique_ptr<filagui::ImGuiHelper> progress_ui_;
    ImGuiContext*               progress_context_ = nullptr;
    filament::RenderTarget*     output_target_ = nullptr;
    utils::Entity               camera_entity_;
    utils::Entity               quad_entity_;
    bool                        ready_ = false;
    std::string                 loaded_path_;
    AssetFileSystemRef          assets_;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_LOADING_SCREEN_H_
