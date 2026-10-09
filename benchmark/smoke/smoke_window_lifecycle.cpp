// Exercise the live resize/fullscreen/reload path, including a loading screen
// after swap-chain replacement. HEAVEN_SMOKE_WINDOW_LIFECYCLE=1 opts in.
#include "smoke_cases.h"

#include "engine/camera.h"
#include "rhi/rhi.h"
#include "rhi/filament_backend.h"

#include <SDL2/SDL.h>
#include <imgui.h>
#include <array>
#include <cstdio>
#include <limits>

namespace heaven::smoke {

namespace {

bool render_at_size(const CaseContext& ctx, WindowHandle& window,
                    int expected_width, int expected_height) {
    // Compositor configure events are asynchronous, including leaving
    // fullscreen. Wait at most two seconds for the requested drawable size.
    for (int attempt = 0; attempt < 200; ++attempt) {
        SDL_PumpEvents();
        get_drawable_size(window.window, window.width, window.height);
        if (window.width == expected_width && window.height == expected_height) break;
        SDL_Delay(10);
    }
    if (window.width != expected_width || window.height != expected_height) {
        std::fprintf(stderr, "[smoke_window_lifecycle] expected %dx%d, got %dx%d\n",
                     expected_width, expected_height, window.width, window.height);
        return false;
    }
    ctx.backend.resize(window.width, window.height);
    auto* overlay_context = ImGui::GetCurrentContext();
    constexpr std::array progress_steps{0.0f, 0.5f, 1.0f, 2.0f, -1.0f,
        std::numeric_limits<float>::quiet_NaN()};
    for (const float progress : progress_steps) {
        ctx.backend.show_loading_screen(ctx.config.data_dir,
            progress >= 0.0f ? "Uploading scene meshes" : "Reading scene assets",
            window.width, window.height, progress);
        if (ImGui::GetCurrentContext() != overlay_context) {
            std::fprintf(stderr, "[smoke_window_lifecycle] loading progress replaced "
                                "the application's ImGui context\n");
            return false;
        }
    }
    Camera camera;
    const auto lighting = lighting_for_time_of_day(ctx.config.time_of_day);
    const WeatherState weather;
    const TonemapParams tonemap;
    const MaterialResolver resolve = [](const std::string&, const std::string&) {
        return GpuMaterial{};
    };
    // FIFO presentation and the renderer's GPU fences are asynchronous. Four
    // immediate attempts can all be skipped after a resize or splash draw.
    // Keep the four attempts, then allow the same bounded settle time for an
    // actual presentation rather than treating a pending GPU fence as failure.
    const auto deadline = SDL_GetTicks64() + 2000;
    int presented_frames = 0;
    int attempted_frames = 0;
    do {
        SDL_PumpEvents();
        presented_frames += ctx.backend.render_frame({}, {}, {}, resolve, camera,
            lighting, weather, tonemap, window.width, window.height).presented;
        ++attempted_frames;
        if (presented_frames == 0 || attempted_frames < 4) SDL_Delay(10);
    } while ((presented_frames == 0 || attempted_frames < 4) && SDL_GetTicks64() < deadline);
    if (presented_frames == 0) {
        std::fprintf(stderr, "[smoke_window_lifecycle] %dx%d: no presentation "
                            "after %d attempts within 2 seconds\n", window.width,
                     window.height, attempted_frames);
    }
    std::printf("[smoke_window_lifecycle] %dx%d: %d presentations in %d attempts\n",
                window.width, window.height, presented_frames, attempted_frames);
    return presented_frames > 0;
}

} // namespace

CaseResult run_window_lifecycle_case(const CaseContext& ctx, WindowHandle& window) {
    if (auto initialized = ctx.backend.init_pipelines(ctx.config); !initialized) {
        std::fprintf(stderr, "[smoke_window_lifecycle] %s\n",
                     initialized.error().message.c_str());
        return CaseResult::Fail;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    // Loading owns a separate ImGui context; initialize the main overlay first
    // so status/progress draws must restore a real application-owned context.
    backend.overlay_init(ctx.config);
    if (!ImGui::GetCurrentContext()) {
        std::fprintf(stderr, "[smoke_window_lifecycle] main overlay context unavailable\n");
        return CaseResult::Fail;
    }
    const auto* god_rays = backend.debug_find_feature("god_rays");
    const bool restore_god_rays = god_rays && god_rays->enabled();
    // A disabled material retains its samplers. Resize while it is inactive,
    // then enable it again to check that no retired depth binding survives.
    backend.set_feature_enabled("god_rays", false);
    window.set_drawable_size(320, 240);
    bool ok = render_at_size(ctx, window, 320, 240);
    window.set_drawable_size(640, 360);
    ok = render_at_size(ctx, window, 640, 360) && ok;
    backend.set_feature_enabled("god_rays", restore_god_rays);
    ok = render_at_size(ctx, window, 640, 360) && ok;

    SDL_DisplayMode desktop{};
    const int display = SDL_GetWindowDisplayIndex(window.window);
    if (SDL_GetDesktopDisplayMode(display, &desktop) != 0 ||
        SDL_SetWindowFullscreen(window.window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
        std::fprintf(stderr, "[smoke_window_lifecycle] fullscreen failed: %s\n",
                     SDL_GetError());
        ok = false;
    } else {
        // SDL display modes use desktop coordinates on scaled Wayland. Wait
        // for the fullscreen client extent, then render at its physical size.
        int logical_w = 0, logical_h = 0;
        for (int attempt = 0; attempt < 200; ++attempt) {
            SDL_PumpEvents();
            SDL_GetWindowSize(window.window, &logical_w, &logical_h);
            if (logical_w == desktop.w && logical_h == desktop.h) break;
            SDL_Delay(10);
        }
        int drawable_w = 0, drawable_h = 0;
        get_drawable_size(window.window, drawable_w, drawable_h);
        if (logical_w != desktop.w || logical_h != desktop.h ||
            drawable_w <= 0 || drawable_h <= 0) {
            std::fprintf(stderr, "[smoke_window_lifecycle] fullscreen extent did not settle\n");
            ok = false;
        } else {
            ok = render_at_size(ctx, window, drawable_w, drawable_h) && ok;
        }
    }

    // Restore the small window even if a previous check failed.
    if (SDL_SetWindowFullscreen(window.window, 0) != 0) ok = false;
    window.set_drawable_size(ctx.width, ctx.height);
    ok = render_at_size(ctx, window, ctx.width, ctx.height) && ok;
    return ok ? CaseResult::Pass : CaseResult::Fail;
}

} // namespace heaven::smoke
