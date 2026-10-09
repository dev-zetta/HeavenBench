/*
 * HeavenBench — renderer-linked smoke harness (single main + gates)
 *
 * This is the ONE main() across the smoke sources (the target GLOBs the smoke
 * directory, so a second main() would be a duplicate-symbol link error). It
 * owns the gate, the window/backend lifecycle, and the per-case dispatch; the
 * actual assertions live in the per-case files (smoke_frame_pacing.cpp,
 * smoke_overlay_render.cpp). See smoke_cases.h for the contract.
 *
 * Flow:
 *   1. Gate on HEAVEN_SMOKE=1 (opt-in) and a display being present. If either
 *      is missing, exit 77 so CTest reports the harness as skipped.
 *   2. Bring up a small real window + Filament backend ONCE, exactly the way
 *      main.cpp does (create_window + create_backend), so the cases run
 *      against the live backend. An opted-in bring-up failure exits 1.
 *   3. Run each registered case, printing per-case PASS/SKIP/FAIL.
 *   4. Exit 1 if some case FAILED, 77 if all skipped, otherwise 0.
 */

#include "smoke_cases.h"
#include "smoke_displacement_surface.h"
#include "smoke_height_sampler_lit_lifecycle.h"

#include "engine/asset_file_system.h"
#include "engine/path_resolver.h"
#include "rhi/rhi.h"
#include "engine/types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <utility>
#include <vector>

namespace heaven::smoke {

bool smoke_opt_in() {
    const char* v = std::getenv("HEAVEN_SMOKE");
    return v != nullptr && std::strcmp(v, "1") == 0;
}

bool display_available() {
    const char* x11 = std::getenv("DISPLAY");
    const char* wl  = std::getenv("WAYLAND_DISPLAY");
    return (x11 != nullptr && x11[0] != '\0') ||
           (wl  != nullptr && wl[0]  != '\0');
}

} // namespace heaven::smoke

namespace {

constexpr const char* kTag = "[smoke]";

using heaven::smoke::CaseContext;
using heaven::smoke::CaseResult;

// The registry of smoke cases the harness runs, in order. Adding a case is a
// one-line edit here plus its own smoke_*.cpp file (no new main()).
struct CaseEntry {
    const char* name;
    CaseResult (*fn)(const CaseContext&);
};

const CaseEntry kCases[] = {
    {"frame_pacing",        &heaven::smoke::run_frame_pacing_case},
    {"gpu_info",            &heaven::smoke::run_gpu_info_case},
    {"overlay_render",      &heaven::smoke::run_overlay_render_case},
    {"hdr_output",          &heaven::smoke::run_hdr_output_case},
    {"lens_flare",          &heaven::smoke::run_lens_flare_case},
    {"perframe_discipline", &heaven::smoke::run_perframe_discipline_case},
    {"wet_reflections_capability",
                            &heaven::smoke::run_wet_reflections_capability_case},
    {"glass_refraction",    &heaven::smoke::run_glass_refraction_case},
    {"advanced_materials",  &heaven::smoke::run_advanced_materials_case},
    {"shadows_lighting",    &heaven::smoke::run_shadows_lighting_case},
    {"shadow_receivers",    &heaven::smoke::run_shadow_receivers_case},
    {"particle_lighting",   &heaven::smoke::run_particle_lighting_case},
    {"night_lighting",      &heaven::smoke::run_night_lighting_case},
    {"lamp_ground",         &heaven::smoke::run_lamp_ground_case},
    {"cascade_shadow_culling", &heaven::smoke::run_cascade_shadow_culling_case},
    {"camera_grading",      &heaven::smoke::run_camera_grading_case},
    {"dynamic_resolution",  &heaven::smoke::run_dynamic_resolution_case},
    {"soft_shadows_toggle", &heaven::smoke::run_soft_shadows_toggle_case},
    {"contact_shadows",     &heaven::smoke::run_contact_shadows_case},
    {"contact_shadow_stability", &heaven::smoke::run_contact_shadow_stability_case},
    {"finite_sun_contact", &heaven::smoke::run_finite_sun_contact_case},
    {"specular_occlusion", &heaven::smoke::run_specular_occlusion_case},
    {"displacement_filter", &heaven::smoke::run_displacement_filter_case},
    {"height_sample_batch", &heaven::smoke::run_height_sample_batch_case},
    {"displacement_surface", &heaven::smoke::run_displacement_surface_case},
    {"render_test_assets", &heaven::smoke::run_render_test_assets_case},
    {"msaa_fallback",       &heaven::smoke::run_msaa_fallback_case},
    {"point_light_shadows", &heaven::smoke::run_point_light_shadows_case},
    {"volumetric_clouds",   &heaven::smoke::run_volumetric_clouds_case},
    {"ssgi",                &heaven::smoke::run_ssgi_case},
    {"massive_foliage",     &heaven::smoke::run_massive_foliage_case},
    {"rain_asset_failure",  &heaven::smoke::run_rain_asset_failure_case},
    {"rain_build_once",     &heaven::smoke::run_rain_build_once_case},
    {"rain_depth_prepass",  &heaven::smoke::run_rain_depth_prepass_case},
    {"feature_registration",
                            &heaven::smoke::run_feature_registration_case},
    {"gpu_skinning",        &heaven::smoke::run_gpu_skinning_case},
    {"bloom_strength",      &heaven::smoke::run_bloom_strength_case},
    {"gpu_instancing",      &heaven::smoke::run_gpu_instancing_case},
    {"culling",             &heaven::smoke::run_culling_case},
    {"microprofiler",       &heaven::smoke::run_microprofiler_case},
    {"rain_toggle_roundtrip",
                            &heaven::smoke::run_rain_toggle_roundtrip_case},
    {"fps_bar",             &heaven::smoke::run_fps_bar_case},
    {"shipping_look_parity",
                            &heaven::smoke::run_shipping_look_parity_case},
    {"phase8_parity",       &heaven::smoke::run_phase8_parity_case},
    {"material_cache",      &heaven::smoke::run_material_cache_case},
    {"mesh_store",          &heaven::smoke::run_mesh_store_case},
    {"weather_reapply",     &heaven::smoke::run_weather_reapply_case},
    {"music_player",        &heaven::smoke::run_music_player_case},
    {"music_decode_start",  &heaven::smoke::run_music_decode_start_case},
    {"music_device_failure",
                            &heaven::smoke::run_music_device_failure_case},
    {"music_frame_parity",  &heaven::smoke::run_music_frame_parity_case},
    {"height_sampler_lit_lifecycle", &heaven::smoke::run_height_sampler_lit_lifecycle_case},
    {"render_test_reload", &heaven::smoke::run_render_test_reload_case},
    {"loading_progress", &heaven::smoke::run_loading_progress_case},
};

} // namespace

int main() try {
    using namespace heaven;

    // ── Gate: opt-in ────────────────────────────────────────────
    if (!smoke::smoke_opt_in()) {
        std::printf("%s SKIPPED: renderer-linked smoke harness is opt-in "
                    "(set HEAVEN_SMOKE=1 to run)\n", kTag);
        return 77;
    }

    // ── Gate: display present ───────────────────────────────────
    if (!smoke::display_available()) {
        std::printf("%s SKIPPED: no display (DISPLAY / WAYLAND_DISPLAY unset)\n", kTag);
        return 77;
    }

    // A small window is enough — the cases never need a visible result, only a
    // real backend constructed against a native surface. Reuse create_window()
    // so the SDL + X11/Wayland WSI setup matches exactly what main.cpp hands
    // the backend.
    AppConfig config;
    config.width  = 64;
    config.height = 64;
    if (const char* hdr = std::getenv("HEAVEN_SMOKE_HDR");
        hdr && std::strcmp(hdr, "1") == 0) {
        config.hdr = true;
    }
    if (const char* data = std::getenv("HEAVEN_DATA"); data && *data) {
        config.data_dir = data;
    }

    // Match Application::init: every backend feature borrows one application-
    // owned logical asset context before pipeline initialization. In
    // particular, GrassDepthProxy resolves its atlas during the first smoke
    // case that initializes pipelines, so leaving this context empty turns a
    // legitimate missing-asset diagnostic into a null optional dereference.
    auto assets = AssetFileSystem::open(config.data_dir);
    if (!assets) {
        std::printf("%s FAILED: asset filesystem unavailable (%s)\n",
                    kTag, assets.error().message.c_str());
        return 1;
    }
    auto paths = PathResolver::create(config.data_dir);
    if (!paths) {
        std::printf("%s FAILED: path resolver unavailable (%s)\n",
                    kTag, paths.error().message.c_str());
        return 1;
    }

    auto win = create_window(config, GraphicsApi::Filament);
    if (!win) {
        std::printf("%s FAILED: window creation failed (%s)\n",
                    kTag, win.error().message.c_str());
        return 1;
    }

    auto backend = create_backend(GraphicsApi::Filament, *win, config);
    if (!backend) {
        std::printf("%s FAILED: backend creation failed (%s)\n",
                    kTag, backend.error().message.c_str());
        return 1;
    }
    (*backend)->set_asset_context(std::cref(*paths), std::cref(*assets));
    if (config.hdr) {
        // Exercise the production startup order: the HDR splash is presented
        // before any case initializes scene pipelines or the UI.
        (*backend)->show_loading_screen(config.data_dir, "HDR startup regression",
                                        win->width, win->height);
    }

    if (const char* expected = std::getenv("HEAVEN_SMOKE_EXPECT_API");
        expected && *expected && (*backend)->render_api_name() != expected) {
        std::fprintf(stderr, "%s FAILED: expected %s, got %s\n", kTag,
                     expected, (*backend)->render_api_name().c_str());
        return 1;
    }

    // The drawable extent may differ from the requested size on HiDPI/scaled
    // displays — create_window() already queried it into the window handle.
    CaseContext ctx{**backend, config, win->width, win->height};

    if (const char* height_batch = std::getenv("HEAVEN_SMOKE_HEIGHT_BATCH_ONLY");
        height_batch && std::strcmp(height_batch, "1") == 0) {
        const auto result = heaven::smoke::run_height_sample_batch_case(ctx);
        std::printf("%s case 'height_sample_batch': %s\n", kTag,
                    result == CaseResult::Pass ? "PASS" : "FAIL");
        return result == CaseResult::Pass ? 0 : 1;
    }

    // ── Run every registered case ───────────────────────────────
    int failures = 0;
    int skipped  = 0;
    int passed   = 0;

    const char* selected_case = std::getenv("HEAVEN_SMOKE_CASE");
    bool selected_found = false;
    for (const auto& c : kCases) {
        if (selected_case && *selected_case && std::strcmp(selected_case, c.name) != 0) continue;
        selected_found = true;
        CaseResult r = c.fn(ctx);
        switch (r) {
            case CaseResult::Pass:
                ++passed;
                std::printf("%s case '%s': PASS\n", kTag, c.name);
                break;
            case CaseResult::Skip:
                ++skipped;
                std::printf("%s case '%s': SKIP\n", kTag, c.name);
                break;
            case CaseResult::Fail:
                ++failures;
                std::printf("%s case '%s': FAIL\n", kTag, c.name);
                break;
        }
    }

    // Opt in separately because this case briefly enters desktop fullscreen.
    if (const char* lifecycle = std::getenv("HEAVEN_SMOKE_WINDOW_LIFECYCLE");
        lifecycle && std::strcmp(lifecycle, "1") == 0) {
        const auto result = smoke::run_window_lifecycle_case(ctx, *win);
        if (result == CaseResult::Pass) ++passed;
        else ++failures;
        std::printf("%s case 'window_lifecycle': %s\n", kTag,
                    result == CaseResult::Pass ? "PASS" : "FAIL");
    }

    std::printf("%s summary: %d passed, %d skipped, %d failed\n",
                kTag, passed, skipped, failures);

    if (!selected_found) {
        std::fprintf(stderr, "%s unknown HEAVEN_SMOKE_CASE: %s\n", kTag, selected_case);
        return 1;
    }

    if (failures > 0) return 1;
    return passed > 0 ? 0 : 77;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s FAILED: unhandled exception: %s\n", kTag, error.what());
    return 1;
} catch (...) {
    std::fprintf(stderr, "%s FAILED: unknown exception\n", kTag);
    return 1;
}
