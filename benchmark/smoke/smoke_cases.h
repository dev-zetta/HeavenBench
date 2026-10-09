/*
 * HeavenBench — renderer-linked smoke harness (shared declarations)
 *
 * The `heaven_benchmark_smoke` target links the FULL renderer (the same
 * Filament + SDL libraries as `heaven_benchmark`) so it can exercise the live
 * Filament backend. Because that needs a real GPU/display, the harness is
 * GATED and skipped in headless CI:
 *
 *   - It is OPT-IN: real assertions run only when HEAVEN_SMOKE=1 is set.
 *     Without it every case prints a SKIPPED line and the process exits 0, so
 *     `ctest` stays green on developer machines and CI that don't explicitly
 *     request the renderer-linked smoke run.
 *   - It also requires a display: if neither DISPLAY nor WAYLAND_DISPLAY is
 *     set (or SDL/window/backend bring-up fails — the headless case), every
 *     case is skipped and the process exits 0.
 *
 * The harness is MULTI-CASE: a single main() (smoke_main.cpp) brings up one
 * window + backend, then runs each registered smoke case against it, reporting
 * per-case PASS / SKIP / FAIL. Keeping exactly one main() across the smoke
 * sources lets the target (which GLOBs the smoke directory) link cleanly as
 * new cases are dropped in:
 *
 *   - smoke_frame_pacing.cpp  — frame-pacing case
 *   - smoke_overlay_render.cpp — overlay-render case
 *   - smoke_perframe_discipline.cpp — per-frame discipline case
 *
 * A case function NEVER owns the window/backend lifecycle (the harness does);
 * it only asserts against the supplied backend and returns its result. The
 * process exits non-zero only when a case returns Fail; Skip and Pass both
 * exit 0 so the gated test passes in headless CI.
 */
#ifndef HEAVENBENCH_SMOKE_SMOKE_CASES_H_
#define HEAVENBENCH_SMOKE_SMOKE_CASES_H_

namespace heaven {
class IRhiBackend;
struct AppConfig;
struct WindowHandle;
} // namespace heaven

namespace heaven::smoke {

// Outcome of one smoke case. Skip and Pass both keep the process exit code 0
// (the gate is satisfied or the case opted out); only Fail flips it to 1.
enum class CaseResult { Pass, Skip, Fail };

// ── Shared gates (defined in smoke_main.cpp) ────────────────────

// True when the renderer-linked smoke run is explicitly opted into via
// HEAVEN_SMOKE=1. Renderer-linked cases need a real GPU/display, so they stay
// off by default to keep headless CI green.
[[nodiscard]] bool smoke_opt_in();

// True when a windowing display appears to be available. Mirrors the env vars
// SDL inspects on Linux; the actual SDL_Init / window / backend creation in
// the harness is the authoritative check (and is skipped, exit 0, on failure).
[[nodiscard]] bool display_available();

// ── Case interface ──────────────────────────────────────────────

// Per-case inputs: a backend already constructed by the harness against a real
// native window, the config used to create it, and the drawable extent. A case
// may further bring the backend up (e.g. init_pipelines / overlay_init) as its
// scenario requires; if that bring-up fails it returns Skip (headless), and it
// only asserts once fully brought up.
struct CaseContext {
    IRhiBackend&     backend;
    const AppConfig& config;
    int              width  = 0;
    int              height = 0;
};

// Production-material readback against an independent world-space blocker oracle.
[[nodiscard]] CaseResult run_specular_occlusion_case(const CaseContext& ctx);

[[nodiscard]] CaseResult run_window_lifecycle_case(
    const CaseContext& ctx, WindowHandle& window);

// Production loading UI readback: progress fill and stage text remain visible.
[[nodiscard]] CaseResult run_loading_progress_case(const CaseContext& ctx);
[[nodiscard]] CaseResult run_microprofiler_case(const CaseContext& ctx);

// Offscreen readback of the actual HDR encode material. Checks numerical PQ,
// color conversion, premultiplied UI composition and transparent-layer clearing.
[[nodiscard]] CaseResult run_hdr_output_case(const CaseContext& ctx);

// Frame-pacing smoke case: assert frame_pacing_disabled()
// is true on the freshly-constructed backend. Defined in smoke_frame_pacing.cpp.
[[nodiscard]] CaseResult run_frame_pacing_case(const CaseContext& ctx);

// GPU / render-API smoke case (Requirements
// against the live Filament backend assert render_api_name() is
// exactly one of "Vulkan"/"OpenGL" matching the driver actually created, and
// gpu_info() reports a non-empty graphics-device model; on the Vulkan path it
// further asserts a non-negative VRAM (summed DEVICE_LOCAL heaps → MB) and a
// selected device's VRAM is non-negative, while the OpenGL fallback leaves
// VRAM undetermined by design  so only the model + API
// name are required. SKIPs (never fakes a pass) when render_api_name() is empty
// (no live engine / headless software path). Defined in smoke_gpu_info.cpp.
[[nodiscard]] CaseResult run_gpu_info_case(const CaseContext& ctx);

// Overlay-render smoke case: bring the
// backend fully up, enable the overlay, render a single frame with it enabled,
// and assert it renders without error, the ImGui view is drawn after the main
// view, and no render feature is toggled off relative to overlay-off. Defined
// in smoke_overlay_render.cpp.
[[nodiscard]] CaseResult run_overlay_render_case(const CaseContext& ctx);

// Per-frame discipline smoke case: bring the backend up,
// render several empty-scene frames, and assert via debug_build_counters() that
// the per-frame path mutates only transforms (transform_updates grows every
// frame) and never re-runs the one-time scene/emission/grass/bbox builders
// (their counts stay at their frame-1 values). Defined in
// smoke_perframe_discipline.cpp.
[[nodiscard]] CaseResult run_perframe_discipline_case(const CaseContext& ctx);

// Rain asset-load-failure smoke case:
// construct a RainFeature against the live backend with a bogus data_dir so the
// precipitation sprite textures fail to load, and assert the feature stays
// disabled (enabled() == false, needs_depth_prepass() == false, zero build
// counts), adds no renderables to the scene, emits a diagnostic naming the
// failed resource, and that init returns normally. Defined in
// smoke_rain_asset_failure.cpp.
[[nodiscard]] CaseResult run_rain_asset_failure_case(const CaseContext& ctx);

// Rain build-once discipline smoke case (Requirements
// with rain enabled and intensity > 0, render several
// empty-scene frames and assert via debug_build_counters() that the rain-streak
// and splash build counters each reach 1 after the first active frame and never
// increment again, while the per-frame uniform/transform path (transform_updates)
// keeps growing one per frame. Defined in smoke_rain_build_once.cpp.
[[nodiscard]] CaseResult run_rain_build_once_case(const CaseContext& ctx);

// Rain depth-pre-pass participation smoke case
// with rain enabled and intensity > 0, render a few active
// frames and assert via the backend's test-only seams that the rain feature's
// needs_depth_prepass() is honored — debug_find_feature("rain")->needs_depth_prepass()
// is true once enabled+built, debug_depth_texture() is non-null (the shared
// depth pass ran because rain requested it), and the rain feature bound that
// exact shared texture (reads scene depth only through FrameContext::depth_texture).
// Defined in smoke_rain_depth_prepass.cpp.
[[nodiscard]] CaseResult run_rain_depth_prepass_case(const CaseContext& ctx);

// GPU-skinning smoke case
// the four Animated_Objects are skinned
// on the GPU via Filament's native skinning when gpu_skinning is enabled (the
// default). HALF A — against the LIVE backend — brings the pipeline up, renders
// several empty-scene frames, and asserts via debug_build_counters() the
// build-once discipline (scene_builds==1, per-frame transforms only) holds while
// the two GPU-skinning RHI seams are safe + self-consistent: with no
// Animated_Object in the empty harness scene, gpu_skinning_active() for any mesh
// index reports a defined false once built and update_skinned_bones() is a
// benign Ok no-op that does NOT trigger a scene rebuild (the per-frame bone path
// never rebuilds geometry —). HALF B — HERMETIC on a stand-alone NOOP
// engine + MeshStore + MaterialInstanceCache + SceneBuilder (mirrors
// smoke_mesh_store.cpp / smoke_glass_refraction.cpp), because the 64x64 harness
// renders an empty scene so no Animated_Object reaches the live SceneBuilder —
// asserts against a synthetic skinned mesh: (B1) a 3-bone mesh uploaded via
// MeshStore::upload_skinned is marked skinned (skinned_info ⇒ {true,3}) and
// SceneBuilder built with gpu_skinning=true records a skinning-enabled
// renderable (skinned_entities non-empty, the seam gpu_skinning_active /
// update_skinned_bones route through, with a real RenderableManager component,
// (B2) driving the exact per-frame bone push update_skinned_bones
// makes (bone_matrices_interpolated → glm_to_filament → setBones) over several
// frames leaves the surface's VB/IB handles and the recorded skinned-entity set
// unchanged — bones-only, no geometry/renderable rebuild; (B3) the
// same mesh built with gpu_skinning=false records NO skinned renderable, and a
// statically-uploaded surface (the --no-gpu-skinning upload) is never marked
// skinned (skinned_info ⇒ {false,0}) while its update_vertices
// (== update_mesh_vertices) rest-pose re-upload succeeds; and
// (B4) a 300-bone mesh (over Filament's 255-bone limit) still uploads its
// rest-pose geometry but is left UN-marked (skinned_info ⇒ {false,0}) and
// SceneBuilder records no skinned renderable for it even with gpu_skinning=true —
// the object falls back to the CPU_Skinning_Path, MeshStore emitting the
// one-time over-limit diagnostic to stderr (the diagnostic text is covered by
// code inspection; the harness does not capture it), with the marked 3-bone mesh
// the positive control proving the fallback is a genuine branch difference
// The pure grouping/bone-matrix logic is owned by the host
// property tests; on-device skinned motion and pixel parity require separate
// full-scene captures and comparison. Defined in smoke_gpu_skinning.cpp.
[[nodiscard]] CaseResult run_gpu_skinning_case(const CaseContext& ctx);

// GPU-instancing smoke case
// repeated static meshes are drawn as Filament
// instanced renderables when gpu_instancing is enabled (the default) — Instances
// that share a mesh_index (excluding skinned Animated_Objects and singletons)
// are grouped by compute_instance_groups, any group over
// Engine::getMaxAutomaticInstances() is split into chunks, and ONE instanced
// renderable is built per non-empty Shadow_Split subset per chunk (built once
// with RenderableManager::Builder::instances(chunkSize, InstanceBuffer*), then
// only its per-instance local transforms refreshed each frame). Disabling the
// feature (--no-gpu-instancing) restores the per-instance path. HALF A — against
// the LIVE backend (mirrors smoke_perframe_discipline.cpp / smoke_gpu_skinning.cpp
// HALF A) — brings the pipeline up, renders several empty-scene frames, and
// asserts via debug_build_counters() the build-once discipline (scene_builds==1,
// per-frame transforms only) that for instancing means the instanced renderables
// + InstanceBuffers never rebuild after the first build. HALF B —
// HERMETIC on a stand-alone NOOP engine + MeshStore + MaterialInstanceCache +
// SceneBuilder (mirrors smoke_gpu_skinning.cpp HALF B), because the 64x64 harness
// renders an empty scene so no instanced group reaches the live SceneBuilder —
// asserts against a synthetic single-surface repeated static mesh, observed
// through the Filament Scene's getRenderableCount() (the seam
// smoke_rain_toggle_roundtrip.cpp / smoke_ssgi.cpp use) and the pure
// compute_instance_groups helper: (B1) N (>1, <= max) Instances sharing one
// mesh_index built with gpu_instancing=true produce ONE instanced renderable
// (the single Shadow_Split subset of one chunk), NOT N — while the SAME N built
// with gpu_instancing=false produce N per-instance renderables, the disabled-path
// fallback; (B2) a synthetic group of M = max + 2 members splits
// into ceil(M/max) chunks covering all members exactly — asserted through both
// compute_instance_groups (group count == ceil, union of members == {0..M-1},
// every group in [2, max]) and SceneBuilder's realisation (exactly ceil(M/max)
// instanced renderables), degrading to the per-instance fallback assertion when
// the NOOP engine reports max < 2; and (B3) after the instanced
// scene is built once, driving several update_transforms frames with animated
// (changing) member transforms leaves the renderable count UNCHANGED and built()
// true — build-once, transform-refresh-only. The pure grouping logic is
// owned by the host property test (prop_instance_grouping) and the on-device
// placement and pixel parity require separate full-scene capture review. No new
// backend seam — the observable is the Scene's own getRenderableCount() plus the
// pure helper. Defined in smoke_gpu_instancing.cpp.
[[nodiscard]] CaseResult run_gpu_instancing_case(const CaseContext& ctx);
[[nodiscard]] CaseResult run_culling_case(const CaseContext& ctx);
// Actual cascade submissions and no-culling/off-camera-shadow pixel oracles.
[[nodiscard]] CaseResult run_cascade_shadow_culling_case(const CaseContext& ctx);
[[nodiscard]] CaseResult run_shadow_receivers_case(const CaseContext& ctx);
[[nodiscard]] CaseResult run_particle_lighting_case(const CaseContext& ctx);
[[nodiscard]] CaseResult run_night_lighting_case(const CaseContext& ctx);
[[nodiscard]] CaseResult run_lamp_ground_case(const CaseContext& ctx);

// Feature-registration smoke case
// bring the backend up (init_features runs inside init_pipelines) and
// assert via the backend's test-only seams that render_features_ contains
// EXACTLY ONE feature named "rain"  and EXACTLY ONE named
// "wet-reflections"  — debug_count_features(name) == 1 for each, with
// debug_find_feature(name) non-null for a clear missing-registration
// diagnostic. Registration is asset-independent (the features are pushed into
// render_features_ during init_features regardless of asset availability), so
// this case renders no frame and needs no rain-unavailable SKIP. Defined in
// smoke_feature_registration.cpp.
[[nodiscard]] CaseResult run_feature_registration_case(const CaseContext& ctx);

// Toggle isolation / round-trip smoke case
// bring rain active so it builds + enters the
// scene, then toggle rain off then on via the backend's test-only
// debug_set_feature_enabled() seam and assert through debug_render_context() +
// debug_build_counters() that (1) no OTHER feature's scene membership changes —
// the scene renderable count drops by exactly rain's renderables on OFF and
// returns to its pre-toggle value on ON; (2) rain's renderables are restored
// from the build-once buffers — the rain build counters stay at 1 across the
// cycle (no rebuild); and (3) the wet-reflections restore leaves the wetness
// material global at {0,0,0,1} and the SSR options at their shipping values when
// toggled off. Restores rain + wet-reflections to enabled at the end so the
// shared backend is left as found. Defined in smoke_rain_toggle_roundtrip.cpp.
[[nodiscard]] CaseResult run_rain_toggle_roundtrip_case(const CaseContext& ctx);

// Wet-reflections capability-unavailable smoke case (Requirement
// force WetReflectionsFeature's capability probe to fail (via a test
// subclass overriding the protected probe seam) against a hermetic NOOP engine
// + view pre-seeded with sentinel SSR options and material global, then assert
// the feature initialises DISABLED and leaves both untouched (Shipping_Look
// preserved). Self-contained — does not use ctx.backend. Defined in
// smoke_wet_reflections_capability.cpp.
[[nodiscard]] CaseResult run_wet_reflections_capability_case(const CaseContext& ctx);

// Glass refraction screen-data-fallback / disabled-parity smoke case
// screen-space
// refraction routes glass-flagged surfaces through the refractive
// `heaven_glass` material only when the refraction feature is enabled AND the
// screen-space scene color is available. Against a hermetic NOOP engine +
// TextureStore + MaterialInstanceCache, this case (a) forces the
// scene-color-availability seam (refraction_scene_color_available) to report
// unavailable via a test subclass and, with refraction enabled, asserts a
// glass-flagged GpuMaterial resolves to the opaque `heaven_lit` material (not
// `heaven_glass`) for that frame with a diagnostic; (b) asserts a
// glass surface with refraction DISABLED also resolves to opaque `heaven_lit`,
// pixel-matching the Shipping_Look; and (c) as a positive control,
// forces the seam available so the same glass surface routes to the refractive
// `heaven_glass` material — proving the opaque fallbacks are genuine branch
// differences. The base material name is read back via
// MaterialInstance::getMaterial()->getName(). Self-contained — does not use
// ctx.backend. Defined in smoke_glass_refraction.cpp.
[[nodiscard]] CaseResult run_glass_refraction_case(const CaseContext& ctx);

// Group-A advanced-material visual-presence / disabled-parity / cloth-sim
// independence smoke case
// the four
// Group-A advanced shading models (clear coat / anisotropic / subsurface /
// cloth) are routed per surface by the data-driven Material_Model_Selector in
// MaterialInstanceCache::get_or_create, gated by set_advanced_models(). Against
// a hermetic NOOP engine + TextureStore + MaterialInstanceCache (the NOOP
// engine produces no pixels, so — like smoke_glass_refraction.cpp — "visual
// presence" is expressed as the material-routing the lobes depend on, the
// genuine pixel parity requiring separate full-scene captures), this case asserts:
// (a) with each model enabled, its marked surface (dragon⇒clear-coat,
// gear⇒anisotropic, leaf⇒subsurface, flag⇒cloth) routes to the corresponding
// advanced base material — heaven_lit_clearcoat / heaven_lit_aniso /
// heaven_subsurface / heaven_cloth (presence); (b) with all
// advanced models disabled the same surfaces route back to the Shipping_Look
// heaven_lit / heaven_lit_twosided (≤1/255 parity); and
// (c) enabling exactly one model routes only that model's surface, the others
// staying Shipping_Look (per-model independence). The subsurface presence branch
// forces the screen-space-scene-data availability seam available via a test
// subclass (mirroring smoke_glass_refraction.cpp). For cloth-sim independence
// it steps an identical synthetic PBD cloth body twice with the same
// dt/time sequence — once with cloth shading inert and once after the cloth
// model is enabled and a flag surface routed to heaven_cloth — and asserts the
// particle positions are bit-identical (cloth shading never mutates cloth_sim,
// whose step_cloth takes no material/shading input), with the cloth having
// actually moved so the equality is not vacuous. The base material name is read
// back via MaterialInstance::getMaterial()->getName(). Self-contained — does not
// use ctx.backend. Defined in smoke_advanced_materials.cpp.
[[nodiscard]] CaseResult run_advanced_materials_case(const CaseContext& ctx);

// Group-B shadows-&-lighting presence / disabled-parity smoke case

// the three Group-B capabilities —
// transparent foliage shadows, the safe colored-penumbra revisit
// and geometric specular anti-aliasing  — are routed/applied
// per surface in MaterialInstanceCache::get_or_create, gated by
// set_transparent_shadows() / set_colored_penumbra() / set_specular_aa().
// Against a hermetic NOOP engine + TextureStore + MaterialInstanceCache (the
// NOOP engine produces no pixels, so — like smoke_glass_refraction.cpp /
// smoke_advanced_materials.cpp — "presence" is expressed as the material
// routing / per-instance state the visible effects depend on, the genuine
// pixel parity requiring separate full-scene captures), this case asserts:
// (a) with transparent shadows enabled a foliage caster (leaf/leaves/branch/fir
// predicate) routes to "heaven_foliage_shadow" — the coverage-modulated,
// foliage-tinted lower-density shadow vs the opaque silhouette
// — and a non-foliage surface is never routed (predicate
// gates it); disabled it keeps "heaven_lit_twosided" (≤1/255 parity);
// (b) with colored penumbra enabled a plain Standard_Lit single-sided surface
// routes to "heaven_lit_colored_penumbra" — the only material the committed
// HAS_COLORED_PENUMBRA define fix compiles the tint block into, built against
// the epsilon-clamped lib/filament shader whose divisor
// max(1.0 - diffuseColor, 1e-3) is strictly positive so near-white surfaces
// stay firefly-free; disabled it keeps "heaven_lit" (≤1/255
// parity); and (c) the specular-AA strength ladder
// (heaven::spec_aa_strength) is strictly increasing in level and bounded by the
// configured maximum (higher quality ⇒ stronger suppression — the
// host-checkable part of), those exact host values reach the lit
// MaterialInstance (read back via getSpecularAntiAliasingVariance/Threshold,
// and disabled the setters are never called so the instance keeps
// Filament's compiled default — untouched ⇒ ≤1/255 parity, proven
// non-vacuous against the differing enabled-High value. The base material name
// is read back via MaterialInstance::getMaterial()->getName(). Self-contained —
// does not use ctx.backend. Defined in smoke_shadows_lighting.cpp.
[[nodiscard]] CaseResult run_shadows_lighting_case(const CaseContext& ctx);

// Private NOOP checks cover tone-map/grading construction, exposure/DOF math,
// and post-process options. The live backend checks camera/depth FOV, live
// SSR/shadow controls, day/night lighting and sky visibility, wind-arrow removal,
// and SSGI history-camera alignment across disabled and resized captures.
// Pixel-level appearance still requires the separate actual-app capture audit.
// Defined in smoke_camera_grading.cpp.
[[nodiscard]] CaseResult run_camera_grading_case(const CaseContext& ctx);

// Private NOOP View checks cover adaptive-resolution range, upscaler selection,
// disabled state and quality bounds. Filament's actual PID receives synthetic
// GPU durations to check response to the finite adaptive target. NOOP cannot
// prove timer-query support or live scale movement; actual diagnostic captures
// report render_resolution_scale. Scored runs keep a fixed resolution.
// Defined in smoke_dynamic_resolution.cpp.
[[nodiscard]] CaseResult run_dynamic_resolution_case(const CaseContext& ctx);

// Soft-shadow runtime-toggle / config-path smoke case (advanced-rendering spec
// the soft directional-shadow feature
// is a View_Option_Group applied through LightingSystem::setup
// (configure_soft_shadows), not an IRenderFeature. Against a hermetic NOOP
// engine + view, this case drives LightingSystem::setup() with
// FeatureToggles::soft_shadows flipped off→on→off and asserts the View shadow
// configuration is re-applied by the next setup() pass each time: OFF ⇒
// the Shipping_Look basic ShadowType::PCF with Filament-default
// SoftShadowOptions, ON ⇒ contact-hardening ShadowType::PCSS with the
// penumbra scaled by the soft-shadow Quality_Knob sample count, all
// through the existing configuration path rather than inlined into render_frame
// It also drives an out-of-range soft-shadow level through the shared
// apply_quality_args / clamp_quality_level CLI helper and asserts it clamps to
// the nearest valid level (the clamp math itself is unit-tested in task
// Self-contained — does not use ctx.backend. Defined in
// smoke_soft_shadows_toggle.cpp.
[[nodiscard]] CaseResult run_soft_shadows_toggle_case(const CaseContext& ctx);

// Actual NOOP View checks requested bloom strength at unchanged lighting,
// default-curve bit parity and retention of threshold/dirt/quality options.
[[nodiscard]] CaseResult run_bloom_strength_case(const CaseContext& ctx);

// Contact-shadow presence / disabled-parity smoke case (advanced-rendering spec
// screen-space contact shadows are a
// View_Option_Group applied through LightingSystem::setup
// (configure_contact_shadows), not an IRenderFeature. Against a hermetic NOOP
// engine + view, this case drives LightingSystem::setup() with
// FeatureToggles::contact_shadows flipped off->on->off and reads the directional
// sun's LightManager::ShadowOptions back: OFF => screenSpaceContactShadows ==
// false (bit-identical Shipping_Look directional lighting), ON =>
// screenSpaceContactShadows == true with stepCount driven by the contact-shadow
// Quality_Knob (contact_shadow_steps), re-applied by the next setup()
// pass each time. The genuine benchmark-camera darkened-contact-line
// presence/absence pixel assertion  needs the full Heaven scene + camera
// path + accepted reference images and a separate comparison. This case
// exercises compare_rgba8 / within_shipping_look_tolerance on synthetic images;
// neither the smoke fixture nor production --benchmark performs that comparison.
// Self-contained -- does not use ctx.backend. Defined in
// smoke_contact_shadows.cpp.
[[nodiscard]] CaseResult run_contact_shadows_case(const CaseContext& ctx);
// Actual GPU pixels: TAA stability on a sloped receiver and a positive control
// proving that a nearby occluder still casts a screen-space contact shadow.
[[nodiscard]] CaseResult run_contact_shadow_stability_case(const CaseContext& ctx);

// Exact planar horizon under a 3-degree SUN and a separate contact-only blocker.
[[nodiscard]] CaseResult run_finite_sun_contact_case(const CaseContext& ctx);

// Actual displaced geometry must reject unresolved height detail while
// retaining low-frequency relief, independently of source texture resolution.
[[nodiscard]] CaseResult run_displacement_filter_case(const CaseContext& ctx);

// MSAA unsupported-count fallback / disabled-parity smoke case
// MSAA is a
// View_Option_Group applied through configure_post_processing (rhi/post_process.cpp),
// not an IRenderFeature. Using configure_msaa's supported-set argument as a
// forced-unavailable seam (modelled on smoke_wet_reflections_capability.cpp),
// this case asserts the init-time fallback applies the highest supported sample
// count <= the request (8⇒4, 4⇒2, none-at-or-below⇒1) and honours a supported
// request unchanged. Against a hermetic NOOP engine + view it then drives
// configure_post_processing with msaa_enabled=false and asserts the View's
// MultiSampleAntiAliasingOptions resolve to 1 sample / disabled (TAA-only
// Shipping_Look parity) with the TAA block left unchanged, plus an
// enabled-at-4-samples sanity pass proving the toggle drives the path.
// Self-contained — does not use ctx.backend. Defined in smoke_msaa_fallback.cpp.
[[nodiscard]] CaseResult run_msaa_fallback_case(const CaseContext& ctx);

// Point-light shadow disabled-parity / live-selection smoke case
// shadow-casting
// point/spot lights are applied through LightingSystem::configure_point_light_shadows,
// which uses the pure select_shadow_casting_lights helper (highest-intensity-first,
// budget-bounded) to flip LightManager::setShadowCaster on the backend-owned
// scene lights — it creates/destroys no lights. Against a hermetic NOOP engine,
// this case builds a small set of POINT lights at distinct intensities, wraps
// them in SceneLightInfo entries, and drives configure_point_light_shadows:
// DISABLED ⇒ zero lights report isShadowCaster (Shipping_Look-equivalent local
// lighting); ENABLED with budget < n ⇒ exactly `budget` lights cast and
// they are the highest-intensity ones, with the same entity count before
// and after (no lights created/destroyed). Self-contained — does not use
// ctx.backend. Defined in smoke_point_light_shadows.cpp.
[[nodiscard]] CaseResult run_point_light_shadows_case(const CaseContext& ctx);

// Volumetric-clouds registration / depth-pre-pass / disabled-parity smoke case
// bring
// the backend up (init_features runs inside init_pipelines) and assert via the
// backend's test-only seams that (1) render_features_ contains EXACTLY ONE
// feature named "volumetric-clouds" — debug_count_features == 1 with
// debug_find_feature non-null; (2) the feature is disabled by default and
// adds NONE of its renderables to the scene while off — enabling it via
// debug_set_feature_enabled raises the scene renderable count by exactly its one
// fullscreen quad, proving Shipping_Look-equivalent sky + ground lighting when
// disabled; and (3) once enabled+built its needs_depth_prepass() is
// honored — after a few active frames debug_depth_texture() is non-null (the
// shared half-res depth pass ran because the cloud feature requested it and bound
// FrameContext::depth_texture outside a render pass —). Degrades to
// the asset-independent registration contract when the embedded cloud material is
// unavailable on the backend (needs_depth_prepass()==false ⇒ no quad), and
// restores the feature to its disabled default for subsequent cases. Defined in
// smoke_volumetric_clouds.cpp.
[[nodiscard]] CaseResult run_volumetric_clouds_case(const CaseContext& ctx);

// SSGI registration / depth-pre-pass / effect / disabled-parity smoke case

// bring the backend up (init_features runs inside init_pipelines) and
// assert via the backend's test-only seams that (1) render_features_ contains
// EXACTLY ONE feature named "ssgi" — debug_count_features == 1 with
// debug_find_feature non-null; (2) the feature is disabled by default and
// adds NONE of its renderables to the scene while off — enabling it via
// debug_set_feature_enabled raises the scene renderable count by exactly its one
// fullscreen quad, proving Shipping_Look-equivalent indirect lighting when
// disabled; (3) once enabled+built its needs_depth_prepass() is honored —
// after a few active frames debug_depth_texture() is non-null (the shared
// half-res depth pass ran because the SSGI feature requested it and bound
// FrameContext::depth_texture outside a render pass —); and (4) the
// live Shipping_Look diff helper (compare_rgba8 / within_shipping_look_tolerance)
// detects an SSGI colour-bleed hue shift as a >1/255 difference while disabled
// output stays within tolerance  — the genuine benchmark-camera
// capture toward an adjacent saturated surface requires a separate full-scene
// capture and comparison. Degrades to the asset-independent registration +
// diff-helper contract when the embedded SSGI material is unavailable
// (needs_depth_prepass()==false ⇒ no quad), and restores the feature to its
// disabled default for subsequent cases. Defined in smoke_ssgi.cpp.
[[nodiscard]] CaseResult run_ssgi_case(const CaseContext& ctx);

// Massive-foliage build-once / build-failure-fallback / disabled-parity smoke
// case (plus
// registration cardinality): the MassiveFoliageFeature  is a
// build-once geometry feature modeled on GrassFeature. HALF A asserts against
// the live backend that render_features_ contains EXACTLY ONE feature named
// "massive-foliage" (debug_count_features == 1), that it is DISABLED by
// default, and that across several empty-scene frames
// debug_build_counters().foliage_generations stays 0 — the disabled feature
// builds no foliage, the observable precondition for Shipping_Look-equivalent
// grass + ground detail. Because the feature's build is DEFERRED to the
// first frame with non-empty instances/meshes and the 64x64 harness renders an
// empty scene, HALF B exercises the build-once + build-failure behaviours
// HERMETICALLY against a stand-alone NOOP engine + TextureStore (modeled on
// smoke_wet_reflections_capability.cpp / smoke_glass_refraction.cpp): a
// synthetic non-empty FrameContext drives the deferred build so generation_count()
// reaches 1 on the first active frame and never grows on subsequent frames
// and a ForcedBuildFailure subclass overriding the protected
// can_build_foliage seam (the WetReflectionsFeature::probe_capability pattern)
// proves the feature falls back to the Shipping_Look grass baseline and
// continues (building once), with a default-seam control proving the
// success path. Full-scene pixel parity needs separate accepted-reference
// captures; production build-once behavior needs an actual scene run.
// Defined in smoke_massive_foliage.cpp.
[[nodiscard]] CaseResult run_massive_foliage_case(const CaseContext& ctx);

// Render real frames and check their wall-clock deltas with measure_perf_window.
// The two-second warm-up and minimum 60-second window define measurement
// coverage; the short smoke run remains incomplete. A synthetic full-length
// series verifies mean FPS and frame accounting. Actual performance comparisons
// use the production camera path with matching hardware and settings.
// Defined in smoke_fps_bar.cpp.
[[nodiscard]] CaseResult run_fps_bar_case(const CaseContext& ctx);

// Shipping_Look pixel-parity smoke case (Requirement
// render Shipping_Look-equivalent frames on the live backend (a stack
// WeatherState at intensity 0, the disabled-features state in which rain renders
// nothing and wet-reflections keeps the shipping SSR + wetness global) and
// verify the pure pixel-diff helper (heaven::compare_rgba8 /
// within_shipping_look_tolerance, tolerance kShippingLookMaxChannelDelta = 1) is
// live end-to-end in the renderer-linked binary. The genuine sampled-camera-path
// comparison needs the pre-recorded Shipping_Look reference set + the full Heaven
// scene + the authored camera path, none of which exist in this 64x64 bare
// harness, so the case SKIPs. Separate capture/reference comparison is needed;
// production --benchmark does not perform it. Defined in smoke_shipping_look_parity.cpp.
[[nodiscard]] CaseResult run_shipping_look_parity_case(const CaseContext& ctx);

// Verify explicit all-disabled reference settings, default capability settings,
// and ten per-capability configurations. Render reference frames on the live
// backend and exercise the RGBA comparison helper. Full camera-path pixel
// parity requires recorded reference images and scene assets; validation-layer
// acceptance requires a separate configured run. The case reports those absent
// checks as SKIP after verifying the available assertions.
// Defined in smoke_phase8_parity.cpp.
[[nodiscard]] CaseResult run_phase8_parity_case(const CaseContext& ctx);

// Material-instance-cache key-distinctness + exactly-once-teardown smoke case
// the cache
// keys MaterialInstances on the composite (mesh_path, surface_name) pair so the
// same surface name recurring across different meshes resolves to DISTINCT
// instances, a repeated pair returns the SAME cached instance
// dedup), and teardown destroys each instance exactly once. Like
// smoke_glass_refraction.cpp this stands up a hermetic NOOP Filament engine +
// TextureStore + MaterialInstanceCache (the headless RapidCheck target links no
// Filament, so it cannot build a real MaterialInstance), then asserts: two pairs
// sharing a surface name but differing in mesh_path get different instance
// pointers; two fully-distinct pairs get different pointers; the same pair asked
// twice returns the identical pointer (the map deduped it under one key, so the
// destructor visits it once); and a positive control confirms instances are
// created at all. The exactly-once destruction  is observed as the live
// NOOP cache destructor running cleanly over the deduped map (a double-destroy
// would fault the real engine); the destroy-count proof over a fake engine is
// owned by the host test. Self-contained — does not use ctx.backend.
// Defined in smoke_material_cache.cpp.
[[nodiscard]] CaseResult run_material_cache_case(const CaseContext& ctx);

// Mesh-store vertex re-upload count-validation smoke case (codebase-hardening
// MeshStore::upload records each
// surface's vertex count, and update_vertices rejects a re-upload whose count
// does not match (returning an Error and touching no buffer) so a mismatched
// CPU-skinning push cannot corrupt the interleaved vertex buffer. The headless
// RapidCheck target links no Filament, so the VertexBuffer/IndexBuffer this
// exercises must be built against a real engine; this case stands up a hermetic
// NOOP Filament engine + MeshStore (like smoke_glass_refraction.cpp), uploads a
// small real mesh (one surface, a handful of vertices + a triangle), and asserts
// against that REAL uploaded buffer that update_vertices: (a) accepts a
// matching-count re-upload (Ok — the valid CPU-skinning path); (b)
// rejects a too-small AND a too-large count with an Error and no buffer write
// and (c) treats an unknown handle as a benign no-op (Ok). The
// (orig,new) accept-iff-equal decision over the full integer space is owned by
// the host test. Self-contained — does not use ctx.backend. Defined
// in smoke_mesh_store.cpp.
[[nodiscard]] CaseResult run_mesh_store_case(const CaseContext& ctx);

// Startup sampling of actual uploaded height textures, including transformed
// UVs, mip filtering, 64-bit query IDs, partial chunks and invalid inputs.
[[nodiscard]] CaseResult run_height_sample_batch_case(const CaseContext& ctx);

// Weather re-application discipline smoke case (codebase-hardening spec task
// 23.2 /): the weather-dependent Filament state
// (lighting/exposure + distance fog + post-process) is applied once on the
// first frame (seeding prev_weather_) and re-applied by render_frame ONLY when
// the per-frame WeatherState differs from the last applied one — an unchanged
// WeatherState triggers NO re-application. Mirroring smoke_perframe_discipline /
// smoke_rain_build_once, this drives the live backend and reads the
// debug_build_counters().weather_reapplies counter in DELTAS around its own
// frames (the harness shares one backend, so the counter is cumulative):
// rendering several frames at an UNCHANGED WeatherState advances it by 0;
// rendering a frame at a CHANGED WeatherState (rain intensity 1.0) advances it by
// exactly 1 — proving the per-frame change path is reachable after the baseline
// seed  — and holding that changed state for several more frames advances
// it no further (steady state re-applies nothing). Restores the default
// WeatherState before returning so the shared backend is left as found. SKIPs
// (exit 0) if init_pipelines fails (headless/driver-less). Defined in
// smoke_weather_reapply.cpp.
[[nodiscard]] CaseResult run_weather_reapply_case(const CaseContext& ctx);

// MusicPlayer lifecycle + failure-path smoke case (background-music-playback

// MusicPlayer links SDL2 + the vendored stb_vorbis decoder and is
// compiled into the renderer / renderer-linked smoke targets ONLY (never the
// headless heaven_benchmark_tests host target), so its lifecycle and
// diagnostic-emitting failure paths are exercised HERE rather than in
// benchmark/tests/. None of these scenarios open an audio device or need a
// GPU/display — they all leave playing_ false — so the case constructs
// MusicPlayer instances directly against std::filesystem temp fixtures and
// does NOT use ctx.backend; it runs only under the shared HEAVEN_SMOKE=1 +
// display gate, which satisfies "gated like the existing smoke harness". It
// asserts: (a) a disabled MusicConfig is a pure no-op — start() returns and a
// following stop() is a clean no-op, with volume() reflecting the configured
// level and no device opened/track played; (b) stop() is idempotent and
// safe before any start, after a disabled no-op start, and after a real
// enabled start against a missing file, called repeatedly; (c) the
// missing-file vs unreadable-file diagnostics name the resolved
// Music_Asset_Path and distinguish "not found" from "not readable"
// — stderr is captured via a POSIX dup/dup2 redirect so the actual
// diagnostic substrings are asserted; (d) a non-Vorbis file yields the
// "decode failure" diagnostic naming the path; and (e) the
// Packer_Mode guard  — a normally-constructed player is on a
// Render_Run, so the missing-file path reaches the "not found" diagnostic and
// NOT the "packer mode" rejection, proving on_render_run() returned true; the
// rejection branch is structurally unreachable because the packer never
// constructs a MusicPlayer (asserted behaviorally + documented, no test-only
// public seam is added to force render_run_ false). Self-contained — does not
// use ctx.backend. Defined in smoke_music_player.cpp.
[[nodiscard]] CaseResult run_music_player_case(const CaseContext& ctx);

// Real-decode + playback-start smoke case (background-music-playback spec task
// 10.1 /): closes the loop against the REAL Heaven
// track. It resolves heaven/heaven.oga under the configured Data_Root
// (ctx.config.data_dir, default "extracted/heaven") via PathResolver and, when
// that file exists, (a) decodes it directly through the same stb_vorbis path
// the MusicPlayer uses and asserts a non-empty interleaved buffer with exactly
// 2 channels at 44 100 Hz — the stereo 44.1 kHz PCM contract; and (b)
// constructs a MusicPlayer, start()s it against the real PathResolver, and —
// when an audio device is available — polls the read-only playing() observer
// until it becomes true within the start budget (~1 s of run start per,
// inside the 500 ms-from-init-complete bound of), always stop()ing the
// player before returning. The decode assertion  needs only the file; the
// playback-start assertion  needs a working Audio_Device, so it is
// gated on device availability (SDL_GetNumAudioDevices) and SKIPped (never
// failed) when no device is present or the device cannot be opened in this
// environment (headless audio). SKIPs entirely when the real asset is not
// resolvable from the smoke run's CWD (the genuine decode needs the real file).
// Defined in smoke_music_decode_start.cpp.
[[nodiscard]] CaseResult run_music_decode_start_case(const CaseContext& ctx);

// Device-failure + run-parity smoke case (background-music-playback spec task
// 10.2 /): closes the audio-failure loop that only a
// real (failing) device open can exercise. It forces an unavailable audio
// device by tearing down SDL's current audio driver (SDL_AudioQuit) and
// re-initialising it with a bogus driver name (SDL_AudioInit), which fails and
// leaves NO audio driver current — so the MusicPlayer, started against the REAL
// decodable heaven/heaven.oga (it must decode successfully to REACH the device-
// open step), runs all the way to SDL_OpenAudioDevice, which then fails. The
// case captures stderr via a POSIX dup/dup2 redirect around start()+stop() and
// asserts: (a) the "failed to open audio device" diagnostic is emitted carrying
// a NON-EMPTY SDL-reported reason; (b) playing() stays false after the
// failed open (the open-failure path never sets playing_, and the 5 s timeout
// path of  shares this exact failure-handling contract — playing_ false +
// diagnostic — which is asserted here since SDL cannot be made to hang for 5 s
// deterministically; the limitation is documented in the case); and (c) the
// player-level run-parity contract of  holds — start() returned without
// blocking, the music thread joined cleanly via stop(), and the process did not
// abort, leaving the same externally-observable terminal state as a successful
// audio start (full render-run exit-status parity needs a separate application
// run). The original audio driver is restored (SDL_AudioQuit + SDL_AudioInit)
// before returning so subsequent cases are unaffected. SKIPs entirely when the
// real asset is not resolvable (the genuine decode-then-open cannot be faked).
// Defined in smoke_music_device_failure.cpp.
[[nodiscard]] CaseResult run_music_device_failure_case(const CaseContext& ctx);

// Music-on vs music-off frame-sequence + FPS-parity smoke case

// the genuine "identical frames + median FPS within 1 %" comparison is defined
// over the FULL Heaven scene + the authored camera path + ~60 s of real-time
// playback, and requires separate controlled captures and timing runs. The
// production --benchmark does not compare those results. The smoke harness brings up
// ONE 64x64 backend with an EMPTY scene and uses it directly (not via
// Application), so it cannot drive the authored camera path nor honestly produce
// a 1 % median-FPS bar on a noisy 64x64 empty-scene run — and faking that pass
// is forbidden. Following smoke_fps_bar.cpp / smoke_shipping_look_parity.cpp,
// this gated case does the honest, host-runnable core instead: it renders N
// empty frames with NO player (music-off) and again with a MusicPlayer
// constructed + start()'d ALONGSIDE (music-on, against a PathResolver from
// ctx.config.data_dir — the real heaven/heaven.oga when resolvable, else a
// no-op on the missing file), then asserts via debug_build_counters() that
// (a) both passes rendered the same frame COUNT; (b) the per-frame
// transform_updates delta is uniformly EXACTLY 1 in BOTH passes — no render-loop
// iteration stalled, skipped, or was blocked on music coordination, proving the
// render loop never calls into the player; and (c) the per-frame
// transform-delta SEQUENCE is byte-for-byte identical between the two passes —
// the empty-scene proxy for "ordering and count of rendered frames unchanged"
// For  it measures both passes' median FPS on the live backend
// and asserts only a GENEROUS sanity tolerance (music-on median stays >= 50 % of
// music-off — music does not tank FPS), documenting that the strict <=1 % bar
// over the authored camera path needs separate controlled runs (a 64x64
// empty-scene median is too noisy for 1 %). The player is always stop()'d before returning.
// Defined in smoke_music_frame_parity.cpp.
[[nodiscard]] CaseResult run_music_frame_parity_case(const CaseContext& ctx);

// Actual lens material: camera/light translation preserves appearance and distance fade.
[[nodiscard]] CaseResult run_lens_flare_case(const CaseContext& ctx);

// Actual NOOP textures, explicit roots and production material borrower lifetime.
[[nodiscard]] CaseResult run_render_test_assets_case(const CaseContext& ctx);

// Shared backend: real procedural scene uploads, feature borrowers and reload cleanup.
[[nodiscard]] CaseResult run_render_test_reload_case(const CaseContext& ctx);

} // namespace heaven::smoke

#endif // HEAVENBENCH_SMOKE_SMOKE_CASES_H_
