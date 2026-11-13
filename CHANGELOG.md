# Changelog

## 1.2.0 — 2026-11-13

Update the renderer to public Google Filament 1.80.1 and intentionally regenerate all 49 production material packages for version 80. Preserve the documented HDR, dual-window-system, shadow, profiler, VRS and experimental frame-generation integrations through the new variant, queue, pipeline and ownership APIs. Correct the new incremental scene cache to retain positive constant homogeneous W and weight camera-origin recentering consistently, without changing authored UVs, displacement bounds, ordinary W=1 arithmetic or culling guards. Migrate the sky upload's local pointer to the supported mutable Texture API; a final profiler-query drain is confined to its smoke fixture.

Add optional Off/2×1/2×2 Vulkan variable-rate shading with actual logical-device/MSAA eligibility and explicit fallbacks. Eligible opaque lit main-color work can share fragment shading; masked/blended materials, depth/shadow/motion/SSR work, post-processing and UI remain full rate. Off remains the default. Coarse shading trades detail for possible performance; the retained pre-rebase Intel Heaven measurements did not show a useful whole-scene gain.

Extend opt-in GPU diagnostics with actual pipeline invocation counts, driver compiler statistics, bounded shader text and pass-to-pipeline links where supported. Keep compiler estimates, submitted workloads, timestamps and unavailable/lost coverage distinct. Add identity-checked read-only Linux xe clock observations. Diagnostic requests remain unscored even when unavailable or later stopped. The optional standalone FP16/FP32 and signed INT8/INT32 GPU lab provides reference, packed-dot and cooperative-matrix checks; it is disabled in ordinary builds and is separate from Heaven scores.

Fix verified Intel BMG G31 / Mesa 26.0.8 startup resets and stale color uploads through a narrowly selected, support-checked image policy. Harden Vulkan staging pitch/layout/flush behavior, readback visibility and subresource restoration, command failure handling and submitted-resource lifetimes. Clamp the irradiance-only IBL's zero reflection-mip count before unsigned subtraction. Preserve rendering formats, resolution, lighting and feature settings without global driver overrides or ordinary-frame GPU waits.

Use the pinned persistent Android signing identity for trusted main-branch CI packages, with a separate source/run/attempt-bound receipt and no persistent credentials in Docker or Gradle. Moving from published 1.1.1 requires one final backup/uninstall/reinstall because that APK's old key was not retained; later official updates retain the application ID/key and increase versionCode. APKs remain debuggable development software, with device acceptance separate from signing.

The benchmark version is **1.2.0** and Android versionCode is **6**. Extraction tools remain **0.3.1**, result schema remains **2**, and score version remains **3**. Original Heaven assets and optional generated 2K/4K derivatives remain external and retain their owners' license restrictions. Compare benchmark results with matching application/score versions, hardware, driver, API, workload and effective settings; generation and diagnostic requests follow the existing unscored policy. [Release readiness](docs/RELEASE_READINESS.md) separates source/native checks from exact-version CI, package and publication acceptance.

Local 1.2.0 builds pass 50 FSR-plus-GPU-lab host cases and 43 SDK-disabled host cases. The final 442-renderer/two-extraction static gate passes with only four exact fingerprinted baseline diagnostics; current Intel Linux native suites pass 56 Vulkan cases with six skips and 55 OpenGL cases with seven skips, plus focused GPU-diagnostics, VRS and FSR lifecycle controls. The 360-case Debug utility run retains its separate pre-version-bump source identity. Selected image comparisons retain their per-binary scope and incomplete roof/wall loaded-count log coverage; they do not establish complete visual parity.

A matched full-Heaven Intel Vulkan/X11 view measures 327.928→329.834 ms mean accepted frame time (+0.581%) across the complete native windows and 325.886→334.546 ms (+2.657%) in separately declared final tails. The comparator is a preserved Filament 1.77.2 development executable stamped 1.1.1, not the published 1.1.1 release. Quality settings and High contact shadows are unchanged. Two repeats per treatment, baseline drift and accepted read-only desktop Git activity limit attribution; this is neither a general speedup nor proof of a universal causal regression. Fresh exact-source CI, all eight package/source audits, downloaded-package runtime checks and publication remain pending; [the current qualification record](docs/RELEASE_READINESS.md#november-13-filament-1801-and-120-candidate) retains the detailed scopes.

Register-pressure optimization remains open. All rejected November 12 shader rewrites stay excluded; smaller compiler output alone did not improve their measured workload, and this entry makes no general scene-FPS gain claim.

## 1.1.1 — 2026-11-05

Enforce concrete Linux Vulkan GPU requests through Mesa device isolation and actual device/PCI identity checks. Preserve default/environment preferences and the OpenGL fallback with explicit verification limits. Distinguish original source-surface ordinals in neutral tessellation cache keys, advance ZIP semantics to 37 with conservative v34 loose import, and verify logical EOF/CRC before accepting cache or asset-pack manifest bytes.

Remove invisible rain geometry and rain-only depth demand at effective zero while preserving dry-down, wetness, temporal membership and persistent buffers. Add bounded 64 KiB binary-loader read-ahead with checked logical seeks and unchanged short-read/corruption handling. Add focused host/native negative regressions, sanitizer evidence and [the dated code review/remediation record](docs/CODE_REVIEW_2026-11-04.md). Existing rendering/platform acceptance limits remain scoped in that record.

Correct Intel OpenGL height-sampling teardown crashes by handing retiring current GLX pbuffers to the existing dummy drawable and clearing the matching cached swap-chain pointer. Retain the change in the reproducible Filament patch, add actual-source lifecycle regressions and verify complete OpenGL suites on Intel and AMD, including an isolated native negative control. Material packages remain unchanged.

Refresh retained depth-proxy transforms, culling and instanced buffers once when shared depth resumes after an inactive interval. Preserve idle-pass removal and steady-state dirty updates; add native singleton/instanced depth-field regressions with failing refresh-disabled controls. Final Intel/AMD OpenGL and AMD Vulkan suites each pass 54 cases with five disclosed skips.

The benchmark version is **1.1.1** and Android versionCode is **5**. Extraction tools remain **0.3.1**, result schema remains **2**, and score version remains **3**. Fresh release CI, independent eight-package/source/checksum audits and publication pass at source `7930b9e`; [release readiness](docs/RELEASE_READINESS.md#november-5-published-111) records the exact downloadable bytes and bounded package-runtime evidence. Compare benchmark results using matching application and score versions, workloads and effective settings.

## 1.1.0 — 2026-10-16

Add experimental FSR 3.1 frame generation for explicit Linux and Windows Vulkan SDK builds, plus an optional Windows NVIDIA Streamline DLSS adapter. Both SDK build options and the saved frame-generation mode default off. Settings and `--frame-generation off|fsr3|dlss` show requested/effective modes and explicit unavailability reasons. Both providers require SDR and fixed rendering resolution; any tour requesting generation is diagnostic and unscored, including unavailable requests or a later switch to Off.

Supply accepted-source depth and motion for opaque/masked geometry, animated instances, displacement, GPU bones, CPU deformation, grass, sky and representative transparent effects. Preserve coherent bidirectional heat-haze distortion and compose the current premultiplied UI separately from interpolated scene color. History advances only after accepted sources and resets across cuts, resize, scene reload, long gaps and native presentation changes. Harden queued process-pause/system-sleep recovery, SDK resource reuse, failed-presentation fallback and recreation without adding ordinary-frame GPU waits.

Correct Filament skinned SSR variant selection, fence-aware Vulkan pipeline eviction, material-owned moment-program retirement, asynchronous froxel output lifetime and padded Vulkan linear-image staging. Keep these changes in the reproducible vendor patch with checked source-tree pins. Add host contracts, native rendering oracles and maintained full-scene motion controls covering the affected paths.

Add bounded generated/real image diagnostics, native submission/failure counters and opt-in KHR present-completion tracing with explicit unavailable coverage. Add transient `HEAVEN_INPUT_RESPONSE_TRACE` JSON export correlating consumed F6/camera input to accepted real sources, render-call return and host-observed GPU completion. Collection adds no ordinary-frame wait, preserves unresolved/lost observations and makes tours diagnostic even with generation Off. These software bounds and native submissions do not measure physical latency, display cadence or confirmed displayed FPS.

Extend checksum-pinned SDK preparation, license collection, portable source manifests, static-analysis coverage and host-test discovery. Add separate experimental Linux AppImage and Windows SDK-enabled package pipelines with build/shader receipts and relocation verification. Windows native preflight retains active assertions. Streamline preparation extracts only selected public headers and the framework notice from the checksum-pinned SDK archive. It does not extract, load or bundle proprietary runtime DLLs, and packages do not include them.

Bounded Linux/RADV evidence includes native input/UI/lifecycle checks, short consecutive generated-image review, repeated performance controls, actual system sleep/recovery and ten-minute validated full-scene endurance. Source `5316360` passed all five CI workflows and ordinary/SDK Linux and Windows package audits; the later input-response diagnostic has separate local receipts. General qualitative user acceptance was recorded on October 16: “It appears to work properly.” Detailed per-mode protocol execution, physical input-to-photon latency, exact display cadence, unrestricted image quality and native Windows GPU execution are not established. Proprietary NVIDIA runtime execution remains untested; both integrations remain experimental and off by default. See [frame-generation evidence and limits](docs/FRAME_GENERATION.md) and [the acceptance record](docs/FRAME_GENERATION_ACCEPTANCE.md).

The benchmark version is **1.1.0** and Android versionCode is **4**. Extraction tools remain **0.3.1**, result schema remains **2**, and score version remains **3**. Original Heaven assets remain external. Ordinary scored comparisons require generation Off, input-response tracing disabled, and matching application/score versions, workloads and effective settings.

## 1.0.1 — 2026-10-12

Correct legacy diffuse-color conversion and tune daylight lighting to reduce the scene's washed-out appearance. Preserve the original encoded-domain detail blend before gamma-2 RGB conversion, lower broad daytime ambient fill, retain more sky color below the tone mapper's white shoulder, and reduce clear-day fog while preserving its night medium. Explicit sRGB colors, alpha handling, direct sunlight and independent night sources keep their existing policies; corrected material colors intentionally affect both day and night.

Add eleven maintained real-scene lighting views covering daylight, rain, twilight, night and optional 4K textures. A host contract executes the shared material-color helper with independent numeric oracles and negative controls. Reports and capture metadata distinguish legacy color from sRGB and linear data, including separate cache identities and accurate texture totals.

Increase enclosed street-lamp illumination to restore readable warm pools on nearby walls, benches and paving after legacy color correction. Refine synthetic lamps toward the original's golden hue and raise the initial corrected source power by another 50%, guided by matched native original Heaven night views. Preserve authored light colors, the daytime-off schedule, unrelated light sources, lamp-glass emission and existing light ranges; add real-scene night/day/4K controls and native selective-power/color, twilight-repeat and restoration checks.

Add soft golden local lamp volumes within the existing volumetric-fog pass, guided by the original benchmark's analytic halo. Clip the bounded glow against scene depth and follow live lamp motion, night scheduling and power-off independently of optical-axis flares. Preserve the medium's extinction and current physical lighting; add native controls for occlusion, camera-inside behavior, off-axis lamps, source changes, depth rebinding and cleanup.

Run the material-color contract in the Linux, Windows and macOS host-test pipelines. Release tags reuse the tested source commit without starting duplicate platform build matrices; branch, pull-request and manual builds remain available.

The benchmark version is **1.0.1** and Android versionCode is **3**. Extraction tools remain **0.3.1**, result schema remains **2**, and score version remains **3**. Lighting changes alter the GPU workload even with the same preset; compare results using matching application and score versions, workloads and effective settings.

## 1.0.0 macOS package — 2026-10-12

Publish a native Apple Silicon (M1+) macOS app on the existing 1.0.0 release, requiring macOS 15+. Add Vulkan/MoltenVK window support, LLVM 21 build helpers and app packaging with bundled runtime libraries, notices, checksums and relocation checks. The package is ad hoc signed without Developer ID signing or notarization; full rendering and interactive acceptance on a physical Mac remain unverified.

All four platform workflows pass for macOS source commit `6e959ae59338258743b8226776d8a2b265fe20bc`. Upgrade workflow actions to Node.js 24 and correct portable Linux analysis and Windows packaging regressions exposed during the macOS port. The macOS source archive and provenance accompany the added package; the original 1.0.0 tag and earlier Linux, Windows and Android assets retain their original identities.

## 1.0.0 — 2026-10-10

HeavenBench 1.0.0 establishes a fresh source baseline containing the benchmark, cross-platform build and packaging pipelines, asset preparation tools, rendering fixtures and microprofiler. This version does not by itself publish a release or qualify every platform; remaining runtime, visual and signing gates are recorded in [release readiness](docs/RELEASE_READINESS.md). Original Heaven assets remain external.

The benchmark version is **1.0.0** and Android versionCode is **2**. Extraction tools remain **0.3.1**, result schema remains **2**, and score version remains **3**. Compare benchmark results using matching workloads, effective settings and score versions.

## Daily development history

The summaries below preserve the development record before the history reset. They cover **173 distinct commits across 45 active days**, fetched from the original development and private release-candidate branches, using author dates in Europe/Prague. Stash and agent checkpoint commits are excluded. October 9 summarizes the subsequently documented working-tree integration, and October 10 records this source baseline; these two entries are not counted among the historical commits. Historical changes describe work at the time, including implementations later replaced or refined.

- **2026-10-10:** Set the benchmark version to 1.0.0, retain this daily changelog, exclude reproducible asset packs and local build/cache/report/tooling state, and consolidate the reviewed source tree into one initial commit. Retire the completed root plan, organize user/build/development documentation, and add original Heaven, extraction-tool and HeavenBench credits in the docs and app toolbar. Preserve the prior history and working source in a local recovery backup. Expand the feature overview with optional 4K textures and a benchmark screenshot, and rename the repository to HeavenBench.
- **2026-10-09:** Integrate asynchronous loading and tessellation caches, bounded displacement seam fixes and geometry levels, performance labels, spatial instancing/grass/shadow culling and CPU optimizations. Add nested CPU/worker/GPU microprofiling with live controls and offline reports; refine moonlight/stars, lamp lighting, lit smoke, flare visibility and emissive reflective lamp glass, with focused regression evidence.
- **2026-10-08:** Add matched real-mesh airship AO regressions; correct GTAO bent-normal integration, material AO policy and receiver masking in screen-space specular occlusion.
- **2026-10-07:** Keep Android benchmark playback controls accessible and add an authored airship rendering fixture.
- **2026-10-05:** Prepare bounded displaced geometry, rendering fixes and isolated feature scenes; verify CPU dependency notices and initialize the private release-candidate repository. Repair native Windows flags, fixture paths, resources and CI preflights; correct Android surface sizing and scaled input, and reduce the optional preview workload.
- **2026-10-03:** Reproduce residual roof contact-shadow artifacts, trace their relationship to displaced geometry, and filter vertex displacement to the mesh's actual sampling footprint.
- **2026-10-01:** Refine contact-shadow self-intersection, grazing and steep-surface behavior; add temporal GPU checks and frame profiling for flicker diagnosis.
- **2026-09-30:** Audit and harden project code, static analysis, runtime portability and source hygiene. Add Linux AppImage, Windows ZIP and Android APK packaging, shared Docker/CI pipelines, external asset preparation and offline tools; rebase Filament to 1.77.2 and record HDR, visual and package validation limits.
- **2026-08-10:** Package optional high-resolution textures as an extension archive and add the renderer diagnostics sidebar.
- **2026-08-01:** Add generated village-ground relief, improve its shading, and introduce the 4K texture preparation pipeline.
- **2026-07-31:** Add Zen 4/Zen 5 builds and result build identity; introduce ZIP-backed assets and tessellation storage, bundle the loading screen, and restore ZIP grass assets with authored flower variation.
- **2026-07-30:** Introduce rendering presets, texture semantic handling, output calibration and scaled SDR sizing fixes. Add reproducible CI and dependency pins, organize settings by performance impact, unify build directories, restore scene-lit grass, centralize tuning and modernize project C++ to C++23.
- **2026-07-29:** Correct tessellation camera space and mouse look, expose geometry diagnostics, update dirty-instance smoke tests, and rebuild materials for Filament 1.74.
- **2026-07-28:** Optimize frame work and culling, revise benchmark scoring and GPU selection, add automatic result reports, repair bridges, and reproduce authored depth of field, detail mapping and tessellation behavior.
- **2026-07-27:** Improve sky and volumetric clouds and bundle the renderer's sky assets.
- **2026-07-08:** Rename the benchmark to HeavenBench and update compiled renderer materials.
- **2026-07-07:** Add default configuration loading, startup diagnostics and a loading screen; improve screen-space effects, material variants and build documentation.
- **2026-06-20:** Implement GPU instancing and skinning with focused tests; add double-sided lit materials and thin-surface depth proxies.
- **2026-06-19:** Add live settings editing with configuration save/load.
- **2026-06-18:** Expand advanced materials, shadows, color grading and dynamic resolution; add Ogg Vorbis background music playback.
- **2026-06-17:** Add grass depth proxies for procedural vegetation occlusion.
- **2026-06-16:** Introduce shared linear-depth rendering through proxy renderables.
- **2026-06-15:** Add recorder-based used-asset packing and strengthen loader/configuration validation and robustness.
- **2026-06-13:** Centralize asset path resolution and add JSON configuration support with CLI-equivalent validation.
- **2026-06-11:** Add HDR paper-white control, authored displacement UV transforms and boundary handling, a typed CLI parser, and system/settings result metadata. Freeze floating-island animation during scored tours and honor the cloud toggle in IBL generation.
- **2026-06-10:** Improve atmospheric and grass lighting, add minimum/present FPS tracking, implement SSGI color capture and bilateral depth filtering, and introduce HDR10 output with the colored-penumbra define fix.
- **2026-06-09:** Expand rendering quality controls, refine volumetric clouds and depth culling, unify soft-shadow tuning, and add CPU frame-rate tracking and an FPS overlay.
- **2026-06-08:** Unify rain and splashes on a world-anchored lattice; add wind and per-drop variation, tune wet surfaces and grass, and refine splash ripples.
- **2026-06-07:** Improve world-anchored rain and wet-surface reflections.
- **2026-06-05:** Add Bezier camera-path interpolation and weather with rain and wet reflections.
- **2026-06-04:** Extract backend infrastructure into subsystems and complete benchmark scoring, overlay integration and the initial smoke-test suite.
- **2026-06-03:** Add firefly/flame/glow effects, dual X11/Wayland support and camera collision. Refine LOD/grass handling and split rendering effects, sky IBL generation and utilities into maintainable modules.
- **2026-06-02:** Optimize material lookups, load authored animation parameters, repair cloth mass/constraints/wind, and consolidate rendering on Filament by removing the legacy OpenGL backend.
- **2026-06-01:** Classify metallic materials by inheritance and add heat-haze distance fading, culling and a CLI toggle.
- **2026-05-29:** Add PBD cloth with tracked anchors and screen-space heat-haze refraction.
- **2026-05-28:** Add per-material UV transforms and tessellation of skinned meshes.
- **2026-05-27:** Correct reversed-Z handling in god rays and volumetric fog; improve texture variation and lighting in grass and lit materials.
- **2026-05-25:** Add a procedural sky and starfield, improve night lighting, fix clustered-light artifacts and detail-map parsing, load authored dynamic lights, and implement per-light volumetric scattering.
- **2026-05-24:** Add GPU smoke particles, refine emission tint/strength and smoke shading, and repair camera controls.
- **2026-05-21:** Refine foliage and SSAO, add build helpers, remove the legacy Vulkan backend, and improve tessellation height filtering and boundary detection.
- **2026-05-10:** Optimize grass animation and rendering.
- **2026-05-08:** Refine material blending and specular maps.
- **2026-05-04:** Add CPU tessellation for displacement mapping.
- **2026-05-03:** Integrate Filament and its material system.
- **2026-04-30:** Iterate grass blade geometry, lighting, wind and visual variety; add a compute-generation reference implementation.
- **2026-04-29:** Add parallax occlusion mapping, SSAO, atmospheric/post-processing effects, smoke/fire particles and skeletal animation. Implement rope/bridge simulation with Jolt and improve grass geometry and lighting.
- **2026-04-28:** Initialize archive extraction and mesh-conversion tools and the first benchmark renderer with a multi-backend RHI.
