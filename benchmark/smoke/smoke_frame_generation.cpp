/* Opt-in real SDK/backend lifecycle coverage; no display-cadence claim. */
#include "smoke_cases.h"

#include "engine/camera.h"
#include "engine/render_test_scene.h"
#include "engine/render_test_scene_catalog.h"
#include "rhi/filament_backend.h"

#include <filament/Engine.h>
#include <filament/View.h>
#include <SDL2/SDL.h>
#include <imgui.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_frame_generation]";
#if HEAVEN_HAS_FSR3

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

// Renderer scene/depth/temporal borrowers must retire before mesh owners.
bool retire_meshes(FilamentBackend& backend, std::optional<RenderTestScene>& scene) {
    if (!scene) return true;
    auto* engine = backend.debug_render_context().engine;
    if (!engine) return false;
    std::set<uint64_t> handles;
    for (const auto& mesh : scene->scene.gpu_meshes)
        for (const auto& surface : mesh.surfaces)
            if (surface.handle) handles.insert(surface.handle);
    const auto before_vb = engine->getVertexBufferCount();
    const auto before_ib = engine->getIndexBufferCount();
    for (const auto& mesh : scene->scene.gpu_meshes) backend.release_mesh(mesh);
    scene.reset();
    return before_vb >= handles.size() && before_ib >= handles.size() &&
        engine->getVertexBufferCount() == before_vb - handles.size() &&
        engine->getIndexBufferCount() == before_ib - handles.size();
}

// All callback/asset captures remain alive until this owner retires borrowers.
class Restore {
public:
    Restore(FilamentBackend& backend, const CaseContext& context,
            PathResolverRef paths, AssetFileSystemRef assets,
            std::optional<RenderTestScene>& scene)
        : backend_(backend), context_(context), paths_(paths), assets_(assets), scene_(scene) {}
    ~Restore() { if (!attempted_) static_cast<void>(run()); }
    bool run() noexcept {
        attempted_ = true;
        try {
            backend_.overlay_set_draw({});
            backend_.set_capture_time(std::nullopt);
            backend_.request_frame_generation(FrameGenerationMode::Off);
            backend_.set_asset_context(paths_, assets_);
            const auto result = backend_.reconfigure_for_reload(context_.config);
            if (!result) return false;
            if (!retire_meshes(backend_, scene_)) return false;
            if (context_.window) {
                context_.window->set_drawable_size(context_.width, context_.height);
                for (unsigned attempt = 0; attempt < 200; ++attempt) {
                    SDL_PumpEvents();
                    get_drawable_size(context_.window->window, context_.window->width,
                                      context_.window->height);
                    if (context_.window->width == context_.width &&
                        context_.window->height == context_.height) break;
                    SDL_Delay(10);
                }
                if (context_.window->width != context_.width ||
                    context_.window->height != context_.height) return false;
                backend_.resize(context_.width, context_.height);
            }
            auto& render = backend_.debug_render_context();
            render.engine->flushAndWait();
            const auto same = [](const auto& a, const auto& b) {
                return a.has_value() == b.has_value() && (!a || &a->get() == &b->get());
            };
            return same(render.paths, paths_) && same(render.assets, assets_) &&
                render.output_target == nullptr && render.view->getRenderTarget() == nullptr;
        } catch (const std::exception& error) {
            std::printf("%s restore exception: %s\n", kTag, error.what());
            return false;
        } catch (...) { return false; }
    }
private:
    FilamentBackend& backend_;
    const CaseContext& context_;
    PathResolverRef paths_;
    AssetFileSystemRef assets_;
    std::optional<RenderTestScene>& scene_;
    bool attempted_ = false;
};
#endif
} // namespace

CaseResult run_frame_generation_case(const CaseContext& ctx) {
#if !HEAVEN_HAS_FSR3
    static_cast<void>(ctx);
    std::printf("%s SKIP: optional FSR SDK is not compiled\n", kTag);
    return CaseResult::Skip;
#else
    const char* opted_in = std::getenv("HEAVEN_SMOKE_FRAME_GENERATION");
    if (!opted_in || std::strcmp(opted_in, "1") != 0) {
        std::printf("%s SKIP: set HEAVEN_SMOKE_FRAME_GENERATION=1 for native SDK lifecycle coverage\n", kTag);
        return CaseResult::Skip;
    }
    if (ctx.backend.api() != GraphicsApi::Filament ||
        ctx.backend.render_api_name() != "Vulkan" || ctx.config.hdr) {
        std::printf("%s SKIP: requires the SDR Vulkan backend\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (const auto initialized = backend.init_pipelines(ctx.config); !initialized) {
        std::printf("%s FAIL: pipeline initialization: %s\n", kTag,
                    initialized.error().message.c_str());
        return CaseResult::Fail;
    }
    const auto original = backend.debug_render_context();
    if (!original.engine || !original.view || !ctx.window || !ctx.window->window) {
        std::printf("%s FAIL: missing live engine/view/harness window\n", kTag);
        return CaseResult::Fail;
    }

    std::optional<AssetFileSystem> assets;
    std::optional<PathResolver> paths;
    std::optional<RenderTestScene> scene;
    Restore restore(backend, ctx, original.paths, original.assets, scene);
    uint64_t attempts = 0, accepted_total = 0, ui_calls = 0, bone_updates = 0;
    int width = ctx.width, height = ctx.height;
    const auto* ui_oracle_setting = std::getenv("HEAVEN_SMOKE_FG_UI_ORACLE");
    const bool ui_oracle = ui_oracle_setting && std::string_view(ui_oracle_setting) == "1";
    bool ok = true;
    try {
        AppConfig config = ctx.config;
        require(apply_render_test_scene_defaults(config, "animation"), "animation defaults unavailable");
        config.frame_generation = FrameGenerationMode::Fsr3;
        config.hdr = false;
        config.features.dynamic_resolution = false;
        // Keep the real two-bone path; cloth remains a static opaque card here.
        config.features.cloth = false;
        auto definition = make_render_test_scene("animation");
        require(definition.has_value(), "procedural animation generation failed");
        auto generated = AssetFileSystem::create_memory("frame-generation-lifecycle");
        auto generated_paths = PathResolver::create("frame-generation-lifecycle");
        require(generated.has_value() && generated_paths.has_value(), "asset namespace creation failed");
        require(mount_render_test_scene_assets(*definition, *generated).has_value(), "procedural DDS mount failed");
        require(backend.reconfigure_for_reload(config).has_value(), "initial procedural reload failed");
        assets.emplace(std::move(*generated));
        paths.emplace(std::move(*generated_paths));
        backend.set_asset_context(std::cref(*paths), std::cref(*assets));
        auto upload = upload_render_test_scene(std::move(*definition), backend, config);
        require(upload.has_value(), "procedural mesh upload failed");
        scene.emplace(std::move(*upload));
        require(!scene->scene.animated.empty() && scene->scene.instances.size() > 2,
                "fixture lacks actual animated/instanced geometry");
        backend.set_scene_lights(scene->scene.lights);
        backend.set_particle_emitters(scene->scene.particle_emitters);
        backend.overlay_init(config);
        require(ImGui::GetCurrentContext() != nullptr, "real UI context unavailable");
        const auto* ui_context = ImGui::GetCurrentContext();
        backend.overlay_set_draw([&] {
            ++ui_calls;
            if (ui_oracle) {
                // Exact opaque sentinels make native generated/real UI placement
                // independently observable. The moving block alternates on
                // accepted sources; rejected attempts keep the same stimulus.
                auto* draw = ImGui::GetForegroundDrawList();
                const auto scale = ImGui::GetIO().DisplayFramebufferScale;
                const auto point = [&](float x, float y) { return ImVec2(x / scale.x, y / scale.y); };
                draw->AddRectFilled(point(4, 4), point(16, 16), IM_COL32(232, 36, 188, 255));
                draw->AddRectFilled(point(float(width - 16), float(height - 16)),
                    point(float(width - 4), float(height - 4)), IM_COL32(29, 173, 241, 255));
                const float x = accepted_total % 2 == 0 ? 20.0f : 40.0f;
                draw->AddRectFilled(point(x, 20), point(x + 12, 32), IM_COL32(17, 231, 69, 255));
                const auto source_code = static_cast<int>(16 + 16 * (accepted_total % 12 + 1));
                draw->AddRectFilled(point(4, 36), point(16, 48), IM_COL32(source_code, 73, 201, 255));
                // Different coverages and a moving translucent widget exercise
                // the actual generated-image UI compute, not only opaque copy.
                draw->AddRectFilled(point(20, 4), point(32, 16), IM_COL32(220, 75, 29, 96));
                draw->AddRectFilled(point(36, 4), point(48, 16), IM_COL32(32, 160, 245, 192));
                draw->AddRectFilled(point(x, 36), point(x + 12, 48), IM_COL32(164, 43, 211, 128));
                draw->AddRectFilled(point(20, 50), point(32, 62), IM_COL32(7, 249, 131, 0));
                return;
            }
            ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(210, 74), ImGuiCond_Always);
            ImGui::Begin("Frame generation lifecycle", nullptr,
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
            ImGui::Text("Source %llu", static_cast<unsigned long long>(accepted_total));
            ImGui::ProgressBar(float(accepted_total % 10) / 9.0f);
            ImGui::End();
        });
        Camera camera;
        constexpr std::array<float, 5> pose{7, -10, 6, 120.2564f, -19.7989f};
        camera.position = {pose[0], pose[1], pose[2]};
        camera.yaw = pose[3]; camera.pitch = pose[4]; camera.fov = 50;
        const auto lighting = lighting_for_time_of_day(config.time_of_day);
        const WeatherState weather(config.weather);
        const TonemapParams tonemap;
        const MaterialResolver resolve = [&](const std::string& mesh, const std::string& surface) {
            return scene->resolve_material(mesh, surface);
        };
        std::vector<glm::mat4> base_transforms;
        std::vector<int> dirty;
        const auto remember_instances = [&] {
            base_transforms.clear(); dirty.clear();
            for (std::size_t i = 0; i < scene->scene.instances.size(); ++i) {
                base_transforms.push_back(scene->scene.instances[i].transform);
                dirty.push_back(static_cast<int>(i));
            }
        };
        remember_instances();
        const auto snapshot = [&] {
            original.engine->flushAndWait();
            auto state = backend.frame_generation_status();
            require(state.counters.presented_frames == state.counters.real_presented_frames +
                    state.counters.generated_presented_frames, "native submission split is inconsistent");
            require(state.counters.real_presented_frames <= state.counters.real_frames &&
                    state.counters.generated_presented_frames <= state.counters.generated_frames,
                    "presented counts exceed recorded work");
            require(!state.counters.presentation_feedback_available &&
                    state.counters.confirmed_displayed_frames == 0,
                    "native submissions manufactured display feedback");
            require(state.counters.failed_presentations == 0, "native presentation failed");
            return state;
        };
        const auto frames = [&](unsigned count, const char* label) {
            const auto before = snapshot();
            const uint64_t first_attempt = attempts;
            unsigned accepted = 0;
            const auto deadline = SDL_GetTicks64() + 15000;
            while (accepted < count && attempts - first_attempt < 192 && SDL_GetTicks64() < deadline) {
                SDL_PumpEvents();
                backend.begin_application_frame(++attempts);
                const float time = float(accepted_total) / 60.0f;
                backend.set_capture_time(time);
                for (std::size_t i = 0; i < scene->scene.instances.size(); ++i) {
                    auto& instance = scene->scene.instances[i];
                    instance.transform = base_transforms[i];
                    if (scene->scene.cpu_meshes[instance.mesh_index].name == "instanced_bevel")
                        instance.transform[3].x += 0.2f * std::sin(4 * time + float(i));
                }
                for (const auto& animated : scene->scene.animated) {
                    const auto active = backend.gpu_skinning_active(animated.mesh_index);
                    if (!active.has_value()) continue; // Production first-build behavior.
                    require(*active, "native GPU skinning failed to activate");
                    const float frame = std::fmod(time * animated.anim_speed,
                                                  float(animated.smesh.num_frames));
                    const auto matrices = animated.smesh.bone_matrices_interpolated(frame);
                    require(backend.update_skinned_bones(animated.mesh_index, matrices).has_value(),
                            "native bone update failed");
                    ++bone_updates;
                }
                backend.end_application_simulation();
                backend.overlay_begin_frame(1.0f / 60.0f, width, height);
                const auto frame = backend.render_frame(scene->scene.instances, dirty,
                    scene->scene.gpu_meshes, resolve, camera, lighting, weather, tonemap, width, height);
                if (frame.presented) { ++accepted; ++accepted_total; }
                original.engine->flushAndWait();
                require(ImGui::GetCurrentContext() == ui_context, "UI target lifecycle replaced its context");
                if (!frame.presented) SDL_Delay(2);
            }
            require(accepted == count, "bounded frame loop did not accept requested source frames");
            const auto after = snapshot();
            require(after.counters.real_frames - before.counters.real_frames == accepted,
                    "real-frame IDs differ from accepted backend submissions");
            require(after.counters.rejected_real_frames - before.counters.rejected_real_frames ==
                    attempts - first_attempt - accepted, "rejected attempts were lost or counted as real frames");
            std::printf("%s %s: +%u real, +%llu generated, +%llu native real, +%llu native generated, epoch %llu\n",
                kTag, label, accepted,
                static_cast<unsigned long long>(after.counters.generated_frames - before.counters.generated_frames),
                static_cast<unsigned long long>(after.counters.real_presented_frames - before.counters.real_presented_frames),
                static_cast<unsigned long long>(after.counters.generated_presented_frames - before.counters.generated_presented_frames),
                static_cast<unsigned long long>(after.history_epoch));
            return after;
        };
        const auto require_active = [&](const FrameGenerationStatus& state) {
            if (state.effective != FrameGenerationMode::Fsr3 || !state.capability.available) {
                std::printf("%s provider reason: %s\n", kTag, state.reason.c_str());
                require(false, "actual FSR provider did not become available");
            }
            require(state.history_ready && state.counters.generated_frames_available &&
                    state.counters.presentation_submissions_available,
                    "active provider lacks history or complete native accounting");
            require(state.counters.generated_frames > 0 &&
                    state.counters.generated_presented_frames > 0, "no actual generated image reached native presentation");
        };
        const auto initial = frames(ui_oracle ? 12 : 8, "animated UI warmup");
        require_active(initial);
        require(bone_updates > 0 && ui_calls >= attempts, "fixture never exercised actual bones/UI preparation");
        if (ui_oracle) std::printf("%s native UI oracle: opaque sentinels and alternating accepted-source block, initial sources=12\n", kTag);

        // A wall-time pause must invalidate SDK and geometry history even when
        // camera, drawable and simulation time remain continuous. This is a
        // Linux long-gap control, not a physical Android activity test.
        SDL_Delay(300);
        const auto resumed = frames(1, "pause first source");
        require(resumed.history_epoch > initial.history_epoch &&
                resumed.counters.generated_frames == initial.counters.generated_frames,
                "pause reused interpolation history from before the long gap");
        const auto pause_recovery = frames(5, "pause recovery");
        require_active(pause_recovery);
        require(pause_recovery.counters.generated_frames > resumed.counters.generated_frames,
                "pause did not recover interpolation after fresh sources");

        // A discontinuous projection requires two new real sources. One accepted
        // source cannot claim an interpolation from the previous camera history.
        camera.fov += 7;
        const auto cut = frames(1, "camera cut first source");
        require(cut.history_epoch > pause_recovery.history_epoch &&
                cut.counters.generated_frames == pause_recovery.counters.generated_frames,
                "camera cut reused stale interpolation history");
        const auto recovered = frames(5, "camera cut recovery");
        require_active(recovered);
        require(recovered.counters.generated_frames > cut.counters.generated_frames,
                "camera cut did not recover generation");

#if defined(HEAVEN_FG_FAULT_INJECTION)
        // Queue a continuous source before native acquire reports OUT_OF_DATE.
        // The application has not consumed the recreation event when the SDK
        // callback runs, so this checks the driver-visible history reset.
        require(backend.debug_frame_generation_out_of_date_once(),
                "native acquire fault injection is unavailable");
        const auto recreated = frames(1, "implicit same-size recreation first source");
        require(recreated.history_epoch > recovered.history_epoch &&
                recreated.counters.generated_frames == recovered.counters.generated_frames,
                "implicit recreation interpolated from pre-recreation history");
        const auto recreation_recovery = frames(5, "implicit recreation recovery");
        require_active(recreation_recovery);
        require(recreation_recovery.counters.generated_frames > recreated.counters.generated_frames,
                "implicit recreation did not recover after fresh sources");
#endif

        backend.request_frame_generation(FrameGenerationMode::Off);
        const auto disabled = snapshot();
        require(disabled.requested == FrameGenerationMode::Off &&
                disabled.effective == FrameGenerationMode::Off, "cancellation failed to disable generation");
        const auto off_frames = frames(3, "off with live UI");
        require(off_frames.counters.generated_frames == disabled.counters.generated_frames,
                "off mode recorded new generated work");
        backend.request_frame_generation(FrameGenerationMode::Fsr3);
        const auto restarted_first = frames(1, "re-enable first source");
        require(restarted_first.counters.generated_frames == off_frames.counters.generated_frames,
                "re-enable reused pre-cancellation history");
        require_active(frames(5, "re-enable recovery"));

        const auto resize_to = [&](int target_width, int target_height) {
            const auto before = snapshot();
            ctx.window->set_drawable_size(target_width, target_height);
            for (unsigned attempt = 0; attempt < 200; ++attempt) {
                SDL_PumpEvents();
                get_drawable_size(ctx.window->window, ctx.window->width, ctx.window->height);
                if (ctx.window->width == target_width && ctx.window->height == target_height) break;
                SDL_Delay(10);
            }
            require(ctx.window->width == target_width && ctx.window->height == target_height,
                    "OS drawable resize did not settle");
            width = ctx.window->width; height = ctx.window->height;
            backend.resize(width, height);
            const auto resized = snapshot();
            require(resized.history_epoch > before.history_epoch && !resized.history_ready,
                    "swapchain resize did not invalidate history");
            const auto first = frames(1, "resized first source");
            require(first.counters.generated_frames == before.counters.generated_frames,
                    "resize generated from a retired target/history");
            const auto active = frames(5, "resized recovery");
            require_active(active);
            require(active.counters.generated_frames > first.counters.generated_frames,
                    "resized targets did not recover generation");
        };
        resize_to(320, 180);
        resize_to(384, 216);

        // Reload destroys Runtime and temporal borrowers before reusing the same
        // memory namespace and new mesh owners. Counters belong to the new Runtime.
        require(backend.reconfigure_for_reload(config).has_value(), "active scene reconfigure failed");
        require(retire_meshes(backend, scene), "reload did not release exact old VB/IB owners");
        const auto reset = snapshot();
        require(reset.counters.real_frames == 0 && reset.counters.generated_frames == 0 &&
                reset.counters.presented_frames == 0 && !reset.history_ready,
                "new Runtime inherited the previous capture counters/history");
        require(original.view->getRenderTarget() == nullptr &&
                backend.debug_render_context().output_target == nullptr,
                "reload left a retired temporal target borrowed");
        auto next_definition = make_render_test_scene("animation");
        require(next_definition.has_value(), "reload scene regeneration failed");
        auto next_upload = upload_render_test_scene(std::move(*next_definition), backend, config);
        require(next_upload.has_value(), "reload scene reupload failed");
        scene.emplace(std::move(*next_upload));
        backend.set_scene_lights(scene->scene.lights);
        backend.set_particle_emitters(scene->scene.particle_emitters);
        remember_instances();
        require_active(frames(8, "reloaded animated UI recovery"));
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        ok = false;
    }
    ok = restore.run() && ok;
    if (!ok) return CaseResult::Fail;
    std::printf("%s PASS: %llu accepted sources, native interpolation/submission controls, "
                "bones/UI, pause, cut, off/on, two OS resizes and reload; owner/context restored; "
                "no pixel-quality or scanout claim\n", kTag,
                static_cast<unsigned long long>(accepted_total));
    return CaseResult::Pass;
#endif
}
} // namespace heaven::smoke
