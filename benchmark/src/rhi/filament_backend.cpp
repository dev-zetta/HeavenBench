#include "benchmark/microprofiler.h"
#include "benchmark/input_response.h"
/*
 * HeavenBench — Filament RHI Backend implementation
 *
 * Implements the IRhiBackend interface using Google's Filament rendering engine.
 * This replaces the hand-rolled OpenGL 4.5 multi-pass pipeline with Filament's
 * built-in PBR renderer, shadow mapping, and post-processing.
 */
#include "rhi/filament_backend.h"
#include "rhi/filament_util.h"
#include "rhi/hdr_surface_formats.h"
#include "rhi/screenshot_ppm.h"
#include "rhi/screenshot_hdr.h"
#include "rhi/features/cloud_feature.h"
#include "rhi/features/volumetric_cloud_feature.h"
#include "rhi/features/godrays_feature.h"
#include "rhi/features/ssgi_feature.h"
#include "rhi/features/volfog_feature.h"
#include "rhi/features/grass_feature.h"
#include "rhi/features/massive_foliage_feature.h"
#include "rhi/features/sky_feature.h"
#include "rhi/features/particle_feature.h"
#include "rhi/features/lens_flare_feature.h"
#include "rhi/features/wind_arrows_feature.h"
#include "rhi/features/rain_feature.h"
#include "rhi/features/wet_reflections_feature.h"
#include "rhi/features/advanced_materials_feature.h"
#include "rhi/post_process.h"
#include "rhi/scene_displacement_preparation.h"
#include "rhi/render_test_scene_assets.h"
#include "engine/render_test_scene_catalog.h"
#include "rhi/frame_timing.h"
#include "rhi/frame_generation/frame_generation_runtime.h"
#include "rhi/frame_generation/temporal_effects.h"
#if HEAVEN_HAS_FSR3
#include "rhi/frame_generation/frame_generation_platform.h"
#endif
#if HEAVEN_HAS_DLSS
#include "rhi/frame_generation/dlss_platform.h"
#endif
#include "rhi/profile_bridge.h"
#include "engine/render_presets.h"
#include "engine/startup_timer.h"
#ifdef __APPLE__
#include "rhi/macos_surface.h"
#endif

#include <filament/Engine.h>
#include <filament/FrameHistoryStream.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Camera.h>
#include <filament/SwapChain.h>
#include <filament/Viewport.h>
#include <filament/Texture.h>
#include <filament/ColorGrading.h>
#include <filament/Options.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>
#include <utils/Path.h>
#include <utils/CString.h>

// GPU / render-API info. VulkanPlatform exposes the
// selected VkPhysicalDevice / VkInstance and pulls in BlueVK (Filament's
// Vulkan loader), giving us the vkGetPhysicalDevice* entry points used by
// gpu_info() on the Vulkan path. Platform.h provides the DeviceInfoType
// query used on the OpenGL fallback. This is the only TU that reads
// GPU-API specifics (AGENTS.md).
#include <backend/Platform.h>
#include <backend/PixelBufferDescriptor.h>
#include <backend/platforms/VulkanPlatform.h>

#include <math/mat4.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

// Dear ImGui + filagui overlay. filagui::ImGuiHelper translates ImGui draw
// data into a Filament View/Scene; imgui.h gives us the immediate-mode API
// and the input-queue (io.AddMouse*/AddKeyEvent) used by overlay_handle_event.
#include <filagui/ImGuiHelper.h>
#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <bit>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <utility>

namespace heaven {

struct FilamentBackend::ScreenshotReadback {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    std::vector<float> rgba_pq;
    std::atomic_bool complete = false;
};

Result<void> FilamentBackend::request_screenshot(const std::filesystem::path& path) {
    if (!initialized()) {
        return std::unexpected(Error("Screenshot capture requires an initialized renderer"));
    }
    if (engine_->getBackend() == filament::Engine::Backend::NOOP) {
        return std::unexpected(Error("Screenshot capture requires a graphics backend"));
    }
    if (screenshot_path_) {
        return std::unexpected(Error("A screenshot request is already pending"));
    }
    if (path.empty()) {
        return std::unexpected(Error("Screenshot output path is empty"));
    }
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) return std::unexpected(Error("Cannot resolve screenshot output: " + error.message()));
    std::filesystem::create_directories(absolute.parent_path(), error);
    if (error) return std::unexpected(Error("Cannot create screenshot directory: " + error.message()));
    if (std::filesystem::is_directory(absolute, error)) {
        return std::unexpected(Error("Screenshot output names a directory"));
    }
    screenshot_path_ = std::move(absolute);
    screenshot_result_ = false;
    return {};
}

Result<bool> FilamentBackend::screenshot_complete() const {
    return screenshot_result_;
}

glm::vec2 FilamentBackend::render_resolution_scale() const {
    if (!view_) return {1.0f, 1.0f};
    const auto scale = view_->getLastDynamicResolutionScale();
    return {scale.x, scale.y};
}

void FilamentBackend::set_capture_time(std::optional<float> seconds) {
    using Clock = std::chrono::steady_clock;
    const double limit = std::chrono::duration<double>(Clock::duration::max()).count() / 2.0;
    if (seconds && (!std::isfinite(*seconds) || *seconds < 0.0f || *seconds > limit)) {
        throw std::invalid_argument("Capture time must be finite, nonnegative and representable");
    }
    capture_time_ = seconds;
}

uint64_t FilamentBackend::capture_frame_timestamp() {
    if (!capture_time_) return 0;
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(*capture_time_));
    // Supplying the same timestamp to beginFrame makes Filament's built-in
    // shader clock agree with FrameContext even when CPU / GPU work is slow.
    renderer_->setMaterialTimeEpoch(now - elapsed);
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count());
}

void FilamentBackend::destroy_screenshot_target() {
    if (screenshot_hdr_target_) {
        if (hdr_output_) hdr_output_->set_output_target(nullptr);
        engine_->destroy(screenshot_hdr_target_);
        screenshot_hdr_target_ = nullptr;
    }
    if (screenshot_hdr_texture_) {
        engine_->destroy(screenshot_hdr_texture_);
        screenshot_hdr_texture_ = nullptr;
    }
    screenshot_hdr_width_ = 0;
    screenshot_hdr_height_ = 0;
}

void FilamentBackend::prepare_screenshot_target(int viewport_w, int viewport_h) {
    if (!screenshot_path_ || !hdr_) return;
    if (screenshot_hdr_target_ && screenshot_hdr_width_ == viewport_w &&
        screenshot_hdr_height_ == viewport_h) return;
    destroy_screenshot_target();
    try {
        if (!hdr_active_ || !hdr_output_ || !hdr_output_->valid() ||
            viewport_w <= 0 || viewport_h <= 0) {
            throw std::runtime_error("HDR capture requires a valid composed HDR output");
        }
        using Texture = filament::Texture;
        screenshot_hdr_texture_ = Texture::Builder()
            .width(static_cast<uint32_t>(viewport_w))
            .height(static_cast<uint32_t>(viewport_h))
            .levels(1).sampler(Texture::Sampler::SAMPLER_2D)
            .format(Texture::InternalFormat::RGBA16F)
            .usage(Texture::Usage::COLOR_ATTACHMENT | Texture::Usage::BLIT_SRC)
            .build(*engine_);
        if (!screenshot_hdr_texture_) {
            throw std::runtime_error("Cannot allocate HDR screenshot texture");
        }
        screenshot_hdr_target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR, screenshot_hdr_texture_)
            .build(*engine_);
        if (!screenshot_hdr_target_) {
            throw std::runtime_error("Cannot allocate HDR screenshot target");
        }
        screenshot_hdr_width_ = viewport_w;
        screenshot_hdr_height_ = viewport_h;
    } catch (const std::exception& error) {
        screenshot_result_ = std::unexpected(Error(
            std::string("HDR screenshot target failed: ") + error.what()));
        screenshot_path_.reset();
        destroy_screenshot_target();
    }
}

void FilamentBackend::submit_screenshot(int viewport_w, int viewport_h) {
    if (!screenshot_path_ || screenshot_readback_) return;
    const auto size = screenshot_rgba_size(viewport_w, viewport_h);
    if (!size) {
        screenshot_result_ = std::unexpected(size.error());
        screenshot_path_.reset();
        return;
    }
    try {
        auto readback = std::make_shared<ScreenshotReadback>();
        readback->width = viewport_w;
        readback->height = viewport_h;
        if (hdr_) {
            // NaNs turn an unsupported / empty FLOAT transfer into an explicit
            // error instead of publishing an apparently successful black image.
            readback->rgba_pq.resize(*size, std::numeric_limits<float>::quiet_NaN());
        } else {
            readback->rgba.resize(*size);
        }
        // The callback owns an additional shared reference: even an exception
        // during flush / teardown cannot free memory still used by the driver.
        auto callback_owner = std::make_unique<std::shared_ptr<ScreenshotReadback>>(readback);
        filament::backend::PixelBufferDescriptor buffer(
            hdr_ ? static_cast<void*>(readback->rgba_pq.data())
                 : static_cast<void*>(readback->rgba.data()),
            hdr_ ? readback->rgba_pq.size() * sizeof(float) : readback->rgba.size(),
            filament::backend::PixelDataFormat::RGBA,
            hdr_ ? filament::backend::PixelDataType::FLOAT : filament::backend::PixelDataType::UBYTE,
            [](void*, std::size_t, void* user) {
                const std::unique_ptr<std::shared_ptr<ScreenshotReadback>> owner(
                    static_cast<std::shared_ptr<ScreenshotReadback>*>(user));
                (*owner)->complete.store(true, std::memory_order_release);
            }, callback_owner.release());
        screenshot_readback_ = std::move(readback);
        if (hdr_) {
            // Reuse the exact composed layers and encode material. A float
            // destination also supports HDR displays whose swapchain is packed
            // 10-bit (Filament cannot read that format back as FLOAT directly).
            hdr_output_->set_output_target(screenshot_hdr_target_);
            hdr_output_->encode();
            hdr_output_->set_output_target(nullptr);
            renderer_->readPixels(screenshot_hdr_target_, 0, 0,
                static_cast<uint32_t>(viewport_w), static_cast<uint32_t>(viewport_h),
                std::move(buffer));
        } else {
            renderer_->readPixels(0, 0, static_cast<uint32_t>(viewport_w),
                static_cast<uint32_t>(viewport_h), std::move(buffer));
        }
    } catch (const std::exception& error) {
        if (hdr_output_) hdr_output_->set_output_target(nullptr);
        screenshot_result_ = std::unexpected(Error(
            std::string("Screenshot readback failed: ") + error.what()));
        screenshot_path_.reset();
    }
}

void FilamentBackend::finish_screenshot() {
    if (!screenshot_readback_) return;
    try {
        engine_->flushAndWait();
        if (screenshot_path_) {
            if (!screenshot_readback_->complete.load(std::memory_order_acquire)) {
                screenshot_result_ = std::unexpected(Error("Screenshot readback did not complete"));
            } else {
                const auto& readback = *screenshot_readback_;
                const auto written = hdr_
                    ? write_screenshot_hdr(*screenshot_path_, readback.width, readback.height,
                        readback.rgba_pq, config_.hdr_paper_white_nits)
                    : write_screenshot_ppm(*screenshot_path_, readback.width, readback.height,
                        readback.rgba);
                screenshot_result_ = written ? Result<bool>(true)
                    : Result<bool>(std::unexpected(written.error()));
            }
        }
    } catch (const std::exception& error) {
        screenshot_result_ = std::unexpected(Error(
            std::string("Screenshot completion failed: ") + error.what()));
    }
    screenshot_readback_.reset();
    screenshot_path_.reset();
    destroy_screenshot_target();
}

// ── Helper: extract native window handle from SDL ───────────────

// Returns a pointer to a statically-stored tagged struct that our
// patched Filament VulkanPlatformLinux understands. The tag's first
// 4 bytes tell the platform whether the rest of the struct holds
// Wayland (display+surface) or X11 (window id) data, so a single
// binary can drive either windowing system at runtime.

// The struct layout MUST stay in sync with `UgxLinuxNativeWindow`
// in lib/filament/.../vulkan/platform/VulkanPlatformLinux.cpp.

#if defined(__linux__) && !defined(__ANDROID__)
namespace {
constexpr uint32_t kUgxNativeWaylandTag = 0x57414c44u; // 'WALD'
constexpr uint32_t kUgxNativeX11Tag     = 0x58313158u; // 'X11X'
struct UgxLinuxNativeWindow {
    uint32_t       tag;
    struct wl_display* wl_display;
    struct wl_surface* wl_surface;
    uint32_t       width;
    uint32_t       height;
    unsigned long  x11_window;
};
} // anonymous namespace
#endif

void* FilamentBackend::get_native_window(SDL_Window* sdl_window) {
#ifdef __APPLE__
    if (!macos_surface_) {
        macos_surface_ = std::make_unique<MacOsSurface>(sdl_window);
    }
    return macos_surface_->native_layer();
#else
    SDL_SysWMinfo wmi;
    SDL_VERSION(&wmi.version);
    if (!SDL_GetWindowWMInfo(sdl_window, &wmi)) {
        std::fprintf(stderr, "FilamentBackend: SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return nullptr;
    }
#ifdef __ANDROID__
    if (wmi.subsystem == SDL_SYSWM_ANDROID) {
        std::printf("FilamentBackend: native window = Android\n");
        return reinterpret_cast<void*>(wmi.info.android.window);
    }
    std::fprintf(stderr, "FilamentBackend: unsupported SDL subsystem %d\n",
                 static_cast<int>(wmi.subsystem));
    return nullptr;
#elifdef __linux__
    // Static storage so the pointer stays valid for the program
    // lifetime (Filament keeps the native-window pointer around and
    // re-reads the extent on swap-chain recreation).
    static UgxLinuxNativeWindow native{};

    if (wmi.subsystem == SDL_SYSWM_X11) {
        if (engine_->getBackend() == filament::Engine::Backend::OPENGL) {
            // GLX consumes the XID itself, not our Vulkan dual-WSI structure.
            return std::bit_cast<void*>(wmi.info.x11.window);
        }
        native.tag        = kUgxNativeX11Tag;
        native.x11_window = static_cast<unsigned long>(wmi.info.x11.window);
        std::printf("FilamentBackend: native window = X11 (window=0x%lx)\n",
                    native.x11_window);
        return &native;
    }
    #if SDL_VERSION_ATLEAST(2, 0, 17)
    if (wmi.subsystem == SDL_SYSWM_WAYLAND) {
        if (engine_->getBackend() == filament::Engine::Backend::OPENGL) {
            std::fprintf(stderr,
                "FilamentBackend: OpenGL requires an X11 window on Linux\n");
            return nullptr;
        }
        int w = 0, h = 0;
        // The tagged native handle is consumed by our dual-WSI Filament patch
        // as the Vulkan surface extent. Wayland window coordinates are logical
        // units, but Vulkan swap-chain extents are physical drawable pixels.
        // Using SDL_GetWindowSize here made the swap chain undersized by the
        // compositor scale (2x on the affected SDR setup).
        SDL_Vulkan_GetDrawableSize(sdl_window, &w, &h);
        native.tag        = kUgxNativeWaylandTag;
        native.wl_display = wmi.info.wl.display;
        native.wl_surface = wmi.info.wl.surface;
        native.width      = static_cast<uint32_t>(w);
        native.height     = static_cast<uint32_t>(h);
        std::printf("FilamentBackend: native window = Wayland (%dx%d)\n", w, h);
        return &native;
    }
    #endif
    std::fprintf(stderr, "FilamentBackend: unsupported SDL subsystem %d\n",
                 static_cast<int>(wmi.subsystem));
    return nullptr;
#elifdef _WIN32
    return reinterpret_cast<void*>(wmi.info.win.window);
#else
    return nullptr;
#endif
#endif
}

// ── Helper: convert glm::mat4 to filament::math::mat4f ─────────
// (now shared via rhi/filament_util.h — glm_to_filament)

// ── Constructor / Destructor ────────────────────────────────────

FilamentBackend::FilamentBackend(SDL_Window* window, const AppConfig& config)
    : window_(window), hdr_(config.hdr)
{
    config_ = config;
    // Create Filament engine — prefer Vulkan for best performance on modern GPUs
    filament::Engine::Config engine_config;
    engine_config.driverHandleArenaSizeMB = 32;
    // Reduce command buffer depth for lower input latency
    // Default is 3 MB (3 frames). Use 1 MB (1 frame) for minimal latency.
    engine_config.commandBufferSizeMB = 2;
    engine_config.minCommandBufferSizeMB = 1;
    // Per-render-pass arena and per-frame high-level command buffer.

    // Point/spot-light shadows render a full-scene depth pass *per shadow-casting
    // light* (cube maps cost 6 faces each). With the default shadow-light budget
    // that adds dozens of extra render passes per frame, each emitting its own
    // batch of high-level draw commands. Filament allocates those commands from
    // the per-frame command buffer, which in turn lives inside the per-render-pass
    // arena. With the stock sizes (arena 3 MiB, per-frame commands 2 MiB) the
    // extra shadow passes overflow the arena, which Filament reports as
    // "RenderPass arena is full, using slower system heap". That heap fallback
    // stalls the render thread (camera lag at high FPS) and the command-buffer
    // pressure surfaces as shadow/light flicker. Sizing the arena up so the
    // shadow passes fit removes the fallback; leave >=1 MiB headroom in the arena
    // beyond perFrameCommandsSizeMB as Filament requires.
    engine_config.perRenderPassArenaSizeMB = 16;
    engine_config.perFrameCommandsSizeMB   = 12;

    uint32_t vulkan_window_flags = SDL_WINDOW_VULKAN;
#ifdef __APPLE__
    // SDL_Metal_CreateView converts the window flag after the first engine is
    // created. Reinitializing a backend for that same layer still uses Vulkan.
    vulkan_window_flags |= SDL_WINDOW_METAL;
#endif
    const bool vulkan_window = (SDL_GetWindowFlags(window) & vulkan_window_flags) != 0;
    if (vulkan_window) {
        filament::backend::Platform* presentation_platform = nullptr;
#if HEAVEN_HAS_FSR3
        frame_generation_platform_ = std::make_unique<FrameGenerationPlatform>();
        presentation_platform = frame_generation_platform_.get();
#endif
#if HEAVEN_HAS_DLSS
        if (config_.frame_generation == FrameGenerationMode::Dlss) {
            DlssBootstrapOptions options;
            if (const char* directory = std::getenv("HEAVEN_DLSS_PLUGIN_DIR"))
                options.plugin_directory = directory;
            if (const char* project = std::getenv("HEAVEN_DLSS_PROJECT_ID"))
                options.project_id = project;
            options.engine_version = HEAVENBENCH_VERSION;
            dlss_platform_ = std::make_unique<DlssPlatform>(options);
            presentation_platform = dlss_platform_.get();
        }
#endif
        engine_ = filament::Engine::Builder()
            .backend(filament::Engine::Backend::VULKAN)
            .platform(presentation_platform)
            .config(&engine_config)
            .build();
    }

#ifndef __APPLE__
    if (!engine_ && !hdr_) {
        // Fall back to OpenGL if Vulkan isn't available
        std::printf("FilamentBackend: Vulkan not available, falling back to OpenGL\n");
        engine_ = filament::Engine::Builder()
            .backend(filament::Engine::Backend::OPENGL)
            .config(&engine_config)
            .build();
    }
#endif

    if (!engine_) {
        std::fprintf(stderr, "FilamentBackend: Failed to create Filament engine\n");
        return;
    }

    if (hdr_) {
        if (auto supported = validate_hdr_surface(); !supported) {
            initialization_error_ = supported.error().message;
            return;
        }
    }

    // Create swap chain from native window
    void* native_win = get_native_window(window);
    if (!native_win) {
        initialization_error_ = std::format(
            "Cannot create the native rendering surface: {}", SDL_GetError());
        std::fprintf(stderr, "FilamentBackend: Failed to get native window handle\n");
        return;
    }
    // Create swap chain from native window. When HDR output is requested, ask
    // for an HDR10 (PQ / ST.2084, Rec.2020) swap chain; the Vulkan backend
    // requires a high-precision surface format with the HDR10 colorspace.
    // The app preflight above reports ordinary unsupported-display errors;
    // the backend guard also rejects a later compositor mode-change race.
    uint64_t swapchain_flags = 0;
    if (hdr_) {
        swapchain_flags |= filament::SwapChain::CONFIG_HDR10_ST2084;
    }
    swapchain_ = engine_->createSwapChain(native_win, swapchain_flags);

    // Create renderer
    renderer_ = engine_->createRenderer();
    gpu_history_stream_ =
        std::make_unique<filament::FrameHistoryStream>(renderer_);

    // Start unpaced; adaptive rendering configures a finite controller budget
    // when pipelines are ready. Swapchain presentation stays immediate.
    configure_frame_timing(*renderer_, false);

    // Create scene
    scene_ = engine_->createScene();

    // Create view
    view_ = engine_->createView();
    view_->setScene(scene_);

    // Create camera
    camera_entity_ = utils::EntityManager::get().create();
    fil_camera_ = engine_->createCamera(camera_entity_);
    view_->setCamera(fil_camera_);

    // Enable post-processing by default. We use Filament's built-in
    // temporal AA (configured later in setup_post_processing) instead
    // of FXAA: TAA tames the foliage / grass shimmer dramatically and
    // also sharpens texturing via its negative LOD bias. FXAA is left
    // off because the two stack visibly, blurring textures without
    // adding extra anti-aliasing benefit.
    view_->setPostProcessingEnabled(true);
    view_->setAntiAliasing(filament::View::AntiAliasing::NONE);
    // Filament's color-grading dither assumes an 8-bit display-code target.
    // HDR here writes linear Rec.2020 into fp16 before a separate PQ encode;
    // adding 1/255 noise in linear light creates visible near-black grain.
    // Keep SDR display dithering, and retain full precision in the HDR target.
    view_->setDithering(hdr_ ? filament::View::Dithering::NONE
                            : filament::View::Dithering::TEMPORAL);

    std::printf("FilamentBackend: Engine created (%s)\n",
                engine_->getBackend() == filament::Engine::Backend::VULKAN ? "Vulkan" : "OpenGL");

    // Log the actual backend for debugging GPU selection
    if (engine_->getBackend() == filament::Engine::Backend::VULKAN) {
        std::printf("FilamentBackend: Vulkan device selected (use VK_ICD_FILENAMES or MESA_VK_DEVICE_SELECT to override)\n");
    }

    // Texture store needs the engine; create it now that the engine
    // exists so load_texture() works for any caller (the scene loader
    // pulls textures in before init_pipelines()).
    texture_store_ = std::make_unique<TextureStore>(*engine_);

    // Mesh store likewise needs the engine; upload_mesh() is called
    // during scene load before init_pipelines().
    mesh_store_ = std::make_unique<MeshStore>(*engine_);

    // Material instance cache depends on the texture store (for texture
    // handle resolution + placeholders). Base materials are loaded later
    // in init_pipelines().
    mat_cache_ = std::make_unique<MaterialInstanceCache>(*engine_, *texture_store_);

    loading_screen_ =
        std::make_unique<LoadingScreen>(*engine_);
    // The loading splash appears before init_pipelines. It needs the same
    // HDR transfer function and caller-specified luminance as the scene/UI.
    if (hdr_) {
        core_ = {engine_, scene_, view_, renderer_, fil_camera_};
        hdr_output_ = std::make_unique<HdrOutput>(core_);
        hdr_output_->set_luminance(config_.hdr_paper_white_nits, config_.hdr_peak_nits);
    }
}

Result<void> FilamentBackend::validate_hdr_surface() const {
    if (!engine_ || engine_->getBackend() != filament::Engine::Backend::VULKAN) {
        return std::unexpected(Error("--hdr requires the Vulkan renderer"));
    }
    auto* platform = static_cast<filament::backend::VulkanPlatform*>(engine_->getPlatform());
    if (!platform || !bluevk::vkGetPhysicalDeviceSurfaceFormatsKHR ||
        !bluevk::vkDestroySurfaceKHR) {
        return std::unexpected(Error("HDR surface query is unavailable"));
    }
    struct ProbeSurface {
        VkInstance instance;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        ~ProbeSurface() {
            if (surface != VK_NULL_HANDLE) {
                bluevk::vkDestroySurfaceKHR(instance, surface, nullptr);
            }
        }
    } probe{platform->getInstance()};
    if (!SDL_Vulkan_CreateSurface(window_, probe.instance, &probe.surface)) {
        return std::unexpected(Error(std::format(
            "HDR surface probe failed: {}", SDL_GetError())));
    }
    return require_hdr10_surface([&](std::uint32_t* count, VkSurfaceFormatKHR* formats) {
        return bluevk::vkGetPhysicalDeviceSurfaceFormatsKHR(
            platform->getPhysicalDevice(), probe.surface, count, formats);
    });
}

void FilamentBackend::set_trace_sink(profile::Recorder* recorder) {
    if (recorder && !profile_bridge_) profile_bridge_ = std::make_unique<ProfileBridge>(*recorder);
    if (!recorder) profile_bridge_.reset();
}

void FilamentBackend::set_trace_frame_context(const profile::Context& context) {
    if (!profile_bridge_) return;
    profile_bridge_->update(context);
    if (renderer_) renderer_->setProfilerContext(utils::profiling::getContext());
}

TraceCapabilities FilamentBackend::trace_capabilities() const {
    return profile_bridge_ ? profile_bridge_->capabilities() : TraceCapabilities{};
}

void FilamentBackend::set_input_response_context(InputResponseRecorder* recorder,
        uint64_t attempt, uint64_t submission) {
    input_response_ = recorder;
    input_response_attempt_ = attempt;
    input_response_submission_ = submission;
}

FilamentBackend::~FilamentBackend() {
    if (!engine_) return;
    if (input_response_) input_response_->retire_pending("backend destroyed");
    engine_->flushAndWait();
    destroy_frame_generation();

    // Usually finish_screenshot already drained the readback. A skipped frame
    // may still own an unused capture target; detach it before HDR teardown.
    if (screenshot_readback_) finish_screenshot();
    destroy_screenshot_target();

    gpu_history_stream_.reset();
    loading_screen_.reset();

    // Tear down the overlay first: the filagui helper frees its own
    // Scene/Camera/Renderable/textures + ImGui context in its destructor,
    // so reset it (and then the backend-owned overlay View) before the
    // engine is destroyed.
    imgui_helper_.reset();
    if (overlay_view_) {
        engine_->destroy(overlay_view_);
        overlay_view_ = nullptr;
    }

    // Tear down modular render features first — each frees its own
    // GPU resources (entities, material instances, buffers).
    destroy_features();
    if (view_) {
        auto bloom = view_->getBloomOptions();
        bloom.dirt = nullptr;
        bloom.dirtStrength = 0.0f;
        view_->setBloomOptions(bloom);
    }
    render_ctx_.test_scene_assets = nullptr;
    render_test_assets_.reset();

    // Destroy textures — owned by texture_store_, freed when it is
    // reset before the engine is destroyed (below).

    // Destroy mesh resources — owned by mesh_store_, freed when it is
    // reset before the engine is destroyed (below).

    // Destroy scene entities FIRST (they reference material instances),
    // THEN the material cache (instances + base materials).
    if (scene_builder_) scene_builder_->clear(core_);
    mat_cache_.reset();

    // Depth pre-pass resources owned by the DepthPrePass subsystem;
    // freed when depth_prepass_ is destroyed at the end of this dtor.

    // Destroy the one post-process object the backend still owns.
    if (view_) view_->setColorGrading(nullptr);
    color_grading_cache_ = {};
    if (color_grading_) engine_->destroy(color_grading_);
    color_grading_ = nullptr;

    // Sun / moon / skybox / IBL cubemap are owned by the LightingSystem
    // subsystem; reset it before the engine is destroyed.
    lighting_.reset();

    // Scene point/spot lights (emission-derived + world-authored) are
    // owned by the light spawner; destroy them, then reset it.
    if (light_spawner_) light_spawner_->destroy(core_);
    light_spawner_.reset();

    // Depth pre-pass subsystem — frees its View/Camera/RT/textures in
    // its own destructor. Reset before the engine is destroyed.
    // First tear down the depth-proxy renderables (they live in the proxy
    // Scene and reference the shared depth MaterialInstance, both owned by
    // depth_prepass_), so they are destroyed while the engine + proxy Scene
    // are still alive.
    if (depth_proxy_builder_ && depth_prepass_) {
        depth_proxy_builder_->clear(core_, depth_prepass_->proxy_scene());
    }
    depth_proxy_builder_.reset();
    depth_prepass_.reset();

    // Scene-colour capture subsystem — frees its capture/history RTs/textures +
    // blit pipeline in its own destructor. Reset before the engine is destroyed.
    scene_color_capture_.reset();

    // HDR output encode subsystem — frees its fp16 target + encode pipeline in
    // its own destructor. Reset before the engine is destroyed.
    if (view_) view_->setRenderTarget(nullptr);
    hdr_output_.reset();

    // Texture store — frees all textures + placeholders in its own
    // destructor. Reset before the engine is destroyed.
    texture_store_.reset();

    // Mesh store — frees all VB/IB in its own destructor. Reset before
    // the engine is destroyed.
    mesh_store_.reset();

    // Destroy core objects
    if (camera_entity_) {
        engine_->destroyCameraComponent(camera_entity_);
        utils::EntityManager::get().destroy(camera_entity_);
    }

    engine_->destroy(view_);
    engine_->destroy(scene_);
    engine_->destroy(renderer_);
    engine_->destroy(swapchain_);

    filament::Engine::destroy(&engine_);
#if HEAVEN_HAS_FSR3
    frame_generation_platform_.reset();
#endif
#if HEAVEN_HAS_DLSS
    dlss_platform_.reset();
#endif
    profile_bridge_.reset();
#ifdef __APPLE__
    // The Vulkan surface borrows the layer. Drop SDL's owning view only after
    // Filament's driver has drained and destroyed every native swap chain.
    macos_surface_.reset();
#endif
}

// ── Identification ──────────────────────────────────────────────

std::string FilamentBackend::device_name() const {
    return "Filament (" + render_api_name() + ")";
}

std::string FilamentBackend::api_version() const {
    return "Filament PBR Engine (" + render_api_name() + " backend)";
}

RendererFeatureStats FilamentBackend::renderer_feature_stats() const {
    RendererFeatureStats out;
    if (view_) {
        const auto bloom = view_->getBloomOptions();
        out.bloom_enabled = bloom.enabled;
        out.bloom_strength = bloom.strength;
    }
    out.ibl_prefiltered =
        lighting_ && lighting_->prefiltered_environment();
    out.msaa_requested = config_.features.msaa;
    out.msaa_samples = msaa_resolved_samples_;
    out.msaa_active = config_.features.msaa && msaa_resolved_samples_ > 1;
    out.custom_msaa_resolve = out.msaa_active && msaa_resolved_samples_ >= 4;
    out.alpha_to_coverage = out.msaa_active;
    out.bent_normals = true;
    out.multi_bounce_ao = true;
    out.gpu_instancing = config_.features.gpu_instancing;
    out.gpu_skinning = config_.features.gpu_skinning;
    out.refraction_requested = config_.features.refraction;
    out.refraction_active =
        config_.features.refraction && mat_cache_ &&
        mat_cache_->refraction_enabled();
    out.ao_quality = config_.quality.ao_quality;
    out.sun_shadow_map_size = config_.quality.sun_shadow_map_size;
    out.point_shadow_map_size = config_.quality.point_shadow_map_size;
    out.material_workflow_requested = config_.material_workflow;
    out.material_workflow_active = config_.material_workflow;
    if (mat_cache_) {
        out.compatibility_workflow_surfaces =
            mat_cache_->compatibility_surface_count();
        out.experimental_workflow_surfaces =
            mat_cache_->experimental_surface_count();
        out.compatibility_workflow_instances =
            mat_cache_->compatibility_instance_count();
        out.experimental_workflow_instances =
            mat_cache_->experimental_instance_count();
        out.material_bindings = mat_cache_->material_binding_count();
    }
    if (texture_store_) {
        const auto& stats = texture_store_->stats();
        out.srgb_textures = stats.srgb;
        out.legacy_color_textures = stats.legacy_color;
        out.scalar_textures = stats.scalar;
        out.normal_textures = stats.normal;
        out.hdr_textures = stats.hdr;
        out.texture_fallbacks = stats.fallbacks;
        out.hires_textures = stats.hires;
        out.original_textures = stats.originals;
        out.texture_missing_variant_fallbacks = stats.missing_variants;
        out.texture_unsupported_format_fallbacks = stats.unsupported_formats;
        out.texture_invalid_variant_fallbacks = stats.invalid_variants;
        out.selected_texture_bytes = stats.selected_mip_bytes;
        out.effective_texture_max_dimension = stats.effective_max_dimension;
        out.texture_quality_requested = stats.requested_quality;
        out.texture_quality_active = stats.active_quality;
    }
    out.enabled_render_features = static_cast<uint64_t>(std::count_if(
        render_features_.begin(), render_features_.end(),
        [](const auto& feature) { return feature && feature->enabled(); }));
    out.total_render_features = render_features_.size();
    return out;
}

// ── GPU / render-API info  ──────────────────────

namespace {

std::pair<DriverFamily, DriverVersionEncoding> vulkan_driver_identity(VkDriverId id) {
    using enum DriverFamily;
    using Encoding = DriverVersionEncoding;
    switch (id) {
        case VK_DRIVER_ID_NVIDIA_PROPRIETARY: return {Nvidia, Encoding::Nvidia};
        case VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS: return {Intel, Encoding::IntelWindows};
        case VK_DRIVER_ID_MESA_NVK: return {MesaNvk, Encoding::Mesa};
        case VK_DRIVER_ID_MESA_LLVMPIPE: return {Software, Encoding::Mesa};
        case VK_DRIVER_ID_GOOGLE_SWIFTSHADER: return {Software, Encoding::Unknown};
        case VK_DRIVER_ID_AMD_PROPRIETARY:
        case VK_DRIVER_ID_AMD_OPEN_SOURCE: return {Amd, Encoding::Unknown};
        case VK_DRIVER_ID_MESA_RADV:
        case VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA:
        case VK_DRIVER_ID_MESA_TURNIP:
        case VK_DRIVER_ID_MESA_V3DV:
        case VK_DRIVER_ID_MESA_PANVK:
        case VK_DRIVER_ID_MESA_VENUS:
        case VK_DRIVER_ID_MESA_DOZEN:
        case VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA:
        case VK_DRIVER_ID_MESA_HONEYKRISP: return {Mesa, Encoding::Mesa};
        default: return {Unknown, Encoding::Unknown};
    }
}

bool supports_device_extension(VkPhysicalDevice device,
                               const char* extension_name) {
    using namespace bluevk;
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) !=
            VK_SUCCESS ||
        count == 0) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(
            device, nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return false;
    }
    return std::ranges::any_of(extensions, [extension_name](const auto& ext) {
        return std::strcmp(ext.extensionName, extension_name) == 0;
    });
}

std::optional<double> read_numeric_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    double value = 0.0;
    if (!(input >> value)) return std::nullopt;
    return value;
}

std::optional<uint32_t> read_hex_id(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    if (!(input >> value)) return std::nullopt;
    try {
        return static_cast<uint32_t>(std::stoul(value, nullptr, 0));
    } catch (...) {
        return std::nullopt;
    }
}

bool is_drm_card_name(std::string_view name) {
    if (!name.starts_with("card") || name.size() == 4) return false;
    return std::ranges::all_of(name.substr(4), [](char ch) {
        return ch >= '0' && ch <= '9';
    });
}

std::optional<std::filesystem::path> linux_gpu_device_path(
        [[maybe_unused]] const IRhiBackend::GpuInfoRaw& info) {
#if defined(__linux__) && !defined(__ANDROID__)
    std::error_code ec;
    if (info.pci_domain >= 0 && info.pci_bus >= 0 && info.pci_device >= 0 &&
        info.pci_function >= 0) {
        std::filesystem::path exact = std::filesystem::path(
            "/sys/bus/pci/devices") /
            std::format("{:04x}:{:02x}:{:02x}.{:x}", info.pci_domain,
                        info.pci_bus, info.pci_device, info.pci_function);
        if (std::filesystem::is_directory(exact, ec)) return exact;
    }

    // PCI bus properties are optional (for example on older Vulkan drivers).
    // Fall back to matching the active device's Vulkan vendor/device IDs
    // against DRM card devices. This remains selection-aware on the common
    // multi-GPU case where the adapters are different models.
    const std::filesystem::path drm_root{"/sys/class/drm"};
    for (std::filesystem::directory_iterator it(drm_root, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!is_drm_card_name(it->path().filename().string())) continue;
        std::filesystem::path device = it->path() / "device";
        const auto vendor = read_hex_id(device / "vendor");
        const auto id = read_hex_id(device / "device");
        if (vendor && id && *vendor == info.vendor_id && *id == info.device_id) {
            std::filesystem::path resolved =
                std::filesystem::canonical(device, ec);
            if (!ec) return resolved;
            ec.clear();
            return device;
        }
    }
#endif
    return std::nullopt;
}

void read_linux_gpu_telemetry(const IRhiBackend::GpuInfoRaw& info,
                              IRhiBackend::GpuTelemetry& telemetry) {
    const auto device = linux_gpu_device_path(info);
    if (!device) return;

    if (const auto value = read_numeric_file(*device / "gpu_busy_percent")) {
        telemetry.utilization_percent = *value;
    }
    if (const auto value = read_numeric_file(*device / "mem_busy_percent")) {
        telemetry.memory_utilization_percent = *value;
    }
    if (const auto value = read_numeric_file(*device / "mem_info_vram_used")) {
        telemetry.device_vram_used_mb = *value / (1024.0 * 1024.0);
    }
    if (const auto value = read_numeric_file(*device / "mem_info_vram_total")) {
        telemetry.device_vram_total_mb = *value / (1024.0 * 1024.0);
    }

    std::error_code ec;
    const std::filesystem::path hwmon_root = *device / "hwmon";
    for (std::filesystem::directory_iterator it(hwmon_root, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::filesystem::path sensors = it->path();
        if (const auto value = read_numeric_file(sensors / "temp1_input")) {
            telemetry.temperature_c = *value / 1000.0;
        }
        if (const auto value = read_numeric_file(sensors / "power1_average")) {
            telemetry.power_w = *value / 1'000'000.0;
        }
        if (const auto value = read_numeric_file(sensors / "freq1_input")) {
            telemetry.core_clock_mhz = *value / 1'000'000.0;
        }
        if (const auto value = read_numeric_file(sensors / "freq2_input")) {
            telemetry.memory_clock_mhz = *value / 1'000'000.0;
        }
        if (const auto value = read_numeric_file(sensors / "fan1_input")) {
            telemetry.fan_rpm = *value;
        }
        break;
    }
}

} // namespace

// The engine prefers Vulkan and falls back to OpenGL at construction, so
// render_api_name() reads the backend Filament ACTUALLY created
// (engine_->getBackend()) — the live driver path — rather than the fixed
// api() family string ("Filament"). This is what the Result_Report needs
// so the reported API matches the run.

std::string FilamentBackend::render_api_name() const {
    if (!engine_) return {};
    return engine_->getBackend() == filament::Engine::Backend::VULKAN
               ? "Vulkan"
               : "OpenGL";
}

// gpu_info() is the ONE place GPU-API specifics are read (AGENTS.md).
// On the Vulkan path it pulls the selected VkPhysicalDevice from Filament's
// VulkanPlatform and reads the selected device's model name and summed
// DEVICE_LOCAL memory-heap sizes (→ MB). On the OpenGL fallback it queries the
// cached GL vendor/renderer/version strings and leaves VRAM undetermined. Any datum
// the driver does not expose is left at its neutral default so the pure RunInfo
// formatter substitutes the Unknown_Value_Placeholder; the query never aborts.
IRhiBackend::GpuInfoRaw FilamentBackend::gpu_info() const {
    GpuInfoRaw info;  // neutral: empty model, vram_mb = -1
    if (!engine_) return info;

    if (engine_->getBackend() == filament::Engine::Backend::VULKAN) {
        using namespace bluevk;

        // Filament creates a VulkanPlatform for the Vulkan backend;
        // getPlatform() hands it back as the Platform base. We only reach
        // here when getBackend() == VULKAN, so the static_cast is safe and
        // we avoid depending on RTTI (Filament is built with -fno-rtti).
        auto* platform =
            static_cast<filament::backend::VulkanPlatform*>(engine_->getPlatform());
        if (!platform) return info;

        VkPhysicalDevice physical_device = platform->getPhysicalDevice();
        if (physical_device != VK_NULL_HANDLE) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(physical_device, &props);
            info.model_name = props.deviceName;
            info.vendor_id = props.vendorID;
            info.device_id = props.deviceID;
            // apiVersion is already uint32_t. Decode the same fields as the
            // Vulkan C macros without their old-style casts.
            info.api_version = std::format(
                "{}.{}.{}", props.apiVersion >> 22U,
                (props.apiVersion >> 12U) & 0x3FFU,
                props.apiVersion & 0xFFFU);

            const bool has_driver_properties =
                props.apiVersion >= ((1U << 22U) | (2U << 12U)) ||
                supports_device_extension(
                    physical_device, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
            const bool has_pci_bus_info = supports_device_extension(
                physical_device, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);

            VkPhysicalDeviceDriverProperties driver_props{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDevicePCIBusInfoPropertiesEXT pci_props{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
            void* property_chain = nullptr;
            if (has_pci_bus_info) {
                pci_props.pNext = property_chain;
                property_chain = &pci_props;
            }
            if (has_driver_properties) {
                driver_props.pNext = property_chain;
                property_chain = &driver_props;
            }
            if (property_chain) {
                VkPhysicalDeviceProperties2 props2{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                props2.pNext = property_chain;
                vkGetPhysicalDeviceProperties2(physical_device, &props2);
            }
            if (has_driver_properties) {
                info.driver_name = driver_props.driverName;
                info.driver_info = driver_props.driverInfo;
                const auto [family, encoding] =
                    vulkan_driver_identity(driver_props.driverID);
                info.driver_family = family;
                info.driver_version = decode_driver_version(
                    encoding, props.driverVersion, info.driver_info);
            }
            if (info.driver_family == DriverFamily::Unknown) {
                info.driver_family = classify_driver_text(
                    info.driver_name, info.driver_info, info.model_name);
            }
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
                info.driver_family = DriverFamily::Software;
            }
            if (has_pci_bus_info) {
                info.pci_domain = static_cast<int>(pci_props.pciDomain);
                info.pci_bus = static_cast<int>(pci_props.pciBus);
                info.pci_device = static_cast<int>(pci_props.pciDevice);
                info.pci_function = static_cast<int>(pci_props.pciFunction);
            }

#ifndef __APPLE__
            // VRAM ≈ the sum of every DEVICE_LOCAL memory heap, bytes → MB.
            VkPhysicalDeviceMemoryProperties mem_props{};
            vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
            VkDeviceSize device_local_bytes = 0;
            for (uint32_t i = 0; i < mem_props.memoryHeapCount; ++i) {
                if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    device_local_bytes += mem_props.memoryHeaps[i].size;
                }
            }
            info.vram_mb = static_cast<long>(device_local_bytes / (VkDeviceSize{1024} * 1024));
#endif
            // Apple Silicon's device-local heaps use unified memory. Keep the
            // dedicated VRAM field unavailable instead of reporting system RAM.
        }

        return info;
    }

    // Filament caches these strings from its active GL context. Querying them
    // through Platform needs no context switch and cannot pick another adapter.
    // GL exposes no portable VRAM or PCI identity, so those stay undetermined.
    if (auto* platform = engine_->getPlatform()) {
        using InfoType = filament::backend::Platform::DeviceInfoType;
        auto* const driver =
            const_cast<filament::backend::Driver*>(engine_->getDriver());
        const utils::CString renderer = platform->getDeviceInfo(
            InfoType::OPENGL_RENDERER, driver);
        const utils::CString vendor = platform->getDeviceInfo(
            InfoType::OPENGL_VENDOR, driver);
        const utils::CString version = platform->getDeviceInfo(
            InfoType::OPENGL_VERSION, driver);
        if (!renderer.empty()) {
            info.model_name = renderer.c_str();
        }
        if (!vendor.empty()) info.driver_name = vendor.c_str();
        if (!version.empty()) info.driver_info = version.c_str();
        const auto details = describe_opengl_driver(
            info.driver_name, info.model_name, info.driver_info);
        info.driver_family = details.family;
        info.driver_version = details.driver_version;
        info.api_version = details.api_version;
    }
    return info;
}

IRhiBackend::GpuTelemetry FilamentBackend::gpu_telemetry() const {
    GpuTelemetry telemetry;
    if (!engine_) return telemetry;

    const GpuInfoRaw info = gpu_info();
    if (info.vram_mb >= 0) {
        telemetry.device_vram_total_mb = static_cast<double>(info.vram_mb);
    }

    if (engine_->getBackend() == filament::Engine::Backend::VULKAN) {
        using namespace bluevk;
        auto* platform = static_cast<filament::backend::VulkanPlatform*>(
            engine_->getPlatform());
        auto* const physical_device =
            platform ? platform->getPhysicalDevice() : VK_NULL_HANDLE;
        if (physical_device != VK_NULL_HANDLE && supports_device_extension(
                physical_device, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
            VkPhysicalDeviceMemoryProperties2 memory{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
            memory.pNext = &budget;
            vkGetPhysicalDeviceMemoryProperties2(physical_device, &memory);

            VkDeviceSize usage_bytes = 0;
            VkDeviceSize budget_bytes = 0;
            for (uint32_t i = 0; i < memory.memoryProperties.memoryHeapCount; ++i) {
                if (memory.memoryProperties.memoryHeaps[i].flags &
                    VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    usage_bytes += budget.heapUsage[i];
                    budget_bytes += budget.heapBudget[i];
                }
            }
            telemetry.process_vram_used_mb =
                static_cast<double>(usage_bytes) / (1024.0 * 1024.0);
            telemetry.process_vram_budget_mb =
                static_cast<double>(budget_bytes) / (1024.0 * 1024.0);
        }
    }

    read_linux_gpu_telemetry(info, telemetry);
    return telemetry;
}

// ── Resource management: mesh upload ────────────────────────────

// ── Resource management: mesh upload ────────────────────────────
// upload_mesh / update_mesh_vertices were migrated to the MeshStore
// subsystem (rhi/mesh_store.{h,cpp}). The backend's IRhiBackend
// overrides delegate to it.

GpuMesh FilamentBackend::upload_mesh(const Mesh& mesh) {
    return mesh_store_->upload(mesh);
}

GpuMesh FilamentBackend::upload_skinned(const SkinnedMesh& mesh) {
    return mesh_store_->upload_skinned(mesh);
}

void FilamentBackend::release_mesh(const GpuMesh& mesh) {
    mesh_store_->release(mesh);
}

// ── Resource management: texture loading ────────────────────────

// ── Resource management: texture loading ────────────────────────
// load_texture / get_filament_texture were migrated to the TextureStore
// subsystem (rhi/texture_store.{h,cpp}). The backend's IResourceProvider
// overrides delegate to it.

uint64_t FilamentBackend::load_texture(
    const std::string& path, TextureSemantic semantic) {
    return texture_store_->load(path, semantic);
}

filament::Texture* FilamentBackend::get_filament_texture(uint64_t handle) {
    return texture_store_->get(handle);
}

void FilamentBackend::record_external_original_texture(
        uint32_t width, uint32_t height, TextureSemantic semantic,
        bool complete_mip_chain) {
    texture_store_->record_external_original_texture(
        width, height, semantic, complete_mip_chain);
}

filament::Texture* FilamentBackend::placeholder_texture() {
    return texture_store_->placeholder();
}

filament::Texture* FilamentBackend::placeholder_normal_texture() {
    return texture_store_->placeholder_normal();
}

filament::Texture* FilamentBackend::placeholder_depth_texture() {
    return texture_store_->placeholder_depth();
}

// ── Pipeline initialization ─────────────────────────────────────

Result<void> FilamentBackend::init_pipelines(const AppConfig& config) {
    if (!engine_) {
        return std::unexpected(Error("Filament engine not initialized"));
    }

    // Idempotent: bringing the pipeline up twice would rebuild the render
    // context (leaking the composite-pass material instances and resetting the
    // persistent scene/feature state). The renderer only calls this once; the
    // gated smoke harness shares one backend across cases, so a later case may
    // re-request bring-up — treat that as a no-op success.
    if (pipelines_initialized_) {
        return {};
    }
    color_grading_cache_ = {};

    // ── Load compiled base materials ────────────────────────────
    if (auto r = mat_cache_->load_base_materials(); !r) {
        return std::unexpected(r.error());
    }

    // ── Skybox + IBL probe ──────────────────────────────────────
    // The procedural Z-up cubemap + Skybox + IndirectLight probe are
    // built by the LightingSystem (constructed in build_render_context
    // below, then init_environment'd once the core objects exist).

    // ── Create the shared placeholder textures ──────────────────
    // 1×1 white + flat-normal fallbacks for material slots with no
    // texture assigned. Owned by the texture store.
    texture_store_->create_placeholders();

    // (unlit material loaded by mat_cache_->load_base_materials();
    //  grass material is loaded by GrassFeature)

    std::printf("FilamentBackend: All materials loaded\n");

    // Stash the config and build the shared render context before any
    // feature initialises (features read ctx.engine/scene/resources).
    config_ = config;
    texture_store_->set_texture_quality(config_.texture_quality);
    mat_cache_->set_tessellation_config(
        config_.tessellation.scale, config_.tessellation.distance,
        config_.tessellation.enabled);

    // ── Refraction routing  ──────────────────────
    // Tell the material cache whether glass surfaces should resolve to the
    // refractive heaven_glass material. Set before the scene is built (the
    // first render_frame), so glass-flagged surfaces pick up the refractive
    // material when --refraction is on and the opaque heaven_lit material
    // (Shipping_Look parity) when it is off. The cache owns the per-frame
    // screen-data-availability fallback  behind its own seam.
    mat_cache_->set_refraction_enabled(config_.features.refraction);

    // ── Transparent foliage shadows  ─────────────
    // Tell the material cache whether foliage casters should resolve to the
    // coverage-modulated heaven_foliage_shadow material. Set before the scene
    // is built so foliage picks up the transparent-shadow caster when
    // --transparent-shadows is on and the Shipping_Look opaque-silhouette path
    // (heaven_lit_twosided) when it is off (≤1/255 parity).
    mat_cache_->set_transparent_shadows(config_.features.transparent_shadows);

    // ── Specular anti-aliasing  ──────────────────
    // Tell the material cache whether the lit base materials should apply
    // Filament's geometric specular anti-aliasing, and at what strength. The
    // variance/threshold are resolved from the specular-AA quality knob
    // (config_.quality.specular_aa_level → spec_aa_strength). Set before the
    // scene is built so each lit/advanced instance picks up the setters when
    // --specular-aa is on, and keeps Filament's defaults (no setter call) when
    // it is off (≤1/255 parity).
    mat_cache_->set_specular_aa(config_.features.specular_aa, config_.quality);
    mat_cache_->set_material_workflow(config_.material_workflow);

    // ── Colored penumbra  ────────────────────────
    // Tell the material cache whether plain Standard_Lit single-sided surfaces
    // should resolve to the heaven_lit_colored_penumbra variant (heaven_lit +
    // `coloredPenumbra: true`, the only material that compiles in the
    // HAS_COLORED_PENUMBRA tint block, built against the epsilon-clamped
    // lib/filament shader so near-white surfaces stay firefly-free).
    // Set before the scene is built so plain lit surfaces pick up the variant
    // when --colored-penumbra is on and keep heaven_lit (Shipping_Look parity)
    // when it is off (≤1/255 parity). The visible penumbra tint
    // lands on the soft-shadow path configured by LightingSystem::setup.
    mat_cache_->set_colored_penumbra(config_.features.colored_penumbra);

    build_render_context();

    // ── MSAA capability query  ─────────────────────────
    // Resolve the requested MSAA sample count against the GPU's supported
    // counts ONCE here, so the per-frame configure_post_processing call uses a
    // count the device can honour and we emit at most one requested-vs-applied
    // diagnostic. Filament exposes no direct "enumerate supported sample
    // counts" API; every desktop Vulkan/GL backend the renderer targets
    // supports the standard {1,2,4,8} multisample counts, which is a superset
    // of the knob's accepted set {1,2,4}, so the request is honoured on real
    // hardware and the fallback path is exercised by the gated smoke seam.
    {
        static constexpr int kSupportedSampleCounts[] = {1, 2, 4, 8};
        msaa_resolved_samples_ = configure_msaa(
            config_.quality.msaa_samples, kSupportedSampleCounts);
        config_.quality.msaa_samples = msaa_resolved_samples_;
    }

    // Build the initial procedural sky cubemap + Skybox + IBL probe now
    // that the LightingSystem exists (constructed in build_render_context).
    lighting_->init_environment(lighting_for_time_of_day(config.time_of_day));
    // The four roughness/irradiance probes are generated by Filament render
    // passes. Complete those passes before any benchmark frame can be accepted,
    // so measured GPU time never includes deferred IBL compilation/filtering.
    engine_->flushAndWait();

    // ── Initialize modular render features ──────────────────────
    // Features that have been decomposed out of the backend live in
    // render_features_ and are driven through the IRenderFeature
    // lifecycle. Currently: clouds.
    if (!config.render_test_scene.empty() &&
        !render_test_scene_uses_assets(config.render_test_scene)) {
        auto assets = RenderTestSceneAssets::create(*engine_);
        if (!assets) return std::unexpected(assets.error());
        render_test_assets_ = std::move(*assets);
        render_ctx_.test_scene_assets = render_test_assets_.get();
    }
    init_features(config);

    std::printf("FilamentBackend: Pipelines initialized\n");
    pipelines_initialized_ = true;
    return {};
}

Result<void> FilamentBackend::prepare_static_displacement(
        RhiScene& scene, const MaterialResolver& resolve,
        const LoadingProgressCallback& progress) {
    return prepare_scene_displacement(*engine_, *mesh_store_, *texture_store_,
                                      config_, scene, resolve, progress);
}

Result<void> FilamentBackend::reconfigure_for_reload(
        const AppConfig& config) {
    if (!pipelines_initialized_ || !engine_ || !texture_store_ || !mat_cache_) {
        return std::unexpected(
            Error("Filament pipelines are unavailable for reload"));
    }

    if (input_response_) input_response_->retire_pending("scene reload");
    engine_->flushAndWait();
    destroy_frame_generation();
    // Features may own material instances and may borrow TextureStore entries
    // (notably the hi-res sky). Destroy those before invalidating textures.
    destroy_features();
    auto bloom = view_->getBloomOptions();
    bloom.dirt = nullptr;
    bloom.dirtStrength = 0.0f;
    view_->setBloomOptions(bloom);
    render_ctx_.test_scene_assets = nullptr;
    render_test_assets_.reset();
    if (scene_builder_) scene_builder_->clear(core_);
    if (depth_proxy_builder_ && depth_prepass_) {
        depth_proxy_builder_->clear(core_, depth_prepass_->proxy_scene());
    }
    if (light_spawner_) light_spawner_->destroy(core_);
    mat_cache_->clear_instances();
    texture_store_->clear_loaded_textures();

    config_ = config;
    texture_store_->set_texture_quality(config_.texture_quality);
    mat_cache_->set_tessellation_config(
        config_.tessellation.scale, config_.tessellation.distance,
        config_.tessellation.enabled);
    mat_cache_->set_refraction_enabled(config_.features.refraction);
    mat_cache_->set_transparent_shadows(
        config_.features.transparent_shadows);
    mat_cache_->set_specular_aa(config_.features.specular_aa, config_.quality);
    mat_cache_->set_material_workflow(config_.material_workflow);
    mat_cache_->set_colored_penumbra(config_.features.colored_penumbra);

    if (!config_.render_test_scene.empty() &&
        !render_test_scene_uses_assets(config_.render_test_scene)) {
        auto assets = RenderTestSceneAssets::create(*engine_);
        if (!assets) return std::unexpected(assets.error());
        render_test_assets_ = std::move(*assets);
        render_ctx_.test_scene_assets = render_test_assets_.get();
    }
    init_features(config_);
    request_frame_generation(config_.frame_generation);
    // Reloaded post-process features / quality also need their cached View
    // settings reapplied, even if weather and time of day did not change.
    lighting_initialized_ = false;
    const std::string_view texture_quality = texture_quality_id(config_.texture_quality);
    std::printf("FilamentBackend: texture/material resources reloaded (%.*s)\n",
                static_cast<int>(texture_quality.size()), texture_quality.data());
    return {};
}

// ── Post-processing setup ───────────────────────────────────────
// setup_post_processing was migrated to rhi/post_process.{h,cpp} as the
// free function configure_post_processing(). The backend keeps ownership
// of the resulting ColorGrading object in color_grading_.

// ── Sky-sphere feature ──────────────────────────────
// init_sky / update_sky were migrated to rhi/features/sky_feature.{h,cpp}.
// The IBL probe generator (build_sky_cubemap) now lives in
// rhi/sky_ibl.{h,cpp}; the LightingSystem subsystem owns the resulting
// texture.

// ── Lighting setup ──────────────────────────────────────────────
// setup_lighting + the init-time skybox/IBL block were migrated to the
// LightingSystem subsystem (rhi/lighting_system.{h,cpp}); render_frame
// calls lighting_->setup(...).

// ── Clear renderables ───────────────────────────────────────────
// clear_renderables / build_scene_entities / update_animated_transforms
// were migrated to the SceneBuilder subsystem (rhi/scene_builder.{h,cpp}).

// ── Grass feature ───────────────────────────────────────────────
// init_grass / update_grass and the deferred per-island generation
// were migrated to rhi/features/grass_feature.{h,cpp}.

// ── Particle feature ────────────────────────────────
// init_particles / update_particles / set_particle_emitters, the
// heat-haze composite (init_composite / ensure_scene_capture) and the
// flame/glow/firefly materials were migrated to
// rhi/features/particle_feature.{h,cpp}.

// Lens flares were migrated to rhi/features/lens_flare_feature.{h,cpp}.
// The backend still spawns and animates the scene lights they anchor to;
// it hands the feature read-only SceneLightInfo descriptors via
// FrameContext::scene_lights.


// ── Particle emitter setup (called after scene load) ────────────

void FilamentBackend::set_particle_emitters(std::span<const ParticleEmitter> emitters) {
    // Visible flame origins also anchor their independent scene lights. Keep
    // this input available when particle rendering is disabled.
    light_spawner_->set_pending_emitters(emitters);
    // Forwarded to the particle feature, which builds the GPU
    // resources lazily on its first update().
    if (particle_feature_) particle_feature_->set_emitters(emitters);
}

// ── Authored scene lights (lamp posts + accent lights) ──────────

// Heaven's heaven.world references LightSpot/LightOmni nodes for the
// lamp posts and accent lights. The mesh-emission heuristic in the
// scene-build path already lights geometry near windows/torches/lamp
// glass; these are *additional* authored lights. They are spawned once
// after the scene is built (see render_frame) and optionally bobbed
// with their floating island in update_floating_lights().

void FilamentBackend::set_scene_lights(std::span<const SceneLight> lights) {
    light_spawner_->set_pending_authored(lights);
}

void FilamentBackend::update_floating_lights(
    std::span<const std::pair<int, float>> group_offsets) {
    light_spawner_->update_floating(core_, group_offsets);
}

// ── Wind debug arrows (cloth simulation visualization) ─────────
// Migrated to rhi/features/wind_arrows_feature.{h,cpp}. The backend
// forwards the per-frame arrow list to the feature, which owns the
// cuboid VB/IB, material, and per-arrow entities.
void FilamentBackend::set_wind_debug_arrows(
    std::span<const IRhiBackend::WindArrow> arrows) {
    if (wind_arrows_feature_) wind_arrows_feature_->set_arrows(arrows);
}


// ── God rays initialization ─────────────────────────────────────

// ── God rays feature ────────────────────────────────────────────
// init_godrays / update_godrays were migrated to
// rhi/features/godrays_feature.{h,cpp}.

// ── Volumetric fog feature ──────────────────────────
// init_volfog / update_volfog and the blue-noise helper were migrated
// to rhi/features/volfog_feature.{h,cpp}.

// ── Composite pass init (heat haze) ─────────────────────────────

// Builds the secondary scene + view that runs after the main view.
// The composite scene contains:
//   1. A fullscreen quad with `heaven_blit.mat` that just blits
//      the captured scene texture onto the swap chain.
//   2. Per-emitter heat-haze billboards (added in update_particles)
//      which sample the captured texture at offset UVs to produce
//      visible distortion.

// init_composite was migrated to rhi/features/particle_feature.cpp.

// ── Cloud initialization ────────────────────────────────────────

// ── Cloud feature ───────────────────────────────────────────────
// init_clouds / update_clouds were migrated to
// rhi/features/cloud_feature.{h,cpp}. The backend now drives clouds
// through the render_features_ vector (see build_render_context /
// init_features / update_features below).

// ── Render feature framework ────────────────────────────────────

void FilamentBackend::set_asset_context(
        PathResolverRef paths, AssetFileSystemRef assets) {
    path_resolver_ = paths;
    asset_file_system_ = assets;
    render_ctx_.paths = paths;
    render_ctx_.assets = assets;
    if (texture_store_) {
        texture_store_->set_asset_file_system(assets);
    }
}

Result<void> FilamentBackend::set_render_test_ground(
        std::size_t instance_index, glm::vec2 local_min,
        glm::vec2 local_max, float local_z) {
    if (!render_test_assets_) {
        return std::unexpected(Error{"procedural ground requires a render test scene"});
    }
    return render_test_assets_->set_ground_patch(
        instance_index, local_min, local_max, local_z);
}

void FilamentBackend::build_render_context() {
    render_ctx_.engine    = engine_;
    render_ctx_.scene     = scene_;
    render_ctx_.view      = view_;
    render_ctx_.renderer  = renderer_;
    render_ctx_.camera    = fil_camera_;
    render_ctx_.resources = this;

    if (asset_file_system_) {
        render_ctx_.assets = asset_file_system_;
        texture_store_->set_asset_file_system(asset_file_system_);
    }

    render_ctx_.paths = path_resolver_;

    // Non-owning core bundle for the collaborator subsystems.
    core_.engine   = engine_;
    core_.scene    = scene_;
    core_.view     = view_;
    core_.renderer = renderer_;
    core_.camera   = fil_camera_;

    // Construct backend-owned collaborator subsystems now that the core
    // objects exist. They create their GPU resources lazily on first use.
    depth_prepass_ = std::make_unique<DepthPrePass>(core_);
    depth_proxy_builder_ = std::make_unique<DepthProxyBuilder>();
    scene_color_capture_ = std::make_unique<SceneColorCapture>(core_);
    if (hdr_) {
        if (!hdr_output_) hdr_output_ = std::make_unique<HdrOutput>(core_);
        hdr_output_->set_luminance(config_.hdr_paper_white_nits, config_.hdr_peak_nits);
    }
    scene_builder_ = std::make_unique<SceneBuilder>();
    light_spawner_ = std::make_unique<SceneLightSpawner>();
    lighting_      = std::make_unique<LightingSystem>(core_);
    request_frame_generation(config_.frame_generation);
}

void FilamentBackend::init_features(const AppConfig& config) {
    // Massive foliage already contains the full baseline grass population.
    // Keep the legacy grass feature registered for runtime fallback and test
    // visibility, but start it disabled whenever massive foliage is active so
    // a default run does not draw two complete meadows.
    {
        AppConfig grass_config = config;
        if (config.features.massive_foliage) {
            grass_config.features.grass = false;
        }
        auto gr = std::make_unique<GrassFeature>();
        grass_feature_ = gr.get();
        gr->init(render_ctx_, grass_config);
        render_features_.push_back(std::move(gr));
    }

    // Massive foliage — build-once geometry feature modeled on GrassFeature
    // Disabled by default (config.features.massive_foliage), so a
    // flagless run is Shipping_Look-equivalent. The raw pointer is a
    // non-owning observer for build-once instrumentation only (mirrors
    // grass_feature_), read by debug_build_counters(); render_features_ owns
    // the feature. Registered exactly once here (smoke test 10.4 asserts
    // exactly one "massive-foliage" feature).
    {
        auto mf = std::make_unique<MassiveFoliageFeature>();
        massive_foliage_feature_ = mf.get();
        mf->init(render_ctx_, config);
        render_features_.push_back(std::move(mf));
    }

    // Particles + heat haze — GPU billboard emitters built lazily once
    // the loader hands over the scene emitters (set_particle_emitters).
    {
        auto pf = std::make_unique<ParticleFeature>();
        particle_feature_ = pf.get();
        pf->init(render_ctx_, config);
        render_features_.push_back(std::move(pf));
    }

    // Lens flares — halo quads anchored to warm luminaires. Built
    // lazily once the backend has spawned the scene lights and handed
    // them over via FrameContext::scene_lights.
    {
        auto lf = std::make_unique<LensFlareFeature>();
        lens_flare_feature_ = lf.get();
        lf->init(render_ctx_, config);
        render_features_.push_back(std::move(lf));
    }

    // God rays — depth-pre-pass participant. Init before clouds so its
    // entity priority ordering relative to other transparent quads
    // matches the legacy inline order.
    {
        auto gr = std::make_unique<GodraysFeature>();
        godrays_feature_ = gr.get();
        gr->init(render_ctx_, config);
        render_features_.push_back(std::move(gr));
    }

    // Volumetric fog — depth-pre-pass participant. Init before god
    // rays in the render order via lower entity priority (set in the
    // feature), but registering here is order-independent.
    {
        auto vf = std::make_unique<VolfogFeature>();
        volfog_feature_ = vf.get();
        vf->init(render_ctx_, config);
        render_features_.push_back(std::move(vf));
    }

    // Clouds — dormant by default (sky-quad seam). First migrated
    // feature; others follow as the backend is decomposed.
    {
        auto cloud = std::make_unique<CloudFeature>();
        cloud_feature_ = cloud.get();
        cloud->init(render_ctx_, config);
        render_features_.push_back(std::move(cloud));
    }

    // Volumetric clouds — raymarched cloud layer that supersedes the
    // dormant CloudFeature above. Disabled by default; depth
    // pre-pass participant. Registered exactly once here.
    {
        auto vc = std::make_unique<VolumetricCloudFeature>();
        volumetric_cloud_feature_ = vc.get();
        vc->init(render_ctx_, config);
        render_features_.push_back(std::move(vc));
    }

    // SSGI — screen-space one-bounce global illumination. Disabled by default
    // depth pre-pass participant. Registered exactly once
    // here (smoke test 9.5 asserts exactly one "ssgi" feature).
    {
        auto ssgi = std::make_unique<SsgiFeature>();
        ssgi_feature_ = ssgi.get();
        ssgi->init(render_ctx_, config);
        render_features_.push_back(std::move(ssgi));
    }

    // Sky-sphere — the visible procedural sky. Detaching/attaching
    // Filament's cubemap Skybox to match this feature's enabled state
    // is handled in setup_lighting(); init it here so the sphere
    // entity exists before the first frame.
    {
        auto sky = std::make_unique<SkyFeature>();
        sky_feature_ = sky.get();
        sky->init(render_ctx_, config);
        render_features_.push_back(std::move(sky));
    }

    // Wind debug arrows — cloth-simulation visualization. Owns no
    // renderables until the main loop pushes arrows via
    // set_wind_debug_arrows() (only when `--wind-debug` is enabled).
    {
        auto wa = std::make_unique<WindArrowsFeature>();
        wind_arrows_feature_ = wa.get();
        wa->init(render_ctx_, config);
        render_features_.push_back(std::move(wa));
    }

    // Rain — falling precipitation + splashes (weather spec).
    // Build-once geometry on the first active frame, animated through
    // uniforms thereafter; renders nothing only while Rain_Intensity is 0
    // (e.g. via `--rain-intensity 0` or `--no-rain`). The default intensity
    // is now medium (0.5), so the default run renders the storm. The raw
    // pointer is a non-owning observer for build-once instrumentation
    // only (mirrors grass_feature_), read by debug_build_counters();
    // render_features_ owns the feature.
    {
        auto rf = std::make_unique<RainFeature>();
        rain_feature_ = rf.get();
        rf->init(render_ctx_, config);
        render_features_.push_back(std::move(rf));
    }

    // Wet-surface reflections + wetness shading (Weather_System wetness
    // response + the selected modern technique). Owns no
    // renderables — it sets a per-view material global and retunes SSR,
    // restoring exact Shipping_Look parity when disabled or at intensity
    // 0. No observer pointer needed (it exposes no build counters).
    {
        auto wr = std::make_unique<WetReflectionsFeature>();
        wet_reflections_feature_ = wr.get();
        wr->init(render_ctx_, config);
        render_features_.push_back(std::move(wr));
    }

    // Advanced material shading models (Group A —).
    // A thin coordinating feature (the WetReflectionsFeature precedent): it owns
    // NO renderables. It carries the Group-A EnabledModels from
    // AppConfig::features into the MaterialInstanceCache's Material_Model_Selector
    // before the scene is built — its init() runs here, ahead of the first
    // render_frame where the scene is built and the cache bakes a
    // MaterialInstance per surface — and restores the Shipping_Look assignment
    // when every model is disabled (≤1/255 parity). The cache is an
    // existing backend infrastructure subsystem, borrowed by a non-owning
    // pointer, so NO per-effect member is added to FilamentBackend
    // Registered exactly once here (the structural test 10.2
    // asserts exactly one "advanced-materials" feature).
    {
        auto am = std::make_unique<AdvancedMaterialsFeature>(mat_cache_.get());
        am->init(render_ctx_, config);
        render_features_.push_back(std::move(am));
    }
}

void FilamentBackend::update_features(const FrameContext& frame) {
    const bool log_startup_timing = frame_count_ <= 2;
    auto total_start = startup_timer_now();
    bool printed_any = false;
    for (auto& f : render_features_) {
        if (!f->enabled()) continue;
        profile::Scope feature_scope(f->name(), "Render feature update");
        auto feature_start = startup_timer_now();
        f->update(render_ctx_, frame);
        const double elapsed = startup_elapsed_ms(feature_start);
        if (log_startup_timing && elapsed >= 1.0) {
            std::printf("  [startup] feature %-26s %8.2f ms\n",
                        f->name(), elapsed);
            printed_any = true;
        }
    }
    const double total = startup_elapsed_ms(total_start);
    if (log_startup_timing && (printed_any || total >= 1.0)) {
        std::printf("  [startup] %-34s %8.2f ms\n",
                    "render feature updates", total);
    }
}

void FilamentBackend::destroy_features() {
    // Each migrated feature owns its GPU resources and frees them in
    // its own destroy(); call those before the core engine teardown.
    if (grass_feature_) {
        grass_feature_->destroy(render_ctx_);
        grass_feature_ = nullptr;
    }
    if (massive_foliage_feature_) {
        massive_foliage_feature_->destroy(render_ctx_);
        massive_foliage_feature_ = nullptr;
    }
    if (cloud_feature_) {
        cloud_feature_->destroy(render_ctx_);
        cloud_feature_ = nullptr;
    }
    if (volumetric_cloud_feature_) {
        volumetric_cloud_feature_->destroy(render_ctx_);
        volumetric_cloud_feature_ = nullptr;
    }
    if (godrays_feature_) {
        godrays_feature_->destroy(render_ctx_);
        godrays_feature_ = nullptr;
    }
    if (ssgi_feature_) {
        ssgi_feature_->destroy(render_ctx_);
        ssgi_feature_ = nullptr;
    }
    if (volfog_feature_) {
        volfog_feature_->destroy(render_ctx_);
        volfog_feature_ = nullptr;
    }
    if (sky_feature_) {
        sky_feature_->destroy(render_ctx_);
        sky_feature_ = nullptr;
    }
    if (particle_feature_) {
        particle_feature_->destroy(render_ctx_);
        particle_feature_ = nullptr;
    }
    if (lens_flare_feature_) {
        lens_flare_feature_->destroy(render_ctx_);
        lens_flare_feature_ = nullptr;
    }
    if (wind_arrows_feature_) {
        wind_arrows_feature_->destroy(render_ctx_);
        wind_arrows_feature_ = nullptr;
    }
    if (rain_feature_) {
        rain_feature_->destroy(render_ctx_);
        rain_feature_ = nullptr;
    }
    render_features_.clear();
    wet_reflections_feature_ = nullptr;
}

// ── Material instance management ────────────────────────────────
// get_or_create_material_instance + the base materials were migrated to
// the MaterialInstanceCache subsystem (rhi/material_instance_cache.{h,cpp}).
// build_scene_entities calls mat_cache_->get_or_create(...).

// ── Depth pre-pass target management ────────────────────────────
// ensure_depth_target + the depth View/Camera/RenderTarget were migrated
// to the DepthPrePass subsystem (rhi/depth_prepass.{h,cpp}).

// ── Scene-capture target management (heat haze) ────────────────
// ensure_scene_capture was migrated to rhi/features/particle_feature.cpp.

// ── Frame rendering ─────────────────────────────────────────────

Result<void> FilamentBackend::apply_weather_state(
    const WeatherState& weather,
    const LightingParams& lighting,
    const TonemapParams& tonemap)
{
    profile::Scope profile_scope("Apply weather pipeline", "Rendering");
    // Guard the live Filament objects rather than throwing, so a re-application
    // attempted before the pipeline is ready (or after teardown) reports failure
    // and the caller keeps the last good state and continues.
    if (!engine_ || !view_) {
        return std::unexpected(Error("apply_weather_state: Filament engine/view not initialized"));
    }

    // Weather fog override: the overcast response raises distance
    // fog as rain intensity climbs. fog_density() returns 0 at intensity 0
    // (clear weather), and enabling Filament's distance fog at density 0 is NOT
    // the same as the Shipping_Look (which leaves distance fog OFF and drives fog
    // through the heaven_volumetric_fog material). So pass the real density only
    // when it is positive; otherwise pass the sentinel (-1.0f) so the
    // post-process config is bit-identical to the shipping look

    const float fog_density  = weather.fog_density();
    const float fog_override = fog_density > 0.0f ? fog_density : -1.0f;
    // Dynamic-resolution minimum render scale. The value was already
    // clamped at the CLI layer (--dynamic-resolution-quality); re-clamping here
    // is harmless and keeps QualityKnobs the single-source-of-truth bound, matching
    // how the other knobs are handled (clamp_dynres_min_scale lives in types.h).
    const float dynres_min_scale =
        clamp_dynres_min_scale(config_.quality.dynres_min_scale, config_.quality).value;
    // Dynamic resolution is an explicit interactive option. Measured benchmark
    // runs keep the configured pixel workload fixed, including full-resolution
    // clouds/fog and scene-color SSGI; lowering render scale would change the
    // workload behind a score. TAA remains enabled independently.
    const bool dynamic_resolution_enabled =
        !benchmark_measurement_ &&
        config_.features.dynamic_resolution;
    configure_frame_timing(*renderer_, dynamic_resolution_enabled);
    color_grading_ = configure_post_processing(
        *engine_, *view_, tonemap, color_grading_, fog_override,
        config_.quality, config_.features.msaa, config_.features.reflections,
        hdr_, config_.tone_mapper,
        config_.features.color_grading, config_.grading,
        dynamic_resolution_enabled, dynres_min_scale, config_.features,
        &color_grading_cache_);

    // Tiny fixtures cannot reliably trigger the load-adaptive controller.
    // Pin its real internal target to the configured lower scale so captures
    // exercise FSR deterministically; ordinary scenes keep controller limits.
    if (!config_.render_test_scene.empty() && dynamic_resolution_enabled) {
        auto options = view_->getDynamicResolutionOptions();
        options.maxScale = options.minScale;
        view_->setDynamicResolutionOptions(options);
    }

    // Apply lighting after post-processing so the day/night bloom retune is
    // not overwritten by configure_post_processing's baseline options.
    lighting_->setup(lighting, sky_feature_ && sky_feature_->enabled(),
                     config_.quality, config_.features, tonemap.bloom_strength);

    // Filament multiplies its whole bloom term by the dirt sampler. Bind a
    // linear unit-plus-dirt gain so clean bloom survives dark texels and the
    // original grime adds bounded modulation. Flare sprites have their own
    // owner; missing packaged dirt remains a clean-bloom fallback.
    filament::Texture* dirt = nullptr;
    if (render_test_assets_) {
        dirt = render_test_assets_->lens_dirt_texture();
    } else {
        const uint64_t dirt_handle = texture_store_->load_bloom_dirt_gain(
            config_.data_dir + "/heaven/textures/sfx/lens_dirt_d.dds");
        dirt = texture_store_->get(dirt_handle);
    }
    if (dirt) {
        filament::View::BloomOptions bloom = view_->getBloomOptions();
        bloom.dirt = dirt;
        bloom.dirtStrength = 1.0f;
        bloom.threshold = true;
        bloom.lensFlare = false;
        view_->setBloomOptions(bloom);
    }

    return {};
}

void FilamentBackend::set_live_render_config(
    const FeatureToggles& features, const QualityKnobs& quality) {
    config_.features.soft_shadows = features.soft_shadows;
    config_.features.contact_shadows = features.contact_shadows;
    config_.features.point_light_shadows = features.point_light_shadows;
    config_.features.reflections = features.reflections;
    config_.quality.soft_shadow_level = quality.soft_shadow_level;
    config_.quality.contact_shadow_level = quality.contact_shadow_level;
    config_.quality.shadow_light_budget = quality.shadow_light_budget;
    if (wet_reflections_feature_) {
        wet_reflections_feature_->set_reflections_enabled(features.reflections);
    }
    // These view/sun settings are cached between weather changes. Invalidate
    // that cache so edits take effect on the next frame even in steady weather.
    lighting_initialized_ = false;
}

std::optional<glm::mat4> FilamentBackend::debug_scene_color_view_proj() const {
    return scene_color_capture_ ? scene_color_capture_->history_view_proj() : std::nullopt;
}

void FilamentBackend::set_benchmark_measurement(bool enabled) {
    if (benchmark_measurement_ == enabled) return;
    benchmark_measurement_ = enabled;

    if (enabled) {
        // Advance past every history item already published. Older in-flight
        // frames can still arrive later, so collect_completed_gpu_times also
        // filters by this steady-clock boundary.
        collect_completed_gpu_times();
        benchmark_gpu_times_.clear();
        benchmark_gpu_start_ns_ =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Re-run the shared lighting/post-process setup on the next frame. This
    // applies the fixed-resolution measurement policy before the first scored
    // frame and restores the configured adaptive policy after the last one.
    lighting_initialized_ = false;
}

void FilamentBackend::collect_completed_gpu_times() {
    collected_gpu_ms_ = -1.0;
    if (!gpu_history_stream_) return;

    for (const auto& sample : gpu_history_stream_->getNewFrames()) {
        if (!sample) {
            if (input_response_) input_response_->history_missing();
            continue;
        }
        // Fence observations are independent of hardware timer-query support.
        // Match exact accepted beginFrame brackets before rejecting durations.
        if (input_response_) {
            input_response_->gpu_frame(sample->frameId, sample->beginFrame,
                sample->backendEndFrame, sample->gpuFrameComplete);
        }
        const auto gpu_ns = sample->gpuFrameDuration;
        if (gpu_ns <= 0) continue;

        collected_gpu_ms_ = static_cast<double>(gpu_ns) / 1'000'000.0;
        if (benchmark_measurement_ &&
            sample->beginFrame >= benchmark_gpu_start_ns_) {
            benchmark_gpu_times_.push_back(collected_gpu_ms_);
        }
    }
}

std::vector<double> FilamentBackend::finalize_benchmark_gpu_times() {
    if (!engine_ || !renderer_ || !benchmark_measurement_) return {};

    // GPU timestamps are asynchronous. Wait once, at the end of the run, then
    // ask Renderer to refresh its public FrameInfo history without submitting
    // another rendered frame. The persistent stream yields only frame IDs not
    // consumed by prior render_frame calls.
    engine_->flushAndWait();
    renderer_->skipFrame();
    collect_completed_gpu_times();
    return benchmark_gpu_times_;
}

void FilamentBackend::show_loading_screen(const std::string& data_dir,
                                          std::string_view status,
                                          int viewport_w, int viewport_h,
                                          float progress) {
    if (!loading_screen_) return;
    if (hdr_output_ && hdr_output_->needs_resize(viewport_w, viewport_h)) {
        // Loading/reloading can resize these targets before the regular
        // update_render_features resize hook runs. Release the same borrowers.
        view_->setRenderTarget(nullptr);
        if (overlay_view_) overlay_view_->setRenderTarget(nullptr);
        if (scene_color_capture_) scene_color_capture_->set_output_target(nullptr);
        for (auto& feature : render_features_) {
            feature->release_shared_targets(render_ctx_);
        }
    }
    // Resizing replaces the swap chain; the loading screen must borrow the
    // current one for each draw rather than retain the destroyed initial one.
    loading_screen_->show(*swapchain_, data_dir, status, viewport_w, viewport_h,
                          progress, asset_file_system_, hdr_output_.get());
}

FrameResult FilamentBackend::render_frame(
    std::span<const Instance> instances,
    std::span<const int> dirty_instance_indices,
    std::span<const GpuMesh> meshes,
    const MaterialResolver& resolve_material,
    const Camera& camera,
    const LightingParams& lighting,
    const WeatherState& weather,
    const TonemapParams& tonemap,
    int viewport_w, int viewport_h)
{
    profile::Scope scope("Backend frame", "Rendering");
    FrameResult result;
    if (!engine_ || !renderer_ || !swapchain_) return result;

    using FrameClock = std::chrono::steady_clock;
    const auto elapsed_ms = [](FrameClock::time_point start) {
        return std::chrono::duration<double, std::milli>(
            FrameClock::now() - start).count();
    };

    frame_count_++;

    auto stage_start = FrameClock::now();
    update_camera(camera, lighting, viewport_w, viewport_h);
    result.timings.camera_ms = elapsed_ms(stage_start);

    // One-time setup: seed the weather-dependent baseline (lighting/bloom +
    // distance fog + post-process) exactly once, recording the applied
    // WeatherState in prev_weather_. apply_weather_state holds the same three
    // re-applications the per-frame change check below reuses, so the first
    // frame and every later change share one code path.
    if (!lighting_initialized_) {
        if (apply_weather_state(weather, lighting, tonemap)) {
            prev_weather_         = weather;
            weather_initialized_  = true;
            lighting_initialized_ = true;
        }
    }

    // Per-frame weather change check: re-apply the
    // weather-dependent post-process, lighting/bloom, and distance-fog state
    // when — and only when — the current WeatherState differs from the last
    // successfully applied one (by its derived-output signature). This is
    // independent of lighting_initialized_: once weather_initialized_
    // has seeded prev_weather_, a later change re-applies even though the
    // one-time baseline already ran. While the state is unchanged the block is
    // skipped entirely, so steady-state rain costs nothing extra and a run that
    // stays at the Shipping_Look default never re-applies after the first frame
    // On failure prev_weather_ is NOT advanced, so the last good
    // state stays live and rendering continues.
    if (weather_initialized_ && weather != prev_weather_) {
        if (apply_weather_state(weather, lighting, tonemap)) {
            prev_weather_ = weather; // commit only on success
            // Per-frame discipline counter: a successful
            // re-application after the baseline seed. An unchanged WeatherState
            // never enters this block, so this stays flat in steady state.
            ++build_counters_.weather_reapplies;
        }
    }

    stage_start = FrameClock::now();

    // Time of day and the visible-sky toggle vary independently of weather.
    // The lighting subsystem caches their complete input signature; changed
    // frames update existing lights without rebuilding the grading pipeline.
    lighting_->update(lighting, sky_feature_ && sky_feature_->enabled(),
                      config_.features, tonemap.bloom_strength);

    // One-time scene construction: persistent renderable entities and
    // the emissive point lights spawned at every luminaire surface.
    mat_cache_->update_lamp_emission(lighting.time_of_day);
    const bool startup_scene_frame = !scene_builder_->built();
    if (startup_scene_frame) {
        // The set of mesh indices excluded from GPU instancing: Animated_Objects
        // and alpha-masked foliage (whose instanced masked depth proxy
        // would break the scene-depth silhouette). Computed via the shared helper
        // so the depth-proxy builder is given the IDENTICAL set.
        std::unordered_set<int> skinned_mesh_indices;
        {
            ScopedStartupTimer timer("first-frame material resolve");
            skinned_mesh_indices =
                instancing_excluded_mesh_indices(meshes, resolve_material);
        }

        {
            ScopedStartupTimer timer("first-frame scene entities");
            scene_builder_->build(core_, *mesh_store_, *mat_cache_,
                                  instances, meshes, resolve_material,
                                  config_.features.gpu_skinning,
                                  config_.features.gpu_instancing,
                                  skinned_mesh_indices);
        }
        {
            ScopedStartupTimer timer("first-frame emission lights");
            light_spawner_->spawn_emission(core_, instances, meshes, resolve_material);
        }
        const RendererFeatureStats stats = renderer_feature_stats();
        const std::string_view workflow = material_workflow_id(stats.material_workflow_active);
        std::printf(
            "  Renderer features: workflow=%.*s (%llu surfaces, %llu uses), "
            "textures=%llu sRGB/%llu legacy-color/%llu scalar/%llu normal/%llu HDR, "
            "IBL=%s, MSAA=%dx%s, A2C=%s, features=%llu\n",
            static_cast<int>(workflow.size()), workflow.data(),
            static_cast<unsigned long long>(
                stats.material_workflow_active ==
                        MaterialWorkflow::SpecularGlossinessExperimental
                    ? stats.experimental_workflow_surfaces
                    : stats.compatibility_workflow_surfaces),
            static_cast<unsigned long long>(
                stats.material_workflow_active ==
                        MaterialWorkflow::SpecularGlossinessExperimental
                    ? stats.experimental_workflow_instances
                    : stats.compatibility_workflow_instances),
            static_cast<unsigned long long>(stats.srgb_textures),
            static_cast<unsigned long long>(stats.legacy_color_textures),
            static_cast<unsigned long long>(stats.scalar_textures),
            static_cast<unsigned long long>(stats.normal_textures),
            static_cast<unsigned long long>(stats.hdr_textures),
            stats.ibl_prefiltered ? "prefiltered" : "fallback",
            stats.msaa_samples,
            stats.custom_msaa_resolve ? " custom-resolve" : "",
            stats.alpha_to_coverage ? "on" : "off",
            static_cast<unsigned long long>(stats.enabled_render_features));
        // Per-frame discipline counters: these builders run exactly
        // once — the per-frame path below must never re-enter this block.
        ++build_counters_.scene_builds;
        ++build_counters_.emission_spawns;
    }

    // Authored lamp-post / accent lights — spawned once after the scene
    // is built (lazy, since they arrive via set_scene_lights()).
    if (startup_scene_frame) {
        ScopedStartupTimer timer("first-frame authored lights");
        light_spawner_->spawn_authored(core_, scene_builder_->built());
    } else {
        light_spawner_->spawn_authored(core_, scene_builder_->built());
    }

    // Grass generation + per-frame wind/transform updates are handled
    // by GrassFeature (deferred to the first frame inside its update()).

    // Per-frame transform / light-position sync for animated instances.
    scene_builder_->update_transforms(
        core_, instances, dirty_instance_indices);
    light_spawner_->update_emission_positions(
        core_, instances, dirty_instance_indices);
    light_spawner_->update_operating_state(core_, lighting);
    // Per-frame discipline counter: the allowed per-frame mutation.
    ++build_counters_.transform_updates;

    // Shadow-casting point/spot lights. Re-applied every frame through
    // the LightingSystem configuration path (not inlined effect work — it only
    // toggles setShadowCaster on the backend-owned scene lights) so a runtime
    // toggle or budget change lands by the next frame. When the feature is
    // disabled this disables shadow casting on every candidate, keeping local
    // lighting bit-identical to the Shipping_Look.
    lighting_->configure_point_light_shadows(
        light_spawner_->light_infos(),
        config_.quality.shadow_light_budget,
        config_.quality.point_shadow_map_size,
        config_.features.point_light_shadows);
    result.timings.scene_sync_ms = elapsed_ms(stage_start);

    // Advance the frame-rate-independent animation clock. Use Filament's
    // getUserTime() so wind/particles stay smooth regardless of frame
    // rate. Features consume render_time_ via the FrameContext.
    render_time_ = capture_time_.value_or(static_cast<float>(renderer_->getUserTime()));

    // Update all render features (particles, lens flares, god rays,
    // volfog, grass, sky, clouds, heat haze). Ensures the shared depth
    // pre-pass target first if any depth-sampling feature is enabled.
    stage_start = FrameClock::now();
    bool need_depth_pass = false;
    if (startup_scene_frame) {
        ScopedStartupTimer timer("render feature phase");
        need_depth_pass = update_render_features(
            instances, dirty_instance_indices, meshes, resolve_material,
            camera, lighting, weather,
            viewport_w, viewport_h);
    } else {
        need_depth_pass = update_render_features(
            instances, dirty_instance_indices, meshes, resolve_material,
            camera, lighting, weather,
            viewport_w, viewport_h);
    }
    result.timings.feature_ms = elapsed_ms(stage_start);
    if(frame_generation_) {
        view_->setRenderTarget(nullptr);
        if(overlay_view_) overlay_view_->setRenderTarget(nullptr);
        if(frame_generation_->prepare(*mesh_store_,*texture_store_,instances,meshes,
                resolve_material,config_,camera,viewport_w,viewport_h,
                overlay_enabled_ && imgui_helper_ && overlay_draw_)) {
            std::vector<TemporalGrassPatch> patches;
            if(grass_feature_) grass_feature_->append_temporal_patches(patches);
            if(massive_foliage_feature_) massive_foliage_feature_->append_temporal_patches(patches);
            frame_generation_->prepare_grass(patches,instances,camera,render_time_);
            std::vector<TemporalEffectDraw> effects;
            for(const auto& feature : render_features_)
                if(feature->enabled()) feature->append_temporal_draws(effects);
            frame_generation_->prepare_effects(effects,camera,render_time_);
        }
    }
    bool coherent_heat = false;
    bool scene_capture_reserved = false;
    if (particle_feature_) {
        particle_feature_->set_temporal_distortion_enabled(false);
        FrameContext capture_frame;
        capture_frame.camera = &camera;
        capture_frame.viewport_w = viewport_w;
        capture_frame.viewport_h = viewport_h;
        // Preserve ordinary capture arbitration before temporal distortion
        // replaces the heat-haze composite. Releasing its physical target must
        // not silently enable an SSGI bounce that ordinary rendering skips.
        scene_capture_reserved = particle_feature_->wants_scene_capture(capture_frame);
        coherent_heat = frame_generation_ && frame_generation_->scene_target() &&
            config_.features.heat_haze && scene_capture_reserved;
        particle_feature_->set_temporal_distortion_enabled(coherent_heat);
    }
    if (coherent_heat) {
        coherent_heat = particle_feature_->prepare_temporal_distortion(render_ctx_, viewport_w, viewport_h);
        if (!frame_generation_->prepare_distortion(coherent_heat ?
                particle_feature_->temporal_distortion_texture() : nullptr, true)) coherent_heat = false;
        particle_feature_->set_temporal_distortion_enabled(coherent_heat);
    }

    // Depth pre-pass → main view → composite, inside one begin/endFrame.
    stage_start = FrameClock::now();
    if (startup_scene_frame) {
        ScopedStartupTimer timer("first-frame render views");
        result.presented =
            render_views(camera, viewport_w, viewport_h, need_depth_pass, scene_capture_reserved);
    } else {
        result.presented =
            render_views(camera, viewport_w, viewport_h, need_depth_pass, scene_capture_reserved);
    }
    result.timings.render_submit_ms = elapsed_ms(stage_start);

    // Filament publishes completed whole-frame GPU timestamps asynchronously.
    // The persistent stream consumes each frame ID once; it intentionally does
    // not claim to be a per-pass timestamp because the public API exposes no
    // such query.
    collect_completed_gpu_times();
    result.timings.gpu_ms = collected_gpu_ms_;
    return result;
}

// ── render_frame step: camera ───────────────────────────────────

void FilamentBackend::update_camera(const Camera& camera,
                                    const LightingParams& lighting,
                                    int viewport_w, int viewport_h) {
    profile::Scope profile_scope("Camera setup", "Rendering");
    view_->setViewport({0, 0,
        static_cast<uint32_t>(viewport_w),
        static_cast<uint32_t>(viewport_h)});

    float aspect = safe_aspect(viewport_w, viewport_h);
    fil_camera_->setProjection(camera.fov, aspect, 0.5, 2000.0,
                                filament::Camera::Fov::VERTICAL);
    glm::mat4 view_mat = camera.view_matrix();
    glm::mat4 camera_model = glm::inverse(view_mat);
    fil_camera_->setModelMatrix(glm_to_filament(camera_model));
    // Material global 1 carries the absolute user-world camera position.
    // Tessellation shaders convert Filament's camera-relative world position
    // back to user world before measuring the authored displacement fade.
    // Filament also copies these globals into shadow-map passes, keeping visible
    // and shadow displacement identical without mutating every material
    // instance each frame.
    view_->setMaterialGlobal(1, filament::math::float4{
        camera.position.x, camera.position.y, camera.position.z, 1.0f});

    // ── Physical camera  ────────────────────────

    // This is the View_Option_Group for the physically-based camera. It is
    // applied here in the existing update_camera step (exposure is a Camera
    // property, so it cannot live in configure_post_processing) — no per-effect
    // FilamentBackend member is added and no work is inlined into render_frame.
    // The config is read from the already-held config_ (clamped at the CLI layer
    // by clamp_camera_*).

    // Exposure and DOF have separate sources:
    //   * physical_camera controls the photometric exposure aperture, shutter
    //     and ISO;
    //   * benchmark camera tracks provide an authored focus distance and an
    //     effective f/2..f/16 DOF aperture without changing scene exposure.

    // Filament already includes Camera::getAperture() in its physical CoC.
    // cocScale is therefore ONLY cameraAperture / desiredDoFAperture, exactly
    // as Options.h specifies. Multiplying by 16/aperture while also setting the
    // camera to that aperture would apply the f-number twice.

    // Outside authored playback, physical-camera mode retains its configured
    // fixed 20 m focus. With neither source active we retain the existing
    // day/night exposure policy (Sunny-16 at noon, one stop wider at night) and
    // disable DOF, cleanly reverting runtime toggle-off.

    // This is deliberately the only Camera::setExposure call in the renderer.
    // LightingSystem used to restamp its baseline after this method whenever
    // weather or time-of-day changed, silently overriding live UI edits until
    // that adaptation stopped.
    constexpr float k_physical_camera_focus_distance = 20.0f;
    PhysicalCameraConfig defaults;
    const float daylight =
        std::clamp(lighting.sun_intensity / 110000.0f, 0.0f, 1.0f);
    defaults.aperture = 11.0f + daylight * (16.0f - 11.0f);
    const bool physical_camera = config_.features.physical_camera;
    const PhysicalCameraConfig& exposure = physical_camera
        ? config_.physical_camera
        : defaults;
    fil_camera_->setExposure(
        exposure.aperture, exposure.shutter_speed, exposure.sensitivity);

    if (config_.features.depth_of_field && camera.authored_dof.enabled) {
        fil_camera_->setFocusDistance(
            std::max(0.5001f, camera.authored_dof.focus_distance));
        filament::DepthOfFieldOptions dof;
        dof.enabled  = true;
        dof.cocScale = authored_dof_coc_scale(
            exposure.aperture, camera.authored_dof.desired_aperture);
        apply_dof_quality(dof, config_.quality.dof_quality);
        view_->setDepthOfFieldOptions(dof);
    } else if (config_.features.depth_of_field && physical_camera) {
        fil_camera_->setFocusDistance(k_physical_camera_focus_distance);
        filament::DepthOfFieldOptions dof;
        dof.enabled = true;
        dof.cocScale = 1.0f;
        apply_dof_quality(dof, config_.quality.dof_quality);
        view_->setDepthOfFieldOptions(dof);
    } else {
        view_->setDepthOfFieldOptions(filament::DepthOfFieldOptions{});
    }
}

// ── render_frame step: build persistent scene entities ──────────
// build_scene_entities was migrated to SceneBuilder::build
// (rhi/scene_builder.{h,cpp}); render_frame calls scene_builder_->build.

// ── render_frame step: spawn emission / authored lights ─────────
// spawn_emission_lights / spawn_authored_lights /
// update_emission_light_positions were migrated to the SceneLightSpawner
// subsystem (rhi/scene_light_spawner.{h,cpp}); render_frame calls
// light_spawner_->spawn_emission / spawn_authored / update_emission_positions.

// ── render_frame step: animated instance transforms ────────────
// update_animated_transforms was migrated to SceneBuilder::update_transforms
// (rhi/scene_builder.{h,cpp}); render_frame calls
// scene_builder_->update_transforms.

// ── render_frame step: update features ──────────────────────────

bool FilamentBackend::update_render_features(
    std::span<const Instance> instances,
    std::span<const int> dirty_instance_indices,
    std::span<const GpuMesh> meshes,
    const MaterialResolver& resolve_material,
    const Camera& camera,
    const LightingParams& lighting,
    const WeatherState& weather,
    int viewport_w, int viewport_h)
{
    profile::Scope profile_scope("Feature orchestration", "Rendering");
    if (viewport_w != shared_target_width_ || viewport_h != shared_target_height_) {
        view_->setRenderTarget(nullptr);
        scene_color_capture_->set_output_target(nullptr);
        for (auto& feature : render_features_) {
            feature->release_shared_targets(render_ctx_);
        }
        shared_target_width_ = viewport_w;
        shared_target_height_ = viewport_h;
    }

    // God rays, volumetric fog and heat haze all sample the half-res
    // depth pre-pass. Ensure that target NOW — before updating features
    // — so depth-consuming features can bind the depth texture in their
    // update() outside any render pass (Vulkan forbids descriptor-set
    // changes mid-render-pass). DepthPrePass::ensure() lazily creates
    // the depth texture / render target / view on first call.
    bool need_depth_pass = false;
    for (auto& f : render_features_) {
        if (f->enabled() && f->needs_depth_prepass()) {
            need_depth_pass = true;
            break;
        }
    }
    if (need_depth_pass) {
        depth_prepass_->ensure(viewport_w, viewport_h);
        // If the F target / depth material failed to build, depth_texture()
        // is null. Disable the pass for this frame: render_views then skips the
        // depth render (it never aims or renders a null-target view), and the
        // null frame.depth_texture below leaves every consumer on its
        // zero-reading depth placeholder, i.e. no occlusion. The renderer keeps
        // running without the scene-depth pass and without crashing.
        if (!depth_prepass_->depth_texture()) {
            need_depth_pass = false;
        } else {
            depth_prepass_->view()->setMaterialGlobal(
                1, filament::math::float4{
                    camera.position.x, camera.position.y,
                    camera.position.z, 1.0f});
            // Build / sync the depth-proxy renderables that populate the proxy
            // Scene the depth view renders. Gated on need_depth_pass so neither
            // the one-time build nor the per-frame transform sync runs when no
            // enabled consumer samples scene depth.
            sync_depth_proxies(
                instances, dirty_instance_indices, meshes, resolve_material);
        }
    }

    // Scene-colour capture (SSGI). Engage only when a feature wants the
    // rendered scene colour. Done BEFORE building the FrameContext so the
    // requesting feature can bind last frame's colour in its update() below —
    // outside any render pass (Vulkan forbids descriptor changes mid-pass).
    // ensure() builds the capture/history targets + blit lazily; if the blit
    // material is unavailable it returns false and we fall back to direct
    // rendering.
    want_scene_color_ = false;
    for (auto& f : render_features_) {
        if (f->enabled() && f->wants_scene_color()) { want_scene_color_ = true; break; }
    }
    if (want_scene_color_ && !scene_color_capture_->ensure(viewport_w, viewport_h)) {
        want_scene_color_ = false;
    }

    FrameContext frame;
    frame.camera               = &camera;
    frame.lighting             = &lighting;
    // Read-only per-frame weather state, threaded in exactly like
    // `lighting`. `weather` lives on render_frame's stack — no backend
    // member — so features read it through the FrameContext only.
    frame.weather              = &weather;
    frame.time                 = render_time_;
    if (capture_time_) {
        const auto step = static_cast<uint64_t>(std::llround(static_cast<double>(*capture_time_) * 60.0));
        frame.capture_frame = step > 0 ? step - 1 : 0;
    }
    frame.viewport_w           = viewport_w;
    frame.viewport_h           = viewport_h;
    frame.instances            = instances;
    frame.meshes               = meshes;
    frame.resolve_material     = &resolve_material;
    // A lazily built consumer must not bind retained, old-size depth when no
    // pass was requested yet. Its next update receives the newly ensured target.
    frame.depth_texture        = need_depth_pass ? depth_prepass_->depth_texture() : nullptr;
    frame.depth_w              = need_depth_pass ? depth_prepass_->width() : 0;
    frame.depth_h              = need_depth_pass ? depth_prepass_->height() : 0;
    // Expose the depth proxy Scene so procedural-grass features can register
    // depth-only proxies for their blades (clouds/fog/god rays/SSGI/splash
    // occlusion). Only when the depth pass actually runs this frame — otherwise
    // there is no scene-depth texture to occlude into.
    frame.depth_proxy_scene    = need_depth_pass ? depth_prepass_->proxy_scene()
                                                 : nullptr;
    frame.scene_color_texture  = want_scene_color_
                               ? scene_color_capture_->history_texture() : nullptr;
    if (frame.scene_color_texture) {
        frame.scene_color_view_proj = scene_color_capture_->history_view_proj();
    }
    frame.scene_light_entities = light_spawner_->light_entities();
    frame.scene_lights         = light_spawner_->light_infos();
    frame.floating_offsets     = light_spawner_->floating_offsets();
    update_features(frame);

    return need_depth_pass;
}

// ── render_frame step: depth-proxy build / transform sync ───────

void FilamentBackend::sync_depth_proxies(
    std::span<const Instance> instances,
    std::span<const int> dirty_instance_indices,
    std::span<const GpuMesh> meshes,
    const MaterialResolver& resolve_material)
{
    profile::Scope profile_scope("Depth proxy synchronization", "Rendering");
    // The proxy renderables mirror the visible scene's opaque geometry into the
    // depth view's proxy Scene, sharing the main VB/IB and the shared
    // `heaven_depth` MaterialInstance (owned by depth_prepass_). Build them ONCE
    // — deferred to the first frame a consumer needs depth, by which point the
    // scene geometry is uploaded and the depth material exists (mirroring how
    // grass / lens flares defer their build). build() leaves built() false if
    // the depth material instance or proxy Scene is not ready yet, so this
    // retries on a later frame; thereafter only transforms are synced so the
    // depth silhouette tracks animated islands / skinned meshes without
    // regenerating geometry.
    if (!depth_proxy_builder_->built()) {
        depth_proxy_builder_->build(
            core_, *mesh_store_,
            depth_prepass_->material(),
            depth_prepass_->masked_material(),
            *texture_store_,
            depth_prepass_->proxy_scene(),
            instances, meshes, resolve_material,
            config_.tessellation.scale, config_.tessellation.distance,
            config_.features.gpu_instancing,
            instancing_excluded_mesh_indices(meshes, resolve_material),
            config_.features.gpu_skinning, config_.tessellation.enabled);
    } else {
        depth_proxy_builder_->update_transforms(
            core_, instances, dirty_instance_indices);
    }
}

// Single source of the skinned-mesh-index set shared by SceneBuilder and
// DepthProxyBuilder. A mesh is skinned when any of its surfaces was
// uploaded from an animated source, including CPU and bone-limit fallback.
// Returns empty when GPU instancing is disabled — the per-instance path is then
// used for everything (Shipping_Look parity). Because the grouping helper is
// pure and deterministic, giving both builders this identical set guarantees
// identical instanced chunks/members, so the depth silhouette cannot drift from
// the visible instanced scene.
std::unordered_set<int> FilamentBackend::instancing_excluded_mesh_indices(
    std::span<const GpuMesh> meshes,
    const MaterialResolver& resolve_material) const
{
    std::unordered_set<int> excluded;
    if (!config_.features.gpu_instancing) return excluded;
    for (int mi = 0; std::cmp_less(mi, meshes.size()); ++mi) {
        for (const auto& surf : meshes[mi].surfaces) {
            // Animated_Objects are never instanced: they keep their
            // own deformation path, including CPU and bone-limit fallback.
            if (mesh_store_->skinned_info(surf.handle).skinned_source) {
                excluded.insert(mi);
                break;
            }
            // Alpha-masked foliage is excluded too — an instanced masked depth
            // proxy writes a wrong silhouette into the half-res scene-depth
            // texture, so volumetric fog / god rays / SSGI composite in front
            // of the see-through trees (dithered halos). Keep foliage on the
            // proven per-instance path; opaque repeated meshes still instance.
            if (resolve_material(meshes[mi].path, surf.name).alpha_ref > 0.0f) {
                excluded.insert(mi);
                break;
            }
        }
    }
    return excluded;
}

// Prepare UI resources before Filament commits surface-material descriptors in
// beginFrame. Building ImGui commands after rendering the scene leaves newly
// created/modified overlay materials uncommitted for the overlay draw.
bool FilamentBackend::prepare_overlay(int viewport_w, int viewport_h) {
    profile::Scope profile_scope("Build overlay geometry", "UI");
    if (!overlay_enabled_ || !imgui_helper_ || !overlay_draw_) return false;
    overlay_view_->setViewport({0, 0,
        static_cast<uint32_t>(viewport_w),
        static_cast<uint32_t>(viewport_h)});
    imgui_helper_->render(overlay_dt_,
        [this](filament::Engine*, filament::View*) {
            overlay_draw_();
        });
    return true;
}

// ── render_frame step: depth pre-pass + main + composite ────────

bool FilamentBackend::render_views(const Camera& camera,
                                   int viewport_w, int viewport_h,
                                   bool need_depth_pass, bool scene_capture_reserved)
{
    profile::Scope profile_scope("Submit views", "Rendering");
    // Depth pre-pass setup: aim the depth camera and pull post-effect
    // quads out of the scene so they don't pollute the depth render.
    // on_depth_prepass_begin is called for EVERY enabled feature (not
    // just those that request the pass) so passive participants like
    // lens flares — whose alpha quads would write bright depth "voids"
    // that god rays / volfog read as sky — get pulled too.
    if (need_depth_pass) {
        depth_prepass_->aim(camera, viewport_w, viewport_h);
        for (auto& f : render_features_) {
            if (f->enabled()) f->on_depth_prepass_begin(render_ctx_);
        }
    }

    // Scene-capture / composite path (heat haze). A feature opts in per
    // frame via wants_scene_capture(); if so it points the MAIN view at
    // its capture target in begin_scene_capture() and renders its own
    // composite view after the main view. When no feature wants capture,
    // the main view renders straight to the swap chain — unless the
    // scene-colour capture path (SSGI) claims it below.
    // HDR display output: ensure the fp16 HDR scene target and bind the encode
    // quad's source BEFORE beginFrame (descriptor updates must be outside any
    // render pass). When active, every pass that would target the swap chain is
    // redirected to this target, and the encode pass (below) PQ-encodes it to
    // the swap chain as the final step. final_rt is nullptr on the SDR path, so
    // all the setRenderTarget(final_rt) calls keep SDR behaviour identical.
    hdr_active_ = false;
    if (hdr_ && hdr_output_) {
        // ensure() may resize the targets. Do not let a borrowed view retain
        // the previous target across its destruction.
        if (overlay_view_) overlay_view_->setRenderTarget(nullptr);
        hdr_active_ = hdr_output_->ensure(viewport_w, viewport_h);
        if (!hdr_active_) {
            throw std::runtime_error("HDR output encoding could not be initialized");
        }
    }
    filament::RenderTarget* const final_rt = hdr_active_ ? hdr_output_->scene_rt() :
        (frame_generation_ ? frame_generation_->scene_target() : nullptr);
    render_ctx_.output_target = final_rt;

    IRenderFeature* capture_feature = nullptr;
    IRenderFeature* scene_color_feature = nullptr;
    bool scene_color_active = false;
    {
        FrameContext cf;
        cf.camera     = &camera;
        cf.viewport_w = viewport_w;
        cf.viewport_h = viewport_h;
        for (auto& f : render_features_) {
            if (f->enabled() && f->wants_scene_capture(cf)) {
                capture_feature = f.get();
                break;
            }
        }

        // Scene-colour capture path (SSGI). Active only when a feature wants the
        // rendered scene colour and ordinary capture arbitration permits it.
        // A temporal heat field retains the ordinary composite's reservation
        // even though it no longer redirects the main-view render target.
        // The main view renders CLEAN (the SSGI quad lives in the feature's own
        // composite view, not the main scene) into a stable capture texture,
        // which we blit to the swap chain; SSGI then composites its bounce OVER
        // that blitted scene, and the clean texture is copied into the history
        // target for the next frame. That keeps SSGI out of the history it
        // gathers from and avoids alternating the main view's render target
        // under Filament's temporal post-processing.
        scene_color_active = want_scene_color_ && !capture_feature && !scene_capture_reserved &&
                             scene_color_capture_->valid();
        if (scene_color_active) {
            for (auto& f : render_features_) {
                if (f->enabled() && f->wants_scene_color()) {
                    scene_color_feature = f.get();
                    break;
                }
            }
        }

        if (capture_feature) {
            capture_feature->begin_scene_capture(render_ctx_, cf);
        } else if (scene_color_active) {
            scene_color_capture_->aim(camera, viewport_w, viewport_h);
            view_->setRenderTarget(scene_color_capture_->current_rt());
            // The blit composite lands on the HDR scene target under HDR (so the
            // encode pass picks it up), or the swap chain (nullptr) on SDR.
            scene_color_capture_->set_output_target(final_rt);
        } else {
            // Direct path: render straight to the swap chain (SDR) or the HDR
            // scene target (HDR).
            view_->setRenderTarget(final_rt);
        }
    }

    const bool overlay_ready = prepare_overlay(viewport_w, viewport_h);
    const auto overlay_blend_mode = overlay_ready
        ? overlay_view_->getBlendMode() : filament::View::BlendMode::TRANSLUCENT;
    const bool generating = frame_generation_ && frame_generation_->scene_target();
    if(generating && overlay_ready) frame_generation_->bind_ui(*overlay_view_);
    if (hdr_active_) {
        if (overlay_ready) {
            hdr_output_->bind_sdr_layer(*overlay_view_);
        }
        hdr_output_->prepare(viewport_w, viewport_h, true, overlay_ready);
    }

    // ── Render ──────────────────────────────────────────────────
    // Inside beginFrame: render the depth pre-pass (post-effect quads
    // pulled), re-add the quads, then render the main view + composites.
    bool depth_quads_pulled = need_depth_pass;
    bool presented = false;
    prepare_screenshot_target(viewport_w, viewport_h);
    const auto input_begin_lo = input_response_ ? InputResponseRecorder::now_ns() : 0;
    const bool accepted = renderer_->beginFrame(swapchain_, capture_frame_timestamp());
    const auto input_begin_hi = input_response_ ? InputResponseRecorder::now_ns() : 0;
    if (accepted) {
        if (need_depth_pass) {
            // Render the depth pre-pass while the post-effect quads are
            // removed from the scene.
            { profile::Scope submit_scope("Auxiliary depth view submission", "Rendering"); renderer_->render(depth_prepass_->view()); }
            for (auto& f : render_features_) {
                if (f->enabled()) f->on_depth_prepass_end(render_ctx_);
            }
            depth_quads_pulled = false;
        }
        if(generating) {
            if (particle_feature_) particle_feature_->render_temporal_distortion(render_ctx_);
            frame_generation_->render_temporal();
        }

        // Render the main view. The post-effect quads are back in the
        // scene and sample the current frame's depth. With the composite
        // path enabled this writes to the feature's capture texture
        // instead of the swap chain.
        { profile::Scope submit_scope("Main scene view submission", "Rendering"); renderer_->render(view_); }

        // Composite pass: the capture feature blits the captured scene
        // to the swap chain and draws its overlay (heat-haze) quads.
        if (capture_feature) {
            { profile::Scope submit_scope("Heat haze composite submission", "Rendering"); capture_feature->render_composite(render_ctx_); }
        } else if (scene_color_active) {
            // SSGI scene-colour path: the CLEAN main view rendered into the
            // stable capture texture; blit it to the display, then draw the
            // SSGI bounce OVER it (the SSGI quad is in the feature's own view,
            // not the captured main view), so SSGI never gathers its own output.
            // Finally copy this frame's CLEAN render into the history texture
            // used by next frame's gather.
            { profile::Scope submit_scope("Scene color blit submission", "Rendering"); scene_color_capture_->render_blit(); }
            if (scene_color_feature) {
                { profile::Scope submit_scope("SSGI composite submission", "Rendering"); scene_color_feature->render_scene_color_composite(render_ctx_, final_rt); }
            }
            { profile::Scope submit_scope("Scene history copy submission", "Rendering"); scene_color_capture_->render_history_copy(); }
            scene_color_capture_->advance(camera, viewport_w, viewport_h);
        }

        // SDR draws UI directly over the scene. HDR draws its unchanged sRGB
        // shader into a separate target, then converts and composites those
        // colors in linear Rec.2020 before applying PQ exactly once.
        if (overlay_ready) {
            { profile::Scope submit_scope("UI view submission", "Rendering"); renderer_->render(overlay_view_); }
        }
        if (hdr_active_) {
            { profile::Scope submit_scope("HDR encode submission", "Rendering"); hdr_output_->encode(); }
        }

        if(generating) {
            frame_generation_->render_composite();
            frame_generation_->submit(true,camera);
        }
        else if(frame_generation_) frame_generation_->submit(true,camera);
        submit_screenshot(viewport_w, viewport_h);
        { profile::Scope submit_scope("End frame submission", "Rendering"); renderer_->endFrame(); }
        if (input_response_) {
            input_response_->accepted_frame(input_response_attempt_, input_response_submission_,
                input_begin_lo, input_begin_hi, overlay_ready);
        }
        finish_screenshot();
        presented = true;

    }
    if(!presented && frame_generation_) frame_generation_->submit(false,camera);
    if(generating && overlay_ready) frame_generation_->unbind_ui();
    if (hdr_active_ && overlay_ready) {
        overlay_view_->setRenderTarget(nullptr);
        overlay_view_->setBlendMode(overlay_blend_mode);
    }

    // If beginFrame() returned false the depth pre-pass never ran, so the
    // quads pulled by on_depth_prepass_begin were never re-added inside the
    // frame above. Re-add them now so the scene + each feature's in_scene_
    // stay consistent for the next frame and for any runtime toggle (the
    // add/remove in on_enabled_changed assumes the renderables really are in
    // the scene while enabled). This runs OUTSIDE any render pass.
    if (depth_quads_pulled) {
        for (auto& f : render_features_) {
            if (f->enabled()) f->on_depth_prepass_end(render_ctx_);
        }
    }
    return presented;
}
void FilamentBackend::present() {
    // Filament handles presentation internally via endFrame()
    // Nothing to do here for the OpenGL backend
}

void FilamentBackend::request_frame_generation(FrameGenerationMode mode) {
    config_.frame_generation=mode;
    if(!core_.engine) return;
    bool trace_requested=false;
#if HEAVEN_HAS_FSR3
    trace_requested=frame_generation_platform_ &&
        engine_->getPlatform()==frame_generation_platform_.get() &&
        frame_generation_platform_->presentation_trace_requested();
#endif
    // An explicit presentation diagnostic also needs accepted real-source IDs
    // while generation is Off. The existing Runtime's inactive path queues only
    // that native attribution callback; ordinary Off creates no Runtime.
    if(!frame_generation_ && (mode!=FrameGenerationMode::Off || trace_requested)) {
        // A Streamline-created device is tied to that provider until backend
        // recreation. Never also interpose the FSR swap chain on its resources.
        const auto select_fsr_platform = [&]() -> FrameGenerationPlatform* {
#if HEAVEN_HAS_FSR3
            if (engine_->getBackend() != filament::Engine::Backend::VULKAN) return nullptr;
#if HEAVEN_HAS_DLSS
            if (dlss_platform_) return nullptr;
#endif
            return frame_generation_platform_.get();
#else
            return nullptr;
#endif
        };
        const auto select_dlss_platform = [&]() -> DlssPlatform* {
#if HEAVEN_HAS_DLSS
            return engine_->getBackend() == filament::Engine::Backend::VULKAN ? dlss_platform_.get() : nullptr;
#else
            return nullptr;
#endif
        };
        frame_generation_=std::make_unique<FrameGenerationRuntime>(core_, select_fsr_platform(), select_dlss_platform());
    }
    if(frame_generation_) frame_generation_->request(mode);
}

void FilamentBackend::destroy_frame_generation() {
    if(!frame_generation_) return;
    frame_generation_->drain();
    frame_generation_->unbind_ui();
    if(view_) view_->setRenderTarget(nullptr);
    if(overlay_view_) overlay_view_->setRenderTarget(nullptr);
    if(scene_color_capture_) scene_color_capture_->set_output_target(nullptr);
    for(auto& feature:render_features_) feature->release_shared_targets(render_ctx_);
    render_ctx_.output_target=nullptr;
    frame_generation_.reset();
}

void FilamentBackend::begin_application_frame(uint64_t application_frame) {
    if (frame_generation_) frame_generation_->begin_application_frame(application_frame);
}
void FilamentBackend::end_application_simulation() {
    if (frame_generation_) frame_generation_->end_simulation();
}
void FilamentBackend::native_window_message(uint32_t message) {
    if (frame_generation_) frame_generation_->native_window_message(message);
}

FrameGenerationStatus FilamentBackend::frame_generation_status() const {
    return frame_generation_ ? frame_generation_->status() : FrameGenerationStatus{};
}
#if defined(HEAVEN_FG_FAULT_INJECTION) && HEAVEN_FG_FAULT_INJECTION
bool FilamentBackend::debug_frame_generation_out_of_date_once() {
#if HEAVEN_HAS_FSR3
    if (frame_generation_platform_ && frame_generation_) {
        frame_generation_platform_->debug_out_of_date_once();
        return true;
    }
#endif
    return false;
}
bool FilamentBackend::debug_frame_generation_work_failure_once() {
#if HEAVEN_HAS_FSR3
    if (frame_generation_platform_ && frame_generation_) {
        frame_generation_platform_->debug_generated_work_failure_once();
        return true;
    }
#endif
    return false;
}
uint64_t FilamentBackend::debug_frame_generation_work_failure_source() const {
#if HEAVEN_HAS_FSR3
    if (frame_generation_platform_)
        return frame_generation_platform_->debug_generated_work_failure_source();
#endif
    return 0;
}
uint64_t FilamentBackend::debug_frame_generation_provider_attempts() const {
    return frame_generation_ ? frame_generation_->debug_provider_record_attempts() : 0;
}
#endif

Result<void> FilamentBackend::update_mesh_vertices(uint64_t surface_handle,
                                           const Vertex* vertices,
                                           uint32_t vertex_count) {
    auto result = mesh_store_->update_vertices(surface_handle, vertices, vertex_count);
    if (frame_generation_) {
        if (result) frame_generation_->set_positions(surface_handle,{vertices,vertex_count});
        else frame_generation_->invalidate_history();
    }
    return result;
}

// ── Per-frame GPU skinning bone update ──────────────────────────

// Push the interpolated bone matrices for one Animated_Object to its
// renderable(s). SceneBuilder recorded every renderable built with
// .skinning() enabled under its mesh index (the caster + non-caster split
// means up to two entities share the same bone set), so we route the same
// matrices to each. RenderableManager::setBones updates ONLY the bone
// transforms — no VB/IB rebuild — satisfying the build-once discipline
// An unknown / non-skinned mesh index reports no entities and is
// a safe no-op.
Result<void> FilamentBackend::update_skinned_bones(
        int mesh_index, std::span<const glm::mat4> bone_matrices) {
    if(frame_generation_) frame_generation_->set_bones(mesh_index,bone_matrices);
    if (!engine_) {
        return std::unexpected(Error("Filament engine not initialized"));
    }
    if (!scene_builder_) {
        return {};
    }
    auto entities = scene_builder_->skinned_entities(mesh_index);
    std::span<const utils::Entity> proxy_entities =
        depth_proxy_builder_
            ? depth_proxy_builder_->skinned_proxy_entities(mesh_index)
            : std::span<const utils::Entity>{};
    if ((entities.empty() && proxy_entities.empty()) || bone_matrices.empty()) {
        return {};
    }

    // Convert glm::mat4 → filament::math::mat4f once; the same bone set is
    // shared by every renderable of the object.
    std::vector<filament::math::mat4f> mats;
    mats.reserve(bone_matrices.size());
    for (const auto& m : bone_matrices) {
        mats.push_back(glm_to_filament(m));
    }

    auto& rcm = engine_->getRenderableManager();
    for (const auto& entity : entities) {
        auto ri = rcm.getInstance(entity);
        if (!ri) continue;
        rcm.setBones(ri, mats.data(),
                     static_cast<size_t>(mats.size()), /*offset=*/0);
    }
    // Push the SAME matrices to the depth-proxy renderables so the linear-depth
    // silhouette tracks the animated mesh in lockstep with the visible scene
    // (otherwise the depth proxy stays at the rest pose and the screen-space
    // depth consumers composite atmosphere in front of the moving mesh).
    for (const auto& entity : proxy_entities) {
        auto ri = rcm.getInstance(entity);
        if (!ri) continue;
        rcm.setBones(ri, mats.data(),
                     static_cast<size_t>(mats.size()), /*offset=*/0);
    }
    return {};
}

// Whether the Animated_Object at `mesh_index` is actually GPU-skinned this run
// (per-object fallback). The decision is only knowable once the
// persistent scene has been built — SceneBuilder records every skinning-enabled
// renderable under its mesh index during build(). Until then we report
// std::nullopt so the main loop defers skinning that object for the frame
// (its renderable, if any, simply shows its rest pose). Once built, a non-empty
// skinned-entity list means GPU skinning took effect; an empty one means the
// skinned upload/enable did not (e.g. the bone count exceeded Filament's limit,
// so MeshStore left the surfaces unmarked), and the object must fall back to the
// CPU_Skinning_Path.
std::optional<bool> FilamentBackend::gpu_skinning_active(int mesh_index) const {
    if (!scene_builder_ || !scene_builder_->built()) {
        return std::nullopt;
    }
    return !scene_builder_->skinned_entities(mesh_index).empty();
}

// ── Overlay (Dear ImGui via filagui) ────────────────────────────

// The benchmark's status/result/menu overlay is drawn with Dear ImGui,
// translated into a Filament View by filagui::ImGuiHelper. The helper
// owns its own Scene/Camera and configures the View we hand it as a
// translucent 2D layer (ORTHO projection, post-processing OFF, shadows
// OFF — see ImGuiHelper's constructor). We keep that overlay View as a
// SEPARATE additive layer;  renders it on top of the main view
// inside the existing begin/endFrame so every 3D render feature stays
// enabled.

namespace {

// Translate an SDL keycode into the matching ImGuiKey. Mirrors the
// vendored reference backend (third_party/imgui/backends/imgui_impl_sdl2.cpp).
ImGuiKey sdl_keycode_to_imgui(SDL_Keycode keycode) {
    switch (keycode) {
        case SDLK_TAB: return ImGuiKey_Tab;
        case SDLK_LEFT: return ImGuiKey_LeftArrow;
        case SDLK_RIGHT: return ImGuiKey_RightArrow;
        case SDLK_UP: return ImGuiKey_UpArrow;
        case SDLK_DOWN: return ImGuiKey_DownArrow;
        case SDLK_PAGEUP: return ImGuiKey_PageUp;
        case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
        case SDLK_HOME: return ImGuiKey_Home;
        case SDLK_END: return ImGuiKey_End;
        case SDLK_INSERT: return ImGuiKey_Insert;
        case SDLK_DELETE: return ImGuiKey_Delete;
        case SDLK_BACKSPACE: return ImGuiKey_Backspace;
        case SDLK_SPACE: return ImGuiKey_Space;
        case SDLK_RETURN: return ImGuiKey_Enter;
        case SDLK_ESCAPE: return ImGuiKey_Escape;
        case SDLK_QUOTE: return ImGuiKey_Apostrophe;
        case SDLK_COMMA: return ImGuiKey_Comma;
        case SDLK_MINUS: return ImGuiKey_Minus;
        case SDLK_PERIOD: return ImGuiKey_Period;
        case SDLK_SLASH: return ImGuiKey_Slash;
        case SDLK_SEMICOLON: return ImGuiKey_Semicolon;
        case SDLK_EQUALS: return ImGuiKey_Equal;
        case SDLK_LEFTBRACKET: return ImGuiKey_LeftBracket;
        case SDLK_BACKSLASH: return ImGuiKey_Backslash;
        case SDLK_RIGHTBRACKET: return ImGuiKey_RightBracket;
        case SDLK_BACKQUOTE: return ImGuiKey_GraveAccent;
        case SDLK_CAPSLOCK: return ImGuiKey_CapsLock;
        case SDLK_SCROLLLOCK: return ImGuiKey_ScrollLock;
        case SDLK_NUMLOCKCLEAR: return ImGuiKey_NumLock;
        case SDLK_PRINTSCREEN: return ImGuiKey_PrintScreen;
        case SDLK_PAUSE: return ImGuiKey_Pause;
        case SDLK_KP_0: return ImGuiKey_Keypad0;
        case SDLK_KP_1: return ImGuiKey_Keypad1;
        case SDLK_KP_2: return ImGuiKey_Keypad2;
        case SDLK_KP_3: return ImGuiKey_Keypad3;
        case SDLK_KP_4: return ImGuiKey_Keypad4;
        case SDLK_KP_5: return ImGuiKey_Keypad5;
        case SDLK_KP_6: return ImGuiKey_Keypad6;
        case SDLK_KP_7: return ImGuiKey_Keypad7;
        case SDLK_KP_8: return ImGuiKey_Keypad8;
        case SDLK_KP_9: return ImGuiKey_Keypad9;
        case SDLK_KP_PERIOD: return ImGuiKey_KeypadDecimal;
        case SDLK_KP_DIVIDE: return ImGuiKey_KeypadDivide;
        case SDLK_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
        case SDLK_KP_MINUS: return ImGuiKey_KeypadSubtract;
        case SDLK_KP_PLUS: return ImGuiKey_KeypadAdd;
        case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDLK_KP_EQUALS: return ImGuiKey_KeypadEqual;
        case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
        case SDLK_LSHIFT: return ImGuiKey_LeftShift;
        case SDLK_LALT: return ImGuiKey_LeftAlt;
        case SDLK_LGUI: return ImGuiKey_LeftSuper;
        case SDLK_RCTRL: return ImGuiKey_RightCtrl;
        case SDLK_RSHIFT: return ImGuiKey_RightShift;
        case SDLK_RALT: return ImGuiKey_RightAlt;
        case SDLK_RGUI: return ImGuiKey_RightSuper;
        case SDLK_APPLICATION: return ImGuiKey_Menu;
        case SDLK_0: return ImGuiKey_0;
        case SDLK_1: return ImGuiKey_1;
        case SDLK_2: return ImGuiKey_2;
        case SDLK_3: return ImGuiKey_3;
        case SDLK_4: return ImGuiKey_4;
        case SDLK_5: return ImGuiKey_5;
        case SDLK_6: return ImGuiKey_6;
        case SDLK_7: return ImGuiKey_7;
        case SDLK_8: return ImGuiKey_8;
        case SDLK_9: return ImGuiKey_9;
        case SDLK_a: return ImGuiKey_A;
        case SDLK_b: return ImGuiKey_B;
        case SDLK_c: return ImGuiKey_C;
        case SDLK_d: return ImGuiKey_D;
        case SDLK_e: return ImGuiKey_E;
        case SDLK_f: return ImGuiKey_F;
        case SDLK_g: return ImGuiKey_G;
        case SDLK_h: return ImGuiKey_H;
        case SDLK_i: return ImGuiKey_I;
        case SDLK_j: return ImGuiKey_J;
        case SDLK_k: return ImGuiKey_K;
        case SDLK_l: return ImGuiKey_L;
        case SDLK_m: return ImGuiKey_M;
        case SDLK_n: return ImGuiKey_N;
        case SDLK_o: return ImGuiKey_O;
        case SDLK_p: return ImGuiKey_P;
        case SDLK_q: return ImGuiKey_Q;
        case SDLK_r: return ImGuiKey_R;
        case SDLK_s: return ImGuiKey_S;
        case SDLK_t: return ImGuiKey_T;
        case SDLK_u: return ImGuiKey_U;
        case SDLK_v: return ImGuiKey_V;
        case SDLK_w: return ImGuiKey_W;
        case SDLK_x: return ImGuiKey_X;
        case SDLK_y: return ImGuiKey_Y;
        case SDLK_z: return ImGuiKey_Z;
        case SDLK_F1: return ImGuiKey_F1;
        case SDLK_F2: return ImGuiKey_F2;
        case SDLK_F3: return ImGuiKey_F3;
        case SDLK_F4: return ImGuiKey_F4;
        case SDLK_F5: return ImGuiKey_F5;
        case SDLK_F6: return ImGuiKey_F6;
        case SDLK_F7: return ImGuiKey_F7;
        case SDLK_F8: return ImGuiKey_F8;
        case SDLK_F9: return ImGuiKey_F9;
        case SDLK_F10: return ImGuiKey_F10;
        case SDLK_F11: return ImGuiKey_F11;
        case SDLK_F12: return ImGuiKey_F12;
        default: return ImGuiKey_None;
    }
}

// Mouse button: map SDL's 1-based button id to ImGui's 0-based index
// (left=0, right=1, middle=2, X1=3, X2=4). Returns -1 for unknown buttons.
int sdl_mouse_button_to_imgui(uint8_t sdl_button) {
    switch (sdl_button) {
        case SDL_BUTTON_LEFT:   return 0;
        case SDL_BUTTON_RIGHT:  return 1;
        case SDL_BUTTON_MIDDLE: return 2;
        case SDL_BUTTON_X1:     return 3;
        case SDL_BUTTON_X2:     return 4;
        default:                return -1;
    }
}

struct OverlayDisplayMetrics {
    int logical_w = 1;
    int logical_h = 1;
    int drawable_w = 1;
    int drawable_h = 1;
    float framebuffer_scale_x = 1.0f;
    float framebuffer_scale_y = 1.0f;
    float ui_scale = 1.0f;
};

OverlayDisplayMetrics overlay_display_metrics(SDL_Window* window,
                                               int fallback_w,
                                               int fallback_h) {
    OverlayDisplayMetrics m;
    m.drawable_w = std::max(1, fallback_w);
    m.drawable_h = std::max(1, fallback_h);

    if (window) {
        SDL_GetWindowSize(window, &m.logical_w, &m.logical_h);
        get_drawable_size(window, m.drawable_w, m.drawable_h);
    } else {
        m.logical_w = m.drawable_w;
        m.logical_h = m.drawable_h;
    }

    m.logical_w = std::max(1, m.logical_w);
    m.logical_h = std::max(1, m.logical_h);
    m.drawable_w = std::max(1, m.drawable_w);
    m.drawable_h = std::max(1, m.drawable_h);
    m.framebuffer_scale_x = static_cast<float>(m.drawable_w) /
                            static_cast<float>(m.logical_w);
    m.framebuffer_scale_y = static_cast<float>(m.drawable_h) /
                            static_cast<float>(m.logical_h);

    float dpi_scale = 1.0f;
    float screen_scale = 1.0f;
    const int display = window ? SDL_GetWindowDisplayIndex(window) : 0;
    if (display >= 0) {
        float ddpi = 0.0f;
        if (SDL_GetDisplayDPI(display, &ddpi, nullptr, nullptr) == 0 &&
            ddpi > 0.0f) {
            dpi_scale = ddpi / 96.0f;
        }

        SDL_DisplayMode desktop;
        if (SDL_GetDesktopDisplayMode(display, &desktop) == 0 &&
            desktop.w > 0 && desktop.h > 0) {
            const float sx = static_cast<float>(desktop.w) / 1920.0f;
            const float sy = static_cast<float>(desktop.h) / 1080.0f;
            screen_scale = std::min(sx, sy);
        }
    }

    m.ui_scale = std::clamp(std::max(dpi_scale, screen_scale), 1.0f, 2.5f);
    return m;
}

void apply_overlay_ui_scale(float scale) {
    static bool base_style_captured = false;
    static ImGuiStyle base_style;

    ImGuiStyle& style = ImGui::GetStyle();
    if (!base_style_captured) {
        base_style = style;
        base_style_captured = true;
    }

    style = base_style;
    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
}

} // anonymous namespace

void FilamentBackend::overlay_init(const AppConfig& /*config*/) {
    if (!engine_) {
        std::fprintf(stderr, "FilamentBackend: overlay_init skipped (no engine)\n");
        return;
    }
    if (overlay_enabled_) return;  // idempotent

    // Dedicated overlay View. The ImGuiHelper attaches its own Scene and
    // Camera and switches this View into a translucent 2D layer; we keep
    // it separate from the main view_ so the overlay composites additively
    // without touching the 3D pipeline.
    overlay_view_ = engine_->createView();
    overlay_view_->setName("overlay");

    // Construct the helper. An empty font path makes Dear ImGui fall back
    // to its built-in "proggy" atlas (compiled into the library) so the
    // overlay needs NO external font asset. If construction fails for any
    // reason, log it and leave the overlay disabled — the renderer keeps
    // running without an overlay (graceful degradation).
    try {
        imgui_helper_ = std::make_unique<filagui::ImGuiHelper>(
            engine_, overlay_view_, utils::Path{});
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "FilamentBackend: overlay disabled — ImGuiHelper construction "
            "failed: %s\n", e.what());
        imgui_helper_.reset();
    } catch (...) {
        std::fprintf(stderr,
            "FilamentBackend: overlay disabled — ImGuiHelper construction "
            "threw an unknown exception\n");
        imgui_helper_.reset();
    }

    if (!imgui_helper_) {
        if (overlay_view_) {
            engine_->destroy(overlay_view_);
            overlay_view_ = nullptr;
        }
        overlay_enabled_ = false;
        return;
    }

    const OverlayDisplayMetrics metrics =
        overlay_display_metrics(window_, 1, 1);
    apply_overlay_ui_scale(metrics.ui_scale);
    overlay_ui_scale_ = metrics.ui_scale;
    imgui_helper_->setDisplaySize(metrics.logical_w, metrics.logical_h,
                                  metrics.framebuffer_scale_x,
                                  metrics.framebuffer_scale_y);

    overlay_enabled_ = true;
    std::printf("FilamentBackend: overlay initialised (Dear ImGui via filagui, "
                "built-in font, ui scale %.2f, framebuffer scale %.2fx%.2f)\n",
                metrics.ui_scale, metrics.framebuffer_scale_x,
                metrics.framebuffer_scale_y);
}

void FilamentBackend::overlay_begin_frame(float dt, int w, int h) {
    if (!overlay_enabled_) return;
    // ImGui uses SDL window coordinates for input and layout. Filament renders
    // to Vulkan drawable pixels, so pass the logical size plus framebuffer
    // scale separately and scale the style/font from monitor DPI/extent.
    const OverlayDisplayMetrics metrics =
        overlay_display_metrics(window_, w, h);
    if (std::abs(metrics.ui_scale - overlay_ui_scale_) > 0.01f) {
        apply_overlay_ui_scale(metrics.ui_scale);
        overlay_ui_scale_ = metrics.ui_scale;
        std::printf("FilamentBackend: overlay ui scale = %.2f "
                    "(framebuffer %.2fx%.2f)\n",
                    metrics.ui_scale, metrics.framebuffer_scale_x,
                    metrics.framebuffer_scale_y);
    }
    imgui_helper_->setDisplaySize(metrics.logical_w, metrics.logical_h,
                                  metrics.framebuffer_scale_x,
                                  metrics.framebuffer_scale_y);
    overlay_dt_ = dt;
}

bool FilamentBackend::overlay_handle_event(const void* sdl_event) {
    if (!overlay_enabled_ || !sdl_event) return false;

    const SDL_Event* e = static_cast<const SDL_Event*>(sdl_event);
    ImGuiIO& io = ImGui::GetIO();
#ifdef __ANDROID__
    // SDL_TOUCH_MOUSEID is Uint32(-1); avoid the macro's C-style cast.
    constexpr Uint32 touch_mouse_id = static_cast<Uint32>(-1);
#endif

    switch (e->type) {
        case SDL_MOUSEMOTION:
#ifdef __ANDROID__
            io.AddMouseSourceEvent(e->motion.which == touch_mouse_id
                ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
#endif
            io.AddMousePosEvent(static_cast<float>(e->motion.x),
                                static_cast<float>(e->motion.y));
            return io.WantCaptureMouse;

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int button = sdl_mouse_button_to_imgui(e->button.button);
            if (button >= 0) {
#ifdef __ANDROID__
                io.AddMouseSourceEvent(e->button.which == touch_mouse_id
                    ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
#endif
                io.AddMouseButtonEvent(button, e->type == SDL_MOUSEBUTTONDOWN);
            }
            return io.WantCaptureMouse;
        }

        case SDL_MOUSEWHEEL:
            // preciseX/preciseY (SDL 2.0.18+) give smooth trackpad scroll.
            io.AddMouseWheelEvent(e->wheel.preciseX, e->wheel.preciseY);
            return io.WantCaptureMouse;

        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            // Keep modifier state in sync first, then queue the key event.
            SDL_Keymod mods = static_cast<SDL_Keymod>(e->key.keysym.mod);
            io.AddKeyEvent(ImGuiMod_Ctrl,  (mods & KMOD_CTRL)  != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (mods & KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt,   (mods & KMOD_ALT)   != 0);
            io.AddKeyEvent(ImGuiMod_Super, (mods & KMOD_GUI)   != 0);

            ImGuiKey key = sdl_keycode_to_imgui(e->key.keysym.sym);
            if (key != ImGuiKey_None) {
                io.AddKeyEvent(key, e->type == SDL_KEYDOWN);
            }
            return io.WantCaptureKeyboard;
        }

        case SDL_TEXTINPUT:
            io.AddInputCharactersUTF8(e->text.text);
            return io.WantCaptureKeyboard;

        case SDL_WINDOWEVENT:
            // Resize: refresh the ImGui display size so widgets relayout.
            // Not "consumed" — the renderer still needs the new extent.
            if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                e->window.event == SDL_WINDOWEVENT_RESIZED) {
                const OverlayDisplayMetrics metrics =
                    overlay_display_metrics(window_, e->window.data1,
                                            e->window.data2);
                imgui_helper_->setDisplaySize(metrics.logical_w,
                                              metrics.logical_h,
                                              metrics.framebuffer_scale_x,
                                              metrics.framebuffer_scale_y);
            }
            return false;

        default:
            return false;
    }
}

void FilamentBackend::overlay_set_draw(OverlayDraw build_widgets) {
    // Stored for overlay_render(), which invokes
    // imgui_helper_->render(overlay_dt_, cb) each frame and renders the overlay
    // view on top of the scene.
    overlay_draw_ = std::move(build_widgets);
}

bool FilamentBackend::frame_pacing_disabled() const {
    // IMMEDIATE presentation applies no vsync cap. Adaptive rendering supplies
    // a finite GPU-time target to its resolution controller; fixed/scored
    // rendering additionally keeps DisplayInfo::refreshRate at zero.
    return true;
}

IRhiBackend::BackendBuildCounters FilamentBackend::debug_build_counters() const {
    // Copy the per-frame discipline counters maintained in render_frame, then
    // fill grass_generations from the grass feature on demand (the feature owns
    // its deferred-generation count). This proves the per-frame path mutated
    // only transforms — scene/emission/grass builders ran at most once.
    BackendBuildCounters c = build_counters_;
    c.grass_generations = grass_feature_ ? grass_feature_->generation_count() : 0;
    c.foliage_generations =
        massive_foliage_feature_ ? massive_foliage_feature_->generation_count() : 0;

    // Rain-streak / splash build counters (weather spec).
    // Like grass_generations, these are owned by the feature (RainFeature) which
    // builds its streak and splash geometry once on the first active frame and
    // animates it through uniforms thereafter. The aggregation is sourced from
    // the feature on demand here so the backend keeps no per-effect member
    // beyond the grass-style observer pointer used for instrumentation.
    c.rain_streak_builds = rain_feature_ ? rain_feature_->streak_build_count() : 0;
    c.splash_builds      = rain_feature_ ? rain_feature_->splash_build_count() : 0;
    return c;
}

const IRenderFeature* FilamentBackend::debug_find_feature(const char* name) const {
    // Test-only: scan the owning feature vector for the entry whose name()
    // matches. Used by the depth-pre-pass participation smoke case to assert
    // RainFeature::needs_depth_prepass() is honored after an active rain frame
    // The renderer never calls this; it adds
    // no per-effect access path beyond this read-only scan.
    if (!name) return nullptr;
    for (const auto& f : render_features_) {
        if (f && f->name() && std::strcmp(f->name(), name) == 0) {
            return f.get();
        }
    }
    return nullptr;
}

int FilamentBackend::debug_count_features(const char* name) const {
    // Test-only: count how many registered features have a matching name().
    // debug_find_feature() proves existence (and fetches the first match);
    // this proves cardinality so the feature-registration smoke case can
    // assert EXACTLY ONE "rain" and one "wet-reflections" feature were
    // registered in init_features.
    // The renderer never calls this; it adds no per-effect access path
    // beyond this read-only scan. Mirrors debug_find_feature().
    if (!name) return 0;
    int count = 0;
    for (const auto& f : render_features_) {
        if (f && f->name() && std::strcmp(f->name(), name) == 0) {
            ++count;
        }
    }
    return count;
}

const filament::Texture* FilamentBackend::debug_depth_texture() const {
    // Test-only: expose the shared half-res depth pre-pass texture. Non-null
    // only after the depth pass has run (some enabled feature returned
    // needs_depth_prepass()==true and the backend rendered the depth view).
    // Guarded against a null subsystem so the accessor is safe before
    // init_pipelines() builds it.
    return depth_prepass_ ? depth_prepass_->depth_texture() : nullptr;
}

void FilamentBackend::resize([[maybe_unused]] int width,
                             [[maybe_unused]] int height) {
    // Recreate the swap chain for the new drawable size. The viewport that
    // render_frame receives already follows the new window size; this rebuilds
    // the presentation surface so the resolution change actually takes effect.
    // No-op if the engine never came up.
    if (!engine_ || !window_) return;
    if (input_response_) input_response_->retire_pending("surface resize");

    if (hdr_) {
        if (auto supported = validate_hdr_surface(); !supported) {
            throw std::runtime_error(supported.error().message);
        }
    }

    // Re-fetch the native window handle. On Wayland this refreshes the cached
    // width/height in the tagged native-window struct (get_native_window reads
    // SDL_GetWindowSize), so the recreated swap chain matches the new extent; on
    // X11 the surface extent is queried from the live window.
    void* native_win = get_native_window(window_);
    if (!native_win) return;

    // Drain in-flight GPU work before tearing down the old swap chain so no
    // frame still references it, then rebuild with the same HDR flag the
    // constructor used. (width/height come from the native window; the
    // arguments are accepted for interface symmetry / logging.)
    if(frame_generation_) {
        frame_generation_->drain();
        frame_generation_->invalidate_history();
    }
    engine_->flushAndWait();
    if (swapchain_) {
        engine_->destroy(swapchain_);
        swapchain_ = nullptr;
    }
    uint64_t swapchain_flags = 0;
    if (hdr_) {
        swapchain_flags |= filament::SwapChain::CONFIG_HDR10_ST2084;
    }
    swapchain_ = engine_->createSwapChain(native_win, swapchain_flags);
}

void FilamentBackend::set_feature_enabled(const char* name, bool on) {
    // Supported runtime API: drive a registered feature's runtime toggle
    // through the IRenderFeature::set_enabled() API, passing the backend's own
    // render_ctx_ so the feature's on_enabled_changed() add/remove path runs
    // exactly as it would for any runtime toggle. Keeping render_ctx_ access
    // here (rather than exposing a mutable feature pointer + the RenderContext
    // to callers) keeps the toggle encapsulated. The in-UI configuration
    // controller maps live feature toggles to feature names and calls this;
    // the toggle-isolation / round-trip smoke case reaches it via
    // debug_set_feature_enabled(). No-op if no feature matches (or name is
    // null), so a toggle that maps to no registered feature leaves the
    // renderer unchanged.
    if (!name) return;
    if (std::strcmp(name, "massive-foliage") == 0 && grass_feature_) {
        grass_feature_->set_enabled(render_ctx_, !on);
    } else if (std::strcmp(name, "grass") == 0 &&
               on && massive_foliage_feature_) {
        massive_foliage_feature_->set_enabled(render_ctx_, false);
    }
    for (const auto& f : render_features_) {
        if (f && f->name() && std::strcmp(f->name(), name) == 0) {
            f->set_enabled(render_ctx_, on);
            return;
        }
    }
}

void FilamentBackend::set_camera_config(
    bool enabled, const PhysicalCameraConfig& config) {
    config_.features.physical_camera = enabled;
    config_.physical_camera = config;
}

void FilamentBackend::debug_set_feature_enabled(const char* name, bool on) {
    // Thin alias retained for the existing gated smoke cases. Delegates to the
    // supported runtime API so both the tests and the in-UI configuration
    // controller exercise the same toggle path

    set_feature_enabled(name, on);
}

} // namespace heaven
