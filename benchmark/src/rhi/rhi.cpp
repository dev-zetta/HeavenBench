/*
 * HeavenBench — RHI common: window management
 *
 * Filament owns the rendering context entirely (Vulkan preferred,
 * OpenGL fallback) and creates it from the SDL window handle. The
 * application's only job here is to hand SDL the right window
 * flags and pass the SDL_Window pointer through to the backend.
 */
#include "rhi/rhi.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#ifdef __APPLE__
#include <SDL2/SDL_metal.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace heaven {

void get_drawable_size(SDL_Window* window, int& width, int& height) {
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_VULKAN) {
        SDL_Vulkan_GetDrawableSize(window, &width, &height);
#ifdef __APPLE__
    } else if (SDL_GetWindowFlags(window) & SDL_WINDOW_METAL) {
        SDL_Metal_GetDrawableSize(window, &width, &height);
#endif
    } else {
        SDL_GL_GetDrawableSize(window, &width, &height);
    }
}

namespace {

void get_window_extents(SDL_Window* window,
                        int& logical_w, int& logical_h,
                        int& drawable_w, int& drawable_h) {
    SDL_GetWindowSize(window, &logical_w, &logical_h);
    get_drawable_size(window, drawable_w, drawable_h);
}

void fit_window_to_drawable(SDL_Window* window,
                            int requested_w, int requested_h) {
#ifdef __ANDROID__
    // Android's Activity owns the Surface extent. Desktop size negotiation can
    // replace SDL's logical dimensions without resizing that native Surface,
    // leaving the viewport and touch/overlay coordinates inconsistent with the
    // swap chain. Retain the dimensions delivered by Android's surface callback.
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && SDL_strcmp(driver, "android") == 0) {
        return;
    }
#endif

    // A tiny window quantizes a fractional compositor scale (64/51 is not
    // exactly 1.25). Re-evaluate after each settled resize so a one-pixel
    // estimate error can converge; stop after three bounded attempts when the
    // compositor cannot represent the requested physical extent exactly.
    for (int resize_attempt = 0; resize_attempt < 3; ++resize_attempt) {
        int logical_w = 0;
        int logical_h = 0;
        int drawable_w = 0;
        int drawable_h = 0;
        get_window_extents(
            window, logical_w, logical_h, drawable_w, drawable_h);

        if (logical_w <= 0 || logical_h <= 0 ||
            drawable_w <= 0 || drawable_h <= 0 ||
            (drawable_w == requested_w && drawable_h == requested_h)) {
            return;
        }

        const double scale_x =
            static_cast<double>(drawable_w) / static_cast<double>(logical_w);
        const double scale_y =
            static_cast<double>(drawable_h) / static_cast<double>(logical_h);
        if (scale_x <= 0.0 || scale_y <= 0.0) {
            return;
        }

        const int target_logical_w = std::max(
            1, static_cast<int>(std::lround(requested_w / scale_x)));
        const int target_logical_h = std::max(
            1, static_cast<int>(std::lround(requested_h / scale_y)));
        SDL_SetWindowSize(window, target_logical_w, target_logical_h);
        SDL_SetWindowPosition(
            window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

        // Wayland size negotiation is asynchronous. Give SDL a short bounded
        // opportunity to consume the configure event before Filament creates its
        // swap chain from the native handle.
        for (int attempt = 0; attempt < 20; ++attempt) {
            SDL_PumpEvents();
            get_window_extents(
                window, logical_w, logical_h, drawable_w, drawable_h);
            if (drawable_w == requested_w && drawable_h == requested_h) {
                break;
            }
            SDL_Delay(5);
        }
    }
}

} // namespace

// ── WindowHandle ────────────────────────────────────────────────

WindowHandle::~WindowHandle() {
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
        SDL_Quit();
    }
}

WindowHandle::WindowHandle(WindowHandle&& o) noexcept
    : window(o.window), api_context(o.api_context),
      width(o.width), height(o.height) {
    o.window = nullptr;
    o.api_context = nullptr;
}

WindowHandle& WindowHandle::operator=(WindowHandle&& o) noexcept {
    if (this != &o) {
        if (window) { SDL_DestroyWindow(window); SDL_Quit(); }
        window = o.window; api_context = o.api_context;
        width = o.width; height = o.height;
        o.window = nullptr; o.api_context = nullptr;
    }
    return *this;
}

void WindowHandle::set_title(std::string_view title) {
    if (window) SDL_SetWindowTitle(window, std::string(title).c_str());
}

void WindowHandle::set_drawable_size(
    int requested_width, int requested_height) {
    if (!window) return;
    fit_window_to_drawable(window, requested_width, requested_height);
    int logical_w = 0;
    int logical_h = 0;
    get_window_extents(
        window, logical_w, logical_h, width, height);
}

// ── Window creation ─────────────────────────────────────────────

Result<WindowHandle> create_window(
        const AppConfig& config, GraphicsApi /*api*/, std::string_view title) {
#ifdef __APPLE__
    if (config.hdr) {
        return std::unexpected(Error(
            "--hdr currently requires native Wayland HDR10 and is unavailable on macOS"));
    }
#else
    const char* explicit_driver = std::getenv("SDL_VIDEODRIVER");
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    const char* session_type = std::getenv("XDG_SESSION_TYPE");
    const bool native_wayland_session =
        (wayland_display && *wayland_display) ||
        (session_type && std::string_view(session_type) == "wayland");

    // HDR display output requires the native Wayland WSI: under X11/XWayland the
    // Vulkan surface exposes no HDR colorspace (the probe confirmed the
    // X11 path advertises only SDR sRGB formats). Force SDL onto the Wayland
    // backend before SDL_Init so the surface can advertise HDR10. Respect an
    // explicit user override only if it already selects wayland.
    if (config.hdr) {
        if (explicit_driver &&
            std::string_view(explicit_driver) != "wayland") {
            std::fprintf(stderr,
                "HDR: SDL_VIDEODRIVER=%s was set but --hdr requires the Wayland "
                "backend; overriding to wayland.\n", explicit_driver);
        }
        // Use SDL's own cross-platform hint instead of POSIX setenv. This sets
        // the same video-driver selection SDL reads at init time, without
        // depending on a POSIX-only API. SDL_HINT_OVERRIDE forces the value even
        // if SDL_VIDEODRIVER was already present in the environment, preserving
        // the prior overwrite=1 behaviour.
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "wayland", SDL_HINT_OVERRIDE);
    } else if (!explicit_driver && native_wayland_session) {
        // SDL2 often selects X11 first even inside a Wayland session. Under
        // compositor scaling, XWayland can expose a nominal 4K Vulkan drawable
        // while presenting the window at half the intended desktop size.
        // Prefer native Wayland for SDR so SDR and HDR use the same sizing
        // path. An explicit SDL_VIDEODRIVER (including run.sh --x11) wins.
        SDL_SetHintWithPriority(
            SDL_HINT_VIDEODRIVER, "wayland", SDL_HINT_OVERRIDE);
        std::printf(
            "Window: native Wayland selected for the Wayland desktop "
            "(use --x11 to override)\n");
    }
#endif

#ifdef SDL_MAIN_HANDLED
    // Our own main bypasses SDL_main, which normally marks startup ready.
    SDL_SetMainReady();
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        const auto error = std::format("SDL init failed: {}", SDL_GetError());
        SDL_Quit();
        return std::unexpected(Error(error));
    }

    // SDL_WINDOW_VULKAN works on both X11 and Wayland. Native Wayland is
    // preferred on a Wayland desktop; an explicit SDL_VIDEODRIVER can select
    // X11/XWayland instead. Our Filament platform patch handles whichever
    // native handle SDL returns, and the selected driver is logged below.
    // Configuration width/height are benchmark drawable pixels, not desktop
    // coordinate units. Without ALLOW_HIGHDPI SDL_Vulkan_GetDrawableSize()
    // is permitted to return the logical window extent on a scaled desktop
    // (commonly 1920x1080 at 200% for a 4K panel). HDR happened to avoid that
    // on affected systems because it forces the native Wayland path. Request a
    // high-DPI Vulkan drawable for both SDR and HDR so the cached extent below,
    // every offscreen target, and the reported benchmark resolution all use
    // physical pixels.
    uint32_t flags =
        SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN | SDL_WINDOW_ALLOW_HIGHDPI;
    if (config.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;

    WindowHandle wh;
    wh.width  = config.width;
    wh.height = config.height;

    wh.window = SDL_CreateWindow(
        std::string(title).c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        config.width, config.height, flags);

    if (!wh.window && !config.hdr) {
        const std::string vulkan_error = SDL_GetError();
#ifdef __APPLE__
        SDL_Quit();
        return std::unexpected(Error(std::format(
            "macOS requires Vulkan through MoltenVK: {}", vulkan_error)));
#else
#if defined(__linux__) && !defined(__ANDROID__)
        // Filament's Linux OpenGL platform uses GLX. A native Wayland window
        // cannot be handed to it. Switch only an automatically selected driver;
        // keep an explicit Wayland request visible as a startup error.
        const char* driver = SDL_GetCurrentVideoDriver();
        if (driver && std::string_view(driver) != "x11") {
            if (explicit_driver) {
                SDL_Quit();
                return std::unexpected(Error(std::format(
                    "Vulkan window failed: {}. The OpenGL fallback requires "
                    "X11; retry with SDL_VIDEODRIVER=x11.", vulkan_error)));
            }
            SDL_Quit();
            SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "x11", SDL_HINT_OVERRIDE);
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
                const auto error = std::format(
                    "Vulkan window failed: {}. X11 fallback failed: {}",
                    vulkan_error, SDL_GetError());
                SDL_Quit();
                return std::unexpected(Error(error));
            }
        }
#endif
        std::printf("Window: Vulkan unavailable (%s); trying OpenGL\n",
                    vulkan_error.c_str());
        flags = (flags & ~static_cast<uint32_t>(SDL_WINDOW_VULKAN)) |
                SDL_WINDOW_OPENGL;
        wh.window = SDL_CreateWindow(
            std::string(title).c_str(), SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED, config.width, config.height, flags);
        if (!wh.window) {
            const auto error = std::format(
                "Vulkan window failed: {}. OpenGL window failed: {}",
                vulkan_error, SDL_GetError());
            SDL_Quit();
            return std::unexpected(Error(error));
        }
#endif
    }

    if (!wh.window) {
        const auto error = std::format("Window creation failed: {}", SDL_GetError());
        SDL_Quit();
        return std::unexpected(Error(error));
    }

    if (!config.fullscreen) {
        wh.set_drawable_size(config.width, config.height);
    }

    std::printf("Window: SDL video driver = %s\n", SDL_GetCurrentVideoDriver());
    std::printf("Window: requested drawable size = %dx%d%s\n",
                config.width, config.height,
                config.fullscreen ? " fullscreen" : "");

    int logical_w = 0, logical_h = 0;
    int drawable_w = 0, drawable_h = 0;
    get_window_extents(
        wh.window, logical_w, logical_h, drawable_w, drawable_h);
    std::printf("Window: logical client size = %dx%d\n", logical_w, logical_h);

    int border_top = 0, border_left = 0, border_bottom = 0, border_right = 0;
    if (SDL_GetWindowBordersSize(wh.window, &border_top, &border_left,
                                 &border_bottom, &border_right) == 0) {
        const int outer_w = logical_w + border_left + border_right;
        const int outer_h = logical_h + border_top + border_bottom;
        std::printf("Window: decoration borders = L%d T%d R%d B%d (outer %dx%d)\n",
                    border_left, border_top, border_right, border_bottom,
                    outer_w, outer_h);
    }

    // Hard-require the Wayland backend under --hdr (see the forcing above). If
    // SDL fell back to X11/XWayland (e.g. no Wayland session), there is no HDR
    // colorspace to select, so fail fast with a clear message rather than
    // silently presenting SDR.
    if (config.hdr) {
        const char* drv = SDL_GetCurrentVideoDriver();
        if (!drv || std::string_view(drv) != "wayland") {
            return std::unexpected(Error(std::format(
                "--hdr requires the Wayland window backend, but SDL is using "
                "'{}'. Run inside a Wayland session (no X11/XWayland).",
                drv ? drv : "(none)")));
        }
    }

    // Start with the OS cursor visible and free so the overlay menu bar is
    // immediately clickable. Free-fly mouse-look is engaged on demand by the
    // main loop (hold right mouse button), which toggles relative mouse mode;
    // we must NOT lock+hide the cursor here or the menu can never be reached.
    SDL_SetRelativeMouseMode(SDL_FALSE);

    // For Vulkan on HiDPI / scaled displays the actual drawable
    // pixel size differs from the requested logical window size.
    // Filament needs the *drawable* dimensions to size the swap
    // chain correctly — using the logical size leaves the bottom
    // edge of the viewport rendered into pixels that aren't part
    // of the present rectangle.
    wh.width = drawable_w;
    wh.height = drawable_h;
    std::printf("Window: drawable size = %dx%d\n", wh.width, wh.height);
    if (wh.width != config.width || wh.height != config.height) {
        std::fprintf(stderr,
            "Window: requested %dx%d drawable pixels, but the compositor "
            "provided %dx%d. Benchmark results will record the actual "
            "drawable extent.\n",
            config.width, config.height, wh.width, wh.height);
    }

    return wh;
}

} // namespace heaven
