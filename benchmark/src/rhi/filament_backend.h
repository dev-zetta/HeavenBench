/*
 * HeavenBench — Filament RHI Backend
 *
 * Wraps Google's Filament rendering engine behind the IRhiBackend interface.
 * Replaces the hand-rolled OpenGL 4.5 pipeline with Filament's PBR renderer,
 * built-in post-processing (bloom, SSAO, DOF, fog), and shadow mapping.
 */
#ifndef HEAVENBENCH_SRC_RHI_FILAMENT_BACKEND_H_
#define HEAVENBENCH_SRC_RHI_FILAMENT_BACKEND_H_

#include "rhi/rhi.h"
#include "rhi/render_feature.h"
#include "rhi/backend_core.h"
#include "rhi/depth_prepass.h"
#include "rhi/depth_proxy_builder.h"
#include "rhi/scene_color_capture.h"
#include "rhi/hdr_output.h"
#include "rhi/loading_screen.h"
#include "rhi/texture_store.h"
#include "rhi/mesh_store.h"
#include "rhi/material_instance_cache.h"
#include "rhi/scene_builder.h"
#include "rhi/scene_light_spawner.h"
#include "rhi/lighting_system.h"
#include "rhi/post_process.h"

#include "engine/path_resolver.h"
#include "engine/asset_file_system.h"

#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/Camera.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/ColorGrading.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <math/mat4.h>

#include <vector>
#include <memory>
#include <unordered_set>
#include <optional>
#include <string>
#include <chrono>

struct SDL_Window;

// filagui's ImGuiHelper translates Dear ImGui draw data into a Filament
// View/Scene. Forward-declared here so the heavy filagui/imgui headers stay
// out of this header; the unique_ptr below is fine with an incomplete type
// because ~FilamentBackend() is defined in the.cpp where it is complete.
namespace filagui { class ImGuiHelper; }
namespace filament { class FrameHistoryStream; }

namespace heaven {

class CloudFeature;
class VolumetricCloudFeature;
class GodraysFeature;
class SsgiFeature;
class VolfogFeature;
class GrassFeature;
class MassiveFoliageFeature;
class SkyFeature;
class ParticleFeature;
class LensFlareFeature;
class WindArrowsFeature;
class RainFeature;
class WetReflectionsFeature;
class ProfileBridge;
class FrameGenerationRuntime;
class FrameGenerationPlatform;
class DlssPlatform;
#ifdef __APPLE__
class MacOsSurface;
#endif

class FilamentBackend final : public IRhiBackend, public IResourceProvider {
public:
    explicit FilamentBackend(SDL_Window* window, const AppConfig& config);
    ~FilamentBackend() override;
    void set_trace_sink(profile::Recorder* recorder) override;
    void set_trace_frame_context(const profile::Context& context) override;
    [[nodiscard]] TraceCapabilities trace_capabilities() const override;

    [[nodiscard]] bool initialized() const noexcept {
        return engine_ && swapchain_ && renderer_ && view_;
    }
    [[nodiscard]] const std::string& initialization_error() const noexcept {
        return initialization_error_;
    }

    [[nodiscard]] GraphicsApi api() const noexcept override { return GraphicsApi::Filament; }
    [[nodiscard]] std::string device_name() const override;
    [[nodiscard]] std::string api_version() const override;
    [[nodiscard]] RendererFeatureStats renderer_feature_stats() const override;

    [[nodiscard]] GpuMesh  upload_mesh(const Mesh& mesh) override;
    [[nodiscard]] GpuMesh  upload_skinned(const SkinnedMesh& mesh) override;
    void release_mesh(const GpuMesh& mesh) override;
    [[nodiscard]] uint64_t load_texture(
        const std::string& path,
        TextureSemantic semantic = TextureSemantic::ScalarLinear) override;
    void set_asset_context(PathResolverRef paths,
                           AssetFileSystemRef assets) override;
    [[nodiscard]] Result<void> init_pipelines(const AppConfig& config) override;
    [[nodiscard]] Result<void>
    reconfigure_for_reload(const AppConfig& config) override;
    [[nodiscard]] Result<void> set_render_test_ground(
        std::size_t instance_index, glm::vec2 local_min,
        glm::vec2 local_max, float local_z) override;
    [[nodiscard]] Result<void> prepare_static_displacement(
        RhiScene& scene, const MaterialResolver& resolve,
        const LoadingProgressCallback& progress = {}) override;
    void show_loading_screen(const std::string& data_dir,
                             std::string_view status,
                             int viewport_w, int viewport_h,
                             float progress = -1.0f) override;

    FrameResult render_frame(std::span<const Instance> instances,
                      std::span<const int> dirty_instance_indices,
                      std::span<const GpuMesh> meshes,
                      const MaterialResolver& resolve_material,
                      const Camera& camera,
                      const LightingParams& lighting,
                      const WeatherState& weather,
                      const TonemapParams& tonemap,
                      int viewport_w, int viewport_h) override;

    void present() override;

    Result<void> update_mesh_vertices(uint64_t surface_handle,
                              const Vertex* vertices,
                              uint32_t vertex_count) override;

    Result<void> update_skinned_bones(
        int mesh_index, std::span<const glm::mat4> bone_matrices) override;

    // Whether the Animated_Object at `mesh_index` is actually GPU-skinned this
    // run (per-object fallback). Reports std::nullopt until the
    // persistent scene is built (SceneBuilder records skinning-enabled
    // renderables during build()); thereafter true when a skinned renderable
    // exists for the object and false when GPU skinning did not take effect for
    // it (e.g. bone overflow), so the main loop drives that object through the
    // CPU_Skinning_Path instead.
    [[nodiscard]] std::optional<bool> gpu_skinning_active(
        int mesh_index) const override;

    void set_particle_emitters(std::span<const ParticleEmitter> emitters) override;

    void set_scene_lights(std::span<const SceneLight> lights) override;

    void update_floating_lights(
        std::span<const std::pair<int, float>> group_offsets) override;

    void set_wind_debug_arrows(
        std::span<const WindArrow> arrows) override;

    // ── Runtime feature toggle (supported runtime API) ──────────
    // Enable/disable a registered render feature by name, driving its
    // IRenderFeature::set_enabled(render_ctx_, on) so the feature's
    // on_enabled_changed() add/remove path runs exactly as for any runtime
    // toggle. Keeping render_ctx_ (the non-const RenderContext a toggle
    // requires) encapsulated in the backend means callers only need the
    // feature name. No-op if no feature matches (or name is null), so a
    // configuration toggle that maps to no registered feature leaves the
    // renderer unchanged. The in-UI configuration controller drives live
    // feature toggles through this API; debug_set_feature_enabled() delegates
    // here so the gated smoke harness exercises the same path.
    void set_feature_enabled(const char* name, bool on) override;

    void set_camera_config(
        bool enabled, const PhysicalCameraConfig& config) override;

    void set_live_render_config(const FeatureToggles& features,
                                const QualityKnobs& quality) override;

    void set_benchmark_measurement(bool enabled) override;
    void request_frame_generation(FrameGenerationMode mode) override;
    void begin_application_frame(uint64_t application_frame) override;
    void end_application_simulation() override;
    void native_window_message(uint32_t message) override;
    [[nodiscard]] FrameGenerationStatus frame_generation_status() const override;
#if defined(HEAVEN_FG_FAULT_INJECTION) && HEAVEN_FG_FAULT_INJECTION
    [[nodiscard]] bool debug_frame_generation_out_of_date_once();
    [[nodiscard]] bool debug_frame_generation_work_failure_once();
    [[nodiscard]] uint64_t debug_frame_generation_work_failure_source() const;
    [[nodiscard]] uint64_t debug_frame_generation_provider_attempts() const;
#endif
    [[nodiscard]] std::vector<double>
    finalize_benchmark_gpu_times() override;

    [[nodiscard]] Result<void> request_screenshot(
        const std::filesystem::path& path) override;
    [[nodiscard]] Result<bool> screenshot_complete() const override;
    void set_capture_time(std::optional<float> seconds) override;
    [[nodiscard]] glm::vec2 render_resolution_scale() const override;

    // ── Window / swap-chain resize ──────────────────────────────
    // Recreate the Vulkan swap chain for the window's current drawable size.
    // Called by the Main_Loop after it resizes the OS window (in-UI resolution
    // change, fullscreen toggle, or a window-manager resize). Re-reads the
    // native window (refreshing the Wayland extent), flushes in-flight GPU work,
    // and rebuilds the swap chain with the same HDR flag. The per-frame viewport
    // is taken from render_frame's arguments, so no view state changes here.
    void resize(int width, int height) override;

    // ── Overlay (Dear ImGui via filagui) ────────────────────────
    // Constructs the filagui ImGuiHelper over a dedicated translucent
    // overlay View (ORTHO, post-processing OFF, shadows OFF — set up by
    // the helper). On failure the overlay is left disabled and the
    // renderer keeps running. See the.cpp for details.
    void overlay_init(const AppConfig& config) override;
    void overlay_begin_frame(float dt, int w, int h) override;
    [[nodiscard]] bool overlay_handle_event(const void* sdl_event) override;
    void overlay_set_draw(OverlayDraw build_widgets) override;

    // ── Frame pacing  ───────────────────────────
    // True — the constructor configures IMMEDIATE present, refreshRate=0,
    // and frameRate.interval=0 (no vsync / no frame-rate cap).
    [[nodiscard]] bool frame_pacing_disabled() const override;

    // ── GPU / render-API info for the Result_Report  ─
    // gpu_info() reads the active rendering backend's graphics device: on
    // the Vulkan path the selected VkPhysicalDevice (model name and summed
    // DEVICE_LOCAL heap sizes → VRAM MB); on the OpenGL fallback the cached GL
    // vendor/renderer/version strings with VRAM left undetermined. This is the
    // ONLY place GPU-API
    // specifics are read (AGENTS.md); the value crosses the RHI boundary
    // as the neutral GpuInfoRaw. render_api_name() returns the live driver
    // path Filament actually created ("Vulkan" / "OpenGL"), distinct from
    // the fixed api() family string.
    [[nodiscard]] GpuInfoRaw gpu_info() const override;
    [[nodiscard]] GpuTelemetry gpu_telemetry() const override;
    [[nodiscard]] std::string render_api_name() const override;

    // ── Per-frame discipline counters  ────────
    // Returns how many times the backend ran each one-time build site versus
    // the per-frame transform sync, letting a gated smoke case confirm the
    // per-frame path mutates only transforms and never re-runs the builders.
    [[nodiscard]] BackendBuildCounters debug_build_counters() const override;

    // ── Test-only render-context accessor (gated smoke harness) ──
    // Exposes the shared RenderContext (engine / scene / resources) so a
    // gated smoke case can drive a single feature's init/update against
    // the live backend without rendering a frame — used by the RainFeature
    // asset-load-failure case. Valid only
    // after init_pipelines() has populated the context. Like
    // debug_build_counters(), this is test instrumentation: the renderer
    // never calls it, and it adds no per-effect state to the backend.
    [[nodiscard]] RenderContext& debug_render_context() { return render_ctx_; }

    // ── Test-only render-feature lookup (gated smoke harness) ───
    // Scans render_features_ for the registered feature whose name()
    // matches `name` and returns it (or nullptr if none / name is null).
    // Lets a gated smoke case assert depth-pre-pass participation against
    // the live "rain" feature  — e.g. that
    // RainFeature::needs_depth_prepass() is honored after an active frame —
    // without the backend growing any per-effect access path. Test
    // instrumentation only: the renderer never calls it. Mirrors
    // debug_render_context() / debug_build_counters().
    [[nodiscard]] const IRenderFeature* debug_find_feature(const char* name) const;

    // ── Test-only render-feature count (gated smoke harness) ────
    // Counts how many entries in render_features_ have name() equal to
    // `name` (0 if name is null). Where debug_find_feature() only proves
    // a feature with a given name EXISTS, this lets a gated smoke case
    // assert there is EXACTLY ONE — the feature-registration contract that
    // each new effect is registered once in init_features (weather spec
    // Test instrumentation only: the renderer
    // never calls it; adds no per-effect state to the backend. Mirrors
    // debug_find_feature() / debug_depth_texture().
    [[nodiscard]] int debug_count_features(const char* name) const;

    // ── Test-only shared depth-texture accessor (gated smoke) ───
    // Returns the shared half-res depth pre-pass texture, or nullptr if
    // depth_prepass_ is null or the pass has not yet created it. After an
    // active rain frame this is non-null, proving the shared depth pre-pass
    // actually ran because a depth-sampling feature (rain) requested it
    // Test instrumentation only: the
    // renderer never calls it; adds no per-effect state to the backend.
    [[nodiscard]] const filament::Texture* debug_depth_texture() const;
    [[nodiscard]] std::optional<glm::mat4> debug_scene_color_view_proj() const;

    // ── Test-only feature toggle (gated smoke harness) ──────────
    // Thin alias kept for the existing gated smoke cases: delegates to the
    // supported runtime API set_feature_enabled(name, on) so the toggle path
    // is exercised identically whether driven by a test or the in-UI
    // configuration controller. No-op if no feature matches (or name is
    // null). Mirrors debug_find_feature() / debug_count_features().
    void debug_set_feature_enabled(const char* name, bool on);

    // ── IResourceProvider (for render features) ─────────────────
    [[nodiscard]] filament::Texture* get_filament_texture(uint64_t handle) override;
    void record_external_original_texture(
        uint32_t width, uint32_t height, TextureSemantic semantic,
        bool complete_mip_chain) override;
    [[nodiscard]] filament::Texture* placeholder_texture() override;
    [[nodiscard]] filament::Texture* placeholder_normal_texture() override;
    [[nodiscard]] filament::Texture* placeholder_depth_texture() override;

private:
    std::unique_ptr<ProfileBridge> profile_bridge_;
    // Consume newly completed Filament FrameInfo entries exactly once. The
    // latest duration from each drain is stored for the live performance log;
    // benchmark samples are accumulated independently.
    void collect_completed_gpu_times();

    // ── Filament core objects ───────────────────────────────────
    [[nodiscard]] Result<void> validate_hdr_surface() const;
    std::string initialization_error_ = "Failed to initialize the Filament renderer";
    filament::Engine*       engine_     = nullptr;
    filament::SwapChain*    swapchain_  = nullptr;
    filament::Renderer*     renderer_   = nullptr;
    filament::Scene*        scene_      = nullptr;
    filament::View*         view_       = nullptr;
    filament::Camera*       fil_camera_ = nullptr;
    utils::Entity           camera_entity_;
#ifdef __APPLE__
    std::unique_ptr<MacOsSurface> macos_surface_;
#endif

    // Non-owning bundle of the core objects above, handed to the
    // collaborator subsystems (mesh / texture / material stores, scene
    // builder, light spawner, lighting, depth pre-pass). Populated in
    // build_render_context() once the core objects exist.
    BackendCore             core_{};

    // ── Render features (modular, toggleable) ───────────────────
    // Shared state handed to every feature, plus the owning vector.
    // Built in init_pipelines() once the core objects exist.
    RenderContext                              render_ctx_{};
    std::unique_ptr<class RenderTestSceneAssets> render_test_assets_;
    // Application-owned logical asset context borrowed for the backend
    // lifetime. Application tears the backend down before these owners.
    PathResolverRef                            path_resolver_;
    AssetFileSystemRef                         asset_file_system_;
    std::vector<std::unique_ptr<IRenderFeature>> render_features_;
    int shared_target_width_ = 0;
    int shared_target_height_ = 0;
    CloudFeature*                              cloud_feature_ = nullptr;
    VolumetricCloudFeature*                    volumetric_cloud_feature_ = nullptr;
    GodraysFeature*                            godrays_feature_ = nullptr;
    SsgiFeature*                               ssgi_feature_ = nullptr;
    VolfogFeature*                             volfog_feature_ = nullptr;
    GrassFeature*                              grass_feature_ = nullptr;
    MassiveFoliageFeature*                     massive_foliage_feature_ = nullptr;
    SkyFeature*                                sky_feature_ = nullptr;
    ParticleFeature*                           particle_feature_ = nullptr;
    LensFlareFeature*                          lens_flare_feature_ = nullptr;
    WindArrowsFeature*                         wind_arrows_feature_ = nullptr;
    // Non-owning observer pointer for build-once instrumentation only
    // (mirrors grass_feature_): debug_build_counters() reads the rain
    // streak / splash build counts through it. render_features_ owns the
    // feature; this adds no per-effect state or logic to the backend.
    RainFeature*                               rain_feature_ = nullptr;
    AppConfig                                  config_{};

    // Scene light spawner: per-luminaire emission lights + world-authored
    // lamp/accent lights, their per-frame world-position updates, and the
    // flattened entity + SceneLightInfo lists handed to features through
    // FrameContext (volfog reads the entities; lens flares the descriptors).
    std::unique_ptr<SceneLightSpawner>         light_spawner_;

    void build_render_context();
    void init_features(const AppConfig& config);
    void update_features(const FrameContext& frame);
    void destroy_features();

    // ── Lighting ────────────────────────────────────────────────
    // Sun + moon + IBL cubemap/Skybox + bloom retune live in the
    // LightingSystem subsystem. Camera exposure is applied exactly once in
    // update_camera. color_grading_ stays here (the one
    // post-process object whose lifetime the backend owns).
    std::unique_ptr<LightingSystem> lighting_;
    filament::ColorGrading* color_grading_ = nullptr;
    ColorGradingCache color_grading_cache_;

    // ── Materials ───────────────────────────────────────────────
    // Base materials (heaven_lit / heaven_lit_twosided / heaven_unlit)
    // and the per-surface MaterialInstance cache live in the
    // MaterialInstanceCache subsystem.
    std::unique_ptr<MaterialInstanceCache> mat_cache_;

    // ── Resource tracking ───────────────────────────────────────

    // Texture store: DDS loading + path/handle cache + the shared
    // placeholder textures. The backend's IResourceProvider overrides
    // delegate to this; the material cache resolves texture handles
    // through it.
    std::unique_ptr<TextureStore> texture_store_;

    // Mesh store: GPU mesh upload (tangent computation, VB/IB) +
    // CPU-skinning re-upload, keyed by surface handle. The scene builder
    // looks up a surface's VB/IB via mesh_store_->find(handle).
    std::unique_ptr<MeshStore> mesh_store_;

    // Startup splash: draws the HeavenBench loading image while CPU-side
    // scene/material loading blocks the normal render loop.
    std::unique_ptr<LoadingScreen> loading_screen_;

    // Scene builder: persistent renderable entities + per-frame transform
    // sync. Owns the scene-entity list and the "scene built" flag.
    std::unique_ptr<SceneBuilder> scene_builder_;

    bool lighting_initialized_ = false;
    bool pipelines_initialized_ = false;
    float render_time_ = 0.0f;
    std::optional<float> capture_time_;
    struct ScreenshotReadback;
    std::shared_ptr<ScreenshotReadback> screenshot_readback_;
    std::optional<std::filesystem::path> screenshot_path_;
    Result<bool> screenshot_result_ = false;
    filament::Texture* screenshot_hdr_texture_ = nullptr;
    filament::RenderTarget* screenshot_hdr_target_ = nullptr;
    int screenshot_hdr_width_ = 0;
    int screenshot_hdr_height_ = 0;
    int   frame_count_ = 0;

    // ── Weather-dependent Filament state  ───────
    // The weather-dependent post-process / lighting / distance-fog setup is
    // applied once on the first frame (seeding prev_weather_) and then re-applied
    // by render_frame only when the per-frame WeatherState differs from
    // prev_weather_ (by its derived-output signature). weather_initialized_ is
    // set true once the first-frame baseline has seeded prev_weather_, so the
    // change check is independent of any other one-time init flag. On a
    // re-application failure prev_weather_ is NOT advanced, so the last good
    // state stays live.
    WeatherState prev_weather_;
    bool         weather_initialized_ = false;

    // MSAA sample count resolved against GPU capabilities once at init
    // config_.quality.msaa_samples is the requested value; this is
    // the highest GPU-supported count <= request (or 1). configure_post_processing
    // receives a QualityKnobs carrying this resolved count so the per-frame
    // path never re-queries or re-warns.
    int   msaa_resolved_samples_ = 1;

    // ── Per-frame discipline counters  ────────
    // Debug-only instrumentation. scene_builds / emission_spawns are bumped
    // inside the one-time `if (!scene_builder_->built())` block; transform_updates
    // is bumped every frame where update_transforms() runs. grass_generations is
    // sourced from grass_feature_->generation_count() on demand in
    // debug_build_counters(). Cheap uint64 increments, safe in the hot path.
    BackendBuildCounters build_counters_{};

    // ── Grass system ────────────────────────────────────────────
    // Migrated to rhi/features/grass_feature.{h,cpp}.

    // ── Lens flare (lamp / torch glare on close approach) ──────
    // Migrated to rhi/features/lens_flare_feature.{h,cpp}. The backend
    // owns the lights they anchor to and hands the feature read-only
    // SceneLightInfo descriptors via FrameContext::scene_lights.

    // ── Wind debug arrows (cloth simulation visualization) ─────
    // Migrated to rhi/features/wind_arrows_feature.{h,cpp}. The backend
    // forwards the per-frame arrow list via set_wind_debug_arrows().

    // ── Cloud system ────────────────────────────────────────────
    // Migrated to rhi/features/cloud_feature.{h,cpp}. Owned by the
    // render_features_ vector below.

    // ── Sky-sphere system ───────────────────────────────────────
    // Migrated to rhi/features/sky_feature.{h,cpp}. The IBL probe
    // (procedural cubemap) is owned by the LightingSystem subsystem
    // because it drives IndirectLight reflections — lighting
    // infrastructure, not the visible sky. The backend keeps Filament's
    // cubemap Skybox attached as the background only while the sky
    // feature is off (LightingSystem::setup handles that).

    // ── God rays system ─────────────────────────────────────────
    // Migrated to rhi/features/godrays_feature.{h,cpp}. The shared
    // depth pre-pass infrastructure is owned by the DepthPrePass
    // subsystem below because volfog and heat haze also consume it.
    std::unique_ptr<DepthPrePass> depth_prepass_;
    std::unique_ptr<FrameGenerationRuntime> frame_generation_;
    void destroy_frame_generation();
#if HEAVEN_HAS_FSR3
    std::unique_ptr<FrameGenerationPlatform> frame_generation_platform_;
#endif
#if HEAVEN_HAS_DLSS
    std::unique_ptr<DlssPlatform> dlss_platform_;
#endif

    // ── Depth proxy renderables (scene-depth source) ────────────
    // The DepthPrePass renders a parallel set of lightweight "proxy"
    // renderables — built once, sharing the visible scene's VB/IB and the
    // shared `heaven_depth` MaterialInstance — into its proxy Scene to produce
    // the linear scene-depth texture. The backend owns the builder because it
    // holds the MeshStore + the per-frame instance/mesh spans the build needs;
    // the proxy Scene + shared MaterialInstance live on depth_prepass_. Built
    // lazily and transform-synced only while a consumer needs depth (see
    // sync_depth_proxies); torn down before depth_prepass_ in the destructor.
    std::unique_ptr<DepthProxyBuilder> depth_proxy_builder_;

    // ── Scene-colour capture (previous-frame history for SSGI) ──
    // Ping-pong colour targets + blit. Engaged only when a feature returns
    // wants_scene_color() (SSGI); otherwise the main view renders straight to
    // the swap chain. `want_scene_color_` is computed in update_render_features
    // (so the SSGI feature can bind last frame's colour in its update, outside
    // any render pass) and consumed in render_views.
    WetReflectionsFeature* wet_reflections_feature_ = nullptr;
    std::unique_ptr<SceneColorCapture> scene_color_capture_;
    bool                               want_scene_color_ = false;

    // HDR10 output encode. Engaged only when HDR display output is active
    // (hdr_ == true). The main view (and any SSGI/heat-haze composite that would
    // target the swap chain) renders into hdr_output_->scene_rt() — a linear
    // Rec.2020 fp16 target — and encode() applies the PQ OETF to the swap chain
    // as the final stage. Null/unused on the SDR path.
    std::unique_ptr<HdrOutput>         hdr_output_;
    bool                               hdr_active_ = false;

    // ── Volumetric fog system ───────────────────────────────────
    // Migrated to rhi/features/volfog_feature.{h,cpp}. It consumes the
    // shared depth pre-pass (above) and the scene_light_entities_ list.

    // ── Helpers ─────────────────────────────────────────────────
    void* get_native_window(SDL_Window* sdl_window);

    // ── render_frame steps ──────────────────────────────────────
    // render_frame is a thin sequencer over these named steps; each
    // owns one logical phase of the frame. The MaterialResolver / spans
    // come straight from the render_frame arguments.
    void  update_camera(const Camera& camera, const LightingParams& lighting,
                        int viewport_w, int viewport_h);
    // Builds the per-frame FrameContext, ensures the shared depth
    // target if any feature needs it, and updates all features.
    // Returns whether a depth pre-pass is required this frame.
    [[nodiscard]] bool update_render_features(
        std::span<const Instance> instances,
        std::span<const int> dirty_instance_indices,
        std::span<const GpuMesh> meshes,
        const MaterialResolver& resolve_material,
        const Camera& camera,
        const LightingParams& lighting,
        const WeatherState& weather,
        int viewport_w, int viewport_h);
    // Build the depth-proxy renderables once the scene geometry + shared depth
    // material are available, then sync their transforms to the animated
    // instance list each subsequent frame. Called only while a consumer needs
    // the depth pre-pass, so the proxy build/sync cost is not incurred when
    // nothing samples scene depth.
    void  sync_depth_proxies(std::span<const Instance> instances,
                             std::span<const int> dirty_instance_indices,
                             std::span<const GpuMesh> meshes,
                             const MaterialResolver& resolve_material);
    // Compute the set of mesh indices to EXCLUDE from GPU instancing. A mesh is
    // excluded when:
    //   - any surface was marked skinned by MeshStore::upload_skinned (an
    //     Animated_Object — never instanced); or
    //   - any surface resolves to an alpha-masked (foliage) material
    //     (gpu_mat.alpha_ref > 0). An instanced masked depth proxy does not
    //     write the correct alpha-tested silhouette into the half-res
    //     scene-depth texture, so the screen-space depth consumers (volumetric
    //     fog, god rays, SSGI) composite atmosphere in front of the see-through
    //     foliage — a dithered halo around every tree. Opaque repeated meshes
    //     (the 170+ floating islands and props — the real instancing win) are
    //     unaffected and keep the instanced path; foliage falls back to the
    //     proven per-instance path (identical to --no-gpu-instancing for those
    //     meshes).
    // Returns empty when GPU instancing is disabled. SceneBuilder and
    // DepthProxyBuilder MUST be given the SAME set so their shared, deterministic
    // compute_instance_groups() yields identical chunks and cannot drift
    // this helper is the single source of that set.
    [[nodiscard]] std::unordered_set<int> instancing_excluded_mesh_indices(
            std::span<const GpuMesh> meshes,
            const MaterialResolver& resolve_material) const;
    // Build overlay geometry and material bindings before beginFrame commits
    // Filament's surface descriptors. Drawing still happens after the scene.
    [[nodiscard]] bool prepare_overlay(int viewport_w, int viewport_h);

    // Read the final swap-chain image inside the frame, then wait / write only
    // after endFrame so readback never sees a partially composed image.
    void submit_screenshot(int viewport_w, int viewport_h);
    void finish_screenshot();
    void prepare_screenshot_target(int viewport_w, int viewport_h);
    void destroy_screenshot_target();
    [[nodiscard]] uint64_t capture_frame_timestamp();

    // Orchestrates the depth pre-pass, scene-capture/composite path,
    // and the main + composite view renders inside one begin/endFrame.
    [[nodiscard]] bool render_views(const Camera& camera,
                                    int viewport_w, int viewport_h,
                                    bool need_depth_pass);

    // Apply the weather-dependent Filament state for `weather`: the
    // lighting/bloom setup, the distance-fog override (the
    // `fog_density > 0 ? density: -1.0f` sentinel, so density-0 keeps distance
    // fog OFF, matching the Shipping_Look), and the configure_post_processing
    // call. render_frame runs this once on the first frame to seed the baseline
    // and again whenever the per-frame WeatherState changes;
    // factoring it here keeps the first-frame init and the per-frame re-apply on
    // one code path. Returns an Error (rather than throwing) if the engine/view
    // are unavailable so the caller can retain the last good state and keep
    // rendering.
    [[nodiscard]] Result<void> apply_weather_state(const WeatherState& weather,
                                                   const LightingParams& lighting,
                                                   const TonemapParams& tonemap);

    // ── Overlay (Dear ImGui via filagui) ────────────────────────
    // The helper owns its own Scene/Camera and draws into overlay_view_
    // (a dedicated translucent View the backend owns). overlay_enabled_
    // is false until overlay_init() succeeds and gates all overlay work,
    // so a construction failure leaves the renderer running without an
    // overlay. overlay_dt_ stashes the frame delta for the helper's
    // render(dt, cb) call (driven by); overlay_draw_ holds the
    // per-frame widget-building callback set via overlay_set_draw().
    std::unique_ptr<filagui::ImGuiHelper> imgui_helper_;
    filament::View*                       overlay_view_ = nullptr;
    OverlayDraw                           overlay_draw_;
    float                                 overlay_dt_ = 0.0f;
    float                                 overlay_ui_scale_ = 0.0f;
    bool                                  overlay_enabled_ = false;

    // Persistent cursor over Filament's asynchronous frame history. Keeping
    // this alive across frames prevents duplicate sampling of the newest
    // completed frame. Benchmark samples are additionally bounded by the
    // steady-clock timestamp captured before the first measured submission.
    std::unique_ptr<filament::FrameHistoryStream> gpu_history_stream_;
    double collected_gpu_ms_ = -1.0;
    std::vector<double> benchmark_gpu_times_;
    int64_t benchmark_gpu_start_ns_ = 0;

    SDL_Window* window_ = nullptr;

    // HDR display output active (requested via --hdr and an HDR10 swap chain).
    // Set at construction; gates the PQ output-encode path over the SDR tonemap.
    bool hdr_ = false;

    // True only while the graphical score is being measured. Dynamic
    // resolution is forced off in this interval so GPU scores always represent
    // the requested drawable resolution and are comparable between systems.
    bool benchmark_measurement_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FILAMENT_BACKEND_H_
