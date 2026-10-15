/* Opt-in real SDK/backend lifecycle coverage; no display-cadence claim. */
#include "smoke_cases.h"

#include "engine/camera.h"
#include "engine/render_test_scene.h"
#include "engine/render_test_scene_catalog.h"
#include "rhi/filament_backend.h"

#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/View.h>
#include <SDL2/SDL.h>
#include <imgui.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#if HEAVEN_HAS_FSR3 && defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <ctime>
#include <unistd.h>
#endif

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_frame_generation]";
#if HEAVEN_HAS_FSR3

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

#ifdef __linux__
// A test-only driver gate holds a queued packet across an actual process stop.
// The controller releases rejected attempts without stopping them. Callback
// data owns its pipe/state until execution, including exceptional teardown.
class ProcessPauseGate {
public:
    enum class Mode { Control, Pause };
    explicit ProcessPauseGate(bool system_suspend) : state_(std::make_shared<State>()), system_suspend_(system_suspend) {
        const char* value = std::getenv("HEAVEN_SMOKE_FG_PROCESS_PAUSE_FD");
        require(value && *value, "process pause requires the maintained pipe controller");
        const std::string_view input(value);
        int descriptor = -1;
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), descriptor);
        require(parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() &&
                descriptor >= 3 && ::fcntl(descriptor, F_GETFD) >= 0,
                "invalid process-pause controller descriptor");
        state_->descriptor = descriptor;
    }
    void begin(Mode mode, uint64_t source) {
        require(!armed_ && (!queued_ || state_->done.load()), "process gate reused before callback retirement");
        ++serial_; source_ = source; mode_ = mode; armed_ = true; queued_ = false;
        state_->frontend.store(-1); state_->failure.store(0); state_->done.store(false);
        state_->held_ns.store(0);
        state_->awake_ns.store(0); state_->sleep_ns.store(0); state_->clocks_valid.store(false);
    }
    bool armed() const noexcept { return armed_; }
    bool queued() const noexcept { return queued_; }
    void enqueue(filament::Renderer& renderer) {
        require(armed_ && !queued_, "process gate queued more than once");
        auto packet = std::make_unique<Packet>(Packet{state_, serial_, source_, mode_, system_suspend_});
        renderer.nativeCommand(nullptr, nullptr, nullptr, nullptr, callback, packet.get());
        packet.release(); queued_ = true; armed_ = false;
    }
    void frontend_complete(bool accepted, uint64_t epoch, bool ready) noexcept {
        state_->frontend.store(accepted ? 1 : 0, std::memory_order_release);
        std::printf("[smoke_frame_generation_process_pause] packet-queued serial=%llu source=%llu mode=%s accepted=%d epoch=%llu ready=%d\n",
            static_cast<unsigned long long>(serial_), static_cast<unsigned long long>(source_),
            mode_ == Mode::Pause ? "pause" : "control", accepted ? 1 : 0,
            static_cast<unsigned long long>(epoch), ready ? 1 : 0);
        std::fflush(stdout);
    }
    bool completed() const noexcept { return state_->done.load() && state_->failure.load() == 0; }
    uint64_t held_ns() const noexcept { return state_->held_ns.load(); }
    uint64_t awake_ns() const noexcept { return state_->awake_ns.load(); }
    uint64_t sleep_ns() const noexcept { return state_->sleep_ns.load(); }
    bool clocks_valid() const noexcept { return state_->clocks_valid.load(); }
    void cancel() noexcept { state_->cancelled.store(true); }
private:
    struct State {
        ~State() { if (descriptor >= 0) ::close(descriptor); }
        int descriptor = -1;
        std::atomic<int> frontend{-1}, failure{0};
        std::atomic<bool> done{false}, cancelled{false};
        std::atomic<uint64_t> held_ns{0};
        std::atomic<uint64_t> awake_ns{0}, sleep_ns{0};
        std::atomic<bool> clocks_valid{false};
    };
    struct Packet {
        std::shared_ptr<State> state;
        uint64_t serial, source;
        Mode mode;
        bool system_suspend;
    };
    static bool clock_sample(clockid_t clock_id, uint64_t& result) noexcept {
        timespec value{};
        constexpr uint64_t billion = 1000000000;
        if (::clock_gettime(clock_id, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 ||
                std::cmp_greater_equal(value.tv_nsec, billion)) return false;
        const auto seconds = static_cast<uint64_t>(value.tv_sec);
        const auto nanos = static_cast<uint64_t>(value.tv_nsec);
        if (seconds > (std::numeric_limits<uint64_t>::max() - nanos) / billion) return false;
        result = seconds * billion + nanos;
        return true;
    }
    static void callback(void* data, [[maybe_unused]] const void* native_context) noexcept {
        const std::unique_ptr<Packet> packet(static_cast<Packet*>(data));
        auto& state = *packet->state;
        const auto start = std::chrono::steady_clock::now();
        uint64_t boot_before = 0, mono_before = 0;
        const bool clocks_before = !packet->system_suspend ||
            (clock_sample(CLOCK_BOOTTIME, boot_before) && clock_sample(CLOCK_MONOTONIC, mono_before));
        std::printf("[smoke_frame_generation_process_pause] gate-enter serial=%llu source=%llu mode=%s\n",
            static_cast<unsigned long long>(packet->serial), static_cast<unsigned long long>(packet->source),
            packet->mode == Mode::Pause ? "pause" : "control");
        std::fflush(stdout);
        char release = 0;
        int failure = 0;
        while (!release && !failure) {
            if (state.cancelled.load()) { failure = 1; break; }
            const auto limit = packet->system_suspend ? std::chrono::seconds(60) : std::chrono::seconds(10);
            if (std::chrono::steady_clock::now() - start > limit) {
                failure = 2; break;
            }
            pollfd input{state.descriptor, POLLIN, 0};
            const int polled = ::poll(&input, 1, 50);
            if (polled < 0 && errno != EINTR) { failure = 3; break; }
            if (polled <= 0) continue;
            if ((input.revents & POLLIN) != 0) {
                const auto bytes = ::read(state.descriptor, &release, 1);
                if (bytes < 0 && errno == EINTR) continue;
                if (bytes != 1) failure = 4;
            } else if (input.revents != 0) failure = 5;
        }
        const int accepted = state.frontend.load(std::memory_order_acquire);
        const char expected = accepted == 0 ? 'R' : packet->mode == Mode::Pause ? 'P' : 'C';
        if (!failure && (accepted < 0 || release != expected)) failure = 6;
        const auto held = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        state.held_ns.store(static_cast<uint64_t>(held));
        if (packet->system_suspend) {
            uint64_t boot_after = 0, mono_after = 0;
            const bool valid = clocks_before && clock_sample(CLOCK_BOOTTIME, boot_after) &&
                clock_sample(CLOCK_MONOTONIC, mono_after) && boot_after >= boot_before && mono_after >= mono_before;
            const uint64_t awake = valid ? mono_after - mono_before : 0;
            const uint64_t elapsed = valid ? boot_after - boot_before : 0;
            const uint64_t slept = valid && elapsed > awake ? elapsed - awake : 0;
            state.awake_ns.store(awake); state.sleep_ns.store(slept); state.clocks_valid.store(valid);
            std::printf("[smoke_frame_generation_system_suspend] clock-proof serial=%llu source=%llu valid=%d "
                "boot_before_ns=%llu boot_after_ns=%llu mono_before_ns=%llu mono_after_ns=%llu "
                "awake_ns=%llu slept_ns=%llu awake_under_250ms=%d\n",
                static_cast<unsigned long long>(packet->serial), static_cast<unsigned long long>(packet->source),
                valid ? 1 : 0, static_cast<unsigned long long>(boot_before), static_cast<unsigned long long>(boot_after),
                static_cast<unsigned long long>(mono_before), static_cast<unsigned long long>(mono_after),
                static_cast<unsigned long long>(awake), static_cast<unsigned long long>(slept),
                valid && awake < 250000000 ? 1 : 0);
        }
        state.failure.store(failure); state.done.store(true);
        std::printf("[smoke_frame_generation_process_pause] gate-release serial=%llu source=%llu held_ns=%llu failure=%d\n",
            static_cast<unsigned long long>(packet->serial), static_cast<unsigned long long>(packet->source),
            static_cast<unsigned long long>(held), failure);
        std::fflush(stdout);
    }
    std::shared_ptr<State> state_;
    uint64_t serial_ = 0, source_ = 0;
    Mode mode_ = Mode::Control;
    bool system_suspend_ = false;
    bool armed_ = false, queued_ = false;
};
#endif

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
    const char* process_setting = std::getenv("HEAVEN_SMOKE_FG_PROCESS_PAUSE");
    const char* system_setting = std::getenv("HEAVEN_SMOKE_FG_SYSTEM_SUSPEND");
    const bool system_suspend = system_setting && std::string_view(system_setting) == "1";
    const bool process_pause = system_suspend || (process_setting && std::string_view(process_setting) == "1");
    const char* broken_setting = std::getenv("HEAVEN_SMOKE_FG_BROKEN_CHAIN_ORACLE");
    const bool broken_chain_oracle = broken_setting && std::string_view(broken_setting) == "1";
#ifdef __linux__
    std::unique_ptr<ProcessPauseGate> process_gate;
    std::optional<ProcessPauseGate::Mode> gate_mode;
#endif
    bool asynchronous_frames = false;
    bool ok = true;
    try {
        require(!broken_chain_oracle || (!ui_oracle && !process_pause),
                "broken-chain oracle requires ordinary lifecycle UI without the process-pause controller");
#ifndef HEAVEN_FG_FAULT_INJECTION
        require(!broken_chain_oracle, "broken-chain oracle requires native fault-injection support");
#endif
#ifdef __linux__
        if (process_pause) {
            require(!ui_oracle && original.renderer, "process pause requires ordinary lifecycle UI and renderer");
            process_gate = std::make_unique<ProcessPauseGate>(system_suspend);
        }
#else
        require(!process_pause, "process pause controller requires native Linux");
#endif
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
#ifdef __linux__
            if (process_gate && process_gate->armed()) process_gate->enqueue(*original.renderer);
#endif
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
            const auto deadline = SDL_GetTicks64() + (asynchronous_frames ? 30000 : 15000);
            const uint64_t attempt_limit = asynchronous_frames ? uint64_t(count) * 32 + 192 : 192;
            while (accepted < count && attempts - first_attempt < attempt_limit && SDL_GetTicks64() < deadline) {
                SDL_PumpEvents();
#ifdef __linux__
                if (gate_mode) {
                    require(count == 1, "queued process gate requires one accepted source");
                    process_gate->begin(*gate_mode, before.counters.real_frames + 1);
                }
#endif
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
#ifdef __linux__
                if (gate_mode) {
                    require(process_gate->queued(), "process gate was not queued before the SDK callback");
                    // This reads owner counters without GPU draining. Any reset
                    // performed by prepare() would otherwise mask the queued-
                    // callback gap under test. No subsequent prepare runs until
                    // this exact source has completed and its oracle is checked.
                    const auto queued = backend.frame_generation_status();
                    const bool unchanged = queued.history_epoch == before.history_epoch &&
                        queued.history_ready && queued.counters.real_frames == before.counters.real_frames;
                    process_gate->frontend_complete(frame.presented && unchanged,
                        queued.history_epoch, queued.history_ready);
                    original.engine->flushAndWait();
                    require(process_gate->completed(), "process gate controller failed or timed out");
                    require(!frame.presented || unchanged, "frontend reset preceded queued process pause");
                    if (frame.presented) gate_mode.reset();
                } else if (!asynchronous_frames) {
                    original.engine->flushAndWait();
                }
#else
                if (!asynchronous_frames) original.engine->flushAndWait();
#endif
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

#ifdef __linux__
        if (process_pause) {
            const auto owners = [&] {
                return std::array{original.engine->getBufferObjectCount(), original.engine->getViewCount(),
                    original.engine->getSceneCount(), original.engine->getSwapChainCount(),
                    original.engine->getVertexBufferCount(), original.engine->getIndexBufferCount(),
                    original.engine->getSkinningBufferCount(), original.engine->getInstanceBufferCount(),
                    original.engine->getTextureCount(), original.engine->getRenderTargetCount()};
            };
            asynchronous_frames = true;
            const auto async_before = frames(128, system_suspend ? "system suspend asynchronous ring warmup" :
                "process pause asynchronous ring warmup");
            require_active(async_before);
            const auto owner_baseline = owners();
            asynchronous_frames = false;
            gate_mode = ProcessPauseGate::Mode::Control;
            const auto control = frames(1, "queued no-pause control");
            require(control.history_epoch == async_before.history_epoch &&
                    control.counters.generated_frames == async_before.counters.generated_frames + 1 &&
                    control.counters.generated_presented_frames == async_before.counters.generated_presented_frames + 1,
                    "no-pause queued gate did not interpolate exactly one source");
            gate_mode = ProcessPauseGate::Mode::Pause;
            const auto suspended = frames(1, system_suspend ? "queued source after system resume" :
                "queued source after process SIGCONT");
            if (system_suspend) {
                require(process_gate->clocks_valid() && process_gate->sleep_ns() >= 5000000000,
                        "system suspend did not prove five seconds of physical sleep");
                std::printf("[smoke_frame_generation_system_suspend] sleep-integration source=%llu awake_ns=%llu "
                    "slept_ns=%llu awake_under_250ms=%d\n", static_cast<unsigned long long>(suspended.counters.real_frames),
                    static_cast<unsigned long long>(process_gate->awake_ns()),
                    static_cast<unsigned long long>(process_gate->sleep_ns()), process_gate->awake_ns() < 250000000 ? 1 : 0);
            } else {
                require(process_gate->held_ns() >= 500000000,
                        "process pause did not exceed the independent half-second minimum");
            }
            require(suspended.counters.real_frames == control.counters.real_frames + 1 &&
                    suspended.counters.real_presented_frames == control.counters.real_presented_frames + 1,
                    "queued process pause lost or duplicated its real source");
            std::printf("[smoke_frame_generation_process_pause] first-resumed source=%llu epoch_before=%llu epoch_after=%llu generated_before=%llu generated_after=%llu native_generated_before=%llu native_generated_after=%llu\n",
                static_cast<unsigned long long>(suspended.counters.real_frames),
                static_cast<unsigned long long>(control.history_epoch), static_cast<unsigned long long>(suspended.history_epoch),
                static_cast<unsigned long long>(control.counters.generated_frames), static_cast<unsigned long long>(suspended.counters.generated_frames),
                static_cast<unsigned long long>(control.counters.generated_presented_frames), static_cast<unsigned long long>(suspended.counters.generated_presented_frames));
            require(suspended.history_epoch > control.history_epoch &&
                    suspended.counters.generated_frames == control.counters.generated_frames &&
                    suspended.counters.generated_presented_frames == control.counters.generated_presented_frames,
                    "queued callback interpolated across OS process suspension");
            const auto recovered = frames(5, system_suspend ? "queued system suspend recovery" :
                "queued process pause recovery");
            require_active(recovered);
            require(recovered.counters.generated_frames > suspended.counters.generated_frames &&
                    recovered.counters.generated_presented_frames > suspended.counters.generated_presented_frames,
                    "process-pause history did not recover within five accepted sources");
            asynchronous_frames = true;
            require_active(frames(128, system_suspend ? "system suspend asynchronous ring recovery" :
                "process pause asynchronous ring recovery"));
            asynchronous_frames = false;
            require(owners() == owner_baseline, "process-pause asynchronous phases leaked Filament owners");
            if (system_suspend)
                std::printf("[smoke_frame_generation_system_suspend] PASS: physical sleep, same queued source reset, five-source recovery, 256 asynchronous sources and exact owner counts\n");
            else
                std::printf("[smoke_frame_generation_process_pause] PASS: same queued source reset, five-source recovery, 256 asynchronous sources and exact owner counts\n");
        }
#endif

        // A wall-time pause must invalidate SDK and geometry history even when
        // camera, drawable and simulation time remain continuous. This is a
        // Linux long-gap control, not a physical Android activity test.
        const auto before_long_gap = snapshot();
        SDL_Delay(300);
        const auto resumed = frames(1, "pause first source");
        require(resumed.history_epoch > before_long_gap.history_epoch &&
                resumed.counters.generated_frames == before_long_gap.counters.generated_frames,
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

#ifdef HEAVEN_FG_FAULT_INJECTION
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

        if (broken_chain_oracle) {
            constexpr const char* tag = "[smoke_frame_generation_broken_chain]";
            constexpr std::string_view failure_reason = "Generated presentation command buffer is unavailable";
            bool oracle_ok = true;
            const auto oracle_check = [&](bool condition, const char* message) {
                if (!condition) {
                    std::printf("%s FAIL: %s\n", tag, message);
                    oracle_ok = false;
                }
            };
            const auto before_fault = snapshot();
            require_active(before_fault);
            const auto attempts_before_fault = backend.debug_frame_generation_provider_attempts();
            require(attempts_before_fault > 0 && backend.debug_frame_generation_work_failure_once(),
                    "native generated-work fault injection or exact provider-attempt counter is unavailable");

            // Several accepted callbacks may already be queued when the first
            // generated present breaks its chain. Their provider invocations
            // must stop too, independent of a later frontend status poll.
            asynchronous_frames = true;
            auto blocked = frames(16, "broken-chain queued failure sources");
            asynchronous_frames = false;
            const auto fault_source = backend.debug_frame_generation_work_failure_source();
            const auto attempts_after_fault = backend.debug_frame_generation_provider_attempts();
            const auto allowed_attempts = fault_source > before_fault.counters.real_frames
                ? fault_source - before_fault.counters.real_frames : 0;
            oracle_check(fault_source > before_fault.counters.real_frames &&
                         fault_source <= blocked.counters.real_frames,
                         "one-shot generated-work failure did not identify its accepted source");
            oracle_check(attempts_after_fault - attempts_before_fault == allowed_attempts,
                         "already-queued callbacks continued invoking the provider after the chain broke");
            oracle_check(blocked.counters.generated_frames == before_fault.counters.generated_frames + 1,
                         "broken chain recorded further interpolation after its failed source");
            oracle_check(blocked.counters.real_presented_frames - before_fault.counters.real_presented_frames == 16 &&
                         blocked.counters.generated_presented_frames == before_fault.counters.generated_presented_frames,
                         "broken-chain sources were not submitted as real images only");
            oracle_check(blocked.effective == FrameGenerationMode::Off && !blocked.capability.available &&
                         blocked.reason == failure_reason,
                         "broken chain lost its unavailable status or original failure reason");
            const auto failure_effective = frame_generation_mode_id(blocked.effective);
            std::printf("%s failure source=%llu batch_real=16 allowed_attempts=%llu provider_attempts=%llu "
                        "generated_recordings=%llu native_generated=%llu effective=%.*s reason=%s\n", tag,
                        static_cast<unsigned long long>(fault_source),
                        static_cast<unsigned long long>(allowed_attempts),
                        static_cast<unsigned long long>(attempts_after_fault - attempts_before_fault),
                        static_cast<unsigned long long>(blocked.counters.generated_frames - before_fault.counters.generated_frames),
                        static_cast<unsigned long long>(blocked.counters.generated_presented_frames - before_fault.counters.generated_presented_frames),
                        static_cast<int>(failure_effective.size()), failure_effective.data(), blocked.reason.c_str());
            const auto recordings_after_fault = blocked.counters.generated_frames;
            for (unsigned index = 0; index != 4; ++index) {
                const auto previous = blocked.counters;
                blocked = frames(1, "broken-chain unavailable source");
                oracle_check(backend.debug_frame_generation_provider_attempts() == attempts_after_fault &&
                             blocked.counters.generated_frames == recordings_after_fault,
                             "unavailable presentation continued interpolation work");
                oracle_check(blocked.effective == FrameGenerationMode::Off && !blocked.capability.available &&
                             blocked.reason == failure_reason,
                             "unavailable presentation promoted or replaced its failure reason");
                oracle_check(blocked.counters.real_presented_frames == previous.real_presented_frames + 1 &&
                             blocked.counters.generated_presented_frames == previous.generated_presented_frames,
                             "unavailable presentation did not retain exact real-only native counts");
                const auto effective = frame_generation_mode_id(blocked.effective);
                std::printf("%s blocked source=%llu provider_attempts=%llu generated_recordings=%llu "
                            "native_real=%llu native_generated=%llu effective=%.*s reason=%s\n", tag,
                            static_cast<unsigned long long>(blocked.counters.real_frames),
                            static_cast<unsigned long long>(backend.debug_frame_generation_provider_attempts()),
                            static_cast<unsigned long long>(blocked.counters.generated_frames),
                            static_cast<unsigned long long>(blocked.counters.real_presented_frames),
                            static_cast<unsigned long long>(blocked.counters.generated_presented_frames),
                            static_cast<int>(effective.size()), effective.data(), blocked.reason.c_str());
            }

            // Successful native recreation is the existing retry boundary.
            // Continue even after a failed assertion to retain recovery evidence
            // in the initial deliberately failing control.
            require(backend.debug_frame_generation_out_of_date_once(),
                    "broken-chain recovery recreation injection is unavailable");
            const auto retry_first = frames(1, "broken-chain recreation first source");
            oracle_check(retry_first.history_epoch > blocked.history_epoch &&
                         retry_first.counters.generated_frames == blocked.counters.generated_frames,
                         "broken-chain recreation reused pre-failure interpolation history");
            const auto retry = frames(5, "broken-chain recreation recovery");
            oracle_check(retry.effective == FrameGenerationMode::Fsr3 && retry.capability.available && retry.history_ready &&
                         retry.counters.generated_presented_frames > retry_first.counters.generated_presented_frames &&
                         backend.debug_frame_generation_provider_attempts() > attempts_after_fault,
                         "successful recreation did not recover provider work and generated native submissions");
            std::printf("%s recovery provider_attempts=%llu native_real=%llu native_generated=%llu epoch=%llu\n", tag,
                        static_cast<unsigned long long>(backend.debug_frame_generation_provider_attempts()),
                        static_cast<unsigned long long>(retry.counters.real_presented_frames),
                        static_cast<unsigned long long>(retry.counters.generated_presented_frames),
                        static_cast<unsigned long long>(retry.history_epoch));
            std::printf("%s %s: queued and real-only sources, exact provider attempts, stable unavailable reason, "
                        "native recreation recovery\n", tag, oracle_ok ? "PASS" : "FAIL");
            require(oracle_ok, "broken-chain fault oracle rejected interpolation during unavailable presentation");
        }
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
#ifdef __linux__
    if (process_gate) process_gate->cancel();
#endif
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
