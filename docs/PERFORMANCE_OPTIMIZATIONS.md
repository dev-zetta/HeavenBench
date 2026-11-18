# HeavenBench Performance Optimizations

This document describes the performance work implemented after the analytic-sky and bundled-sky-assets changes. The work targets per-frame CPU overhead, repeated GPU state uploads, duplicated geometry, physics cost, expensive screen-space effects, and repeatable profiling.

Correctness takes priority over raw throughput. The frustum-culling work exposed a bounding-box construction bug that initially caused visible objects to disappear under small camera rotations. Optimization 4 documents the root cause and the corrected culling implementation.

## November 4 review remediation

[Rain workload gating](CODE_REVIEW_2026-11-04.md#final-rain-workload-and-image-controls) removes ordinary and temporal precipitation membership at effective intensity zero and resolves depth demand before target arbitration. The isolated corrected native fixture submits 1,017 main-pass triangles/five commands with no rain-only depth pass, matching rain disabled on Vulkan and OpenGL; the initial implementation submitted 120,002 additional main-pass triangles plus a 1,017-triangle depth pass. Positive rain keeps its original workload, dry-down follows effective intensity, independent wetness continues, and buffers remain persistent. Zero/off pixels match exactly; positive captures retain a bounded two-RGB8-level noise-phase difference caused by corrected first-wet depth scheduling. Short draw-count traces do not establish an FPS gain. The CR-06 continuation refreshes retained depth-proxy transforms, culling and instanced bounds once after an inactive interval, so a past one-off dirty event cannot leave stale depth on reactivation. Idle frames retain their skipped synchronization and pass; steady active frames keep ordinary dirty-index updates.

The production binary reader now owns a lazy 64 KiB heap read-ahead buffer, drains cached bytes before direct large-array reads, and retains checked logical seek/tell and EOF/CRC error propagation. The initial scratch `fort.mesh` trial reduced mean load time from 24.41 to 10.41 ms; that historical prototype result is separate from the production acceptance below. Host, malformed-input, corruption and scoped ASan/UBSan regressions cover the integrated reader. Complete-scene startup and rendering FPS effects remain unmeasured.

The [final production-reader acceptance](CODE_REVIEW_2026-11-04.md#production-zip-reader-performance-acceptance) measures 26.101/10.811/26.179 ms per warm ZIP mesh load in baseline/candidate/baseline order on fixed CPU 15, each with at least two seconds of warmup and 60 measured seconds. The integrated reader takes 58.58–58.71% less time with 0.30% baseline drift and identical complete geometry hashes. Task build/test/GPU work is stopped during the measurement. This qualifies the one mesh-loader workload, not whole-scene startup or rendering FPS.

Concrete Vulkan GPU selectors now validate actual identity before scene loading; default/environment behavior and heuristic policies remain explicit. Controlled comparisons must continue recording the backend's actual GPU. Earlier measurements below retain their own recorded hardware/source identities.

## 1. Frame telemetry and benchmark accounting

The RHI now returns a `FrameResult` from `render_frame()`. It contains:

- whether Filament accepted the frame and reached `endFrame()`;
- CPU time for camera setup;
- CPU time for scene and light synchronization;
- CPU time for render-feature updates;
- CPU time spent submitting render views;
- Filament's most recent completed whole-frame GPU duration, when available.

The GPU value comes from Filament's public frame-history API. It is deliberately
reported as a whole-frame duration rather than a per-pass measurement because
the public renderer API does not expose per-pass GPU timestamps.

Benchmark statistics now count only frames accepted by Filament. Elapsed time
from rejected `beginFrame()` attempts is accumulated and attached to the next
accepted frame, preventing rejected frames from inflating the reported FPS.
The benchmark completes only after the terminal camera-path frame is actually
accepted.

At the end of a run, HeavenBench prints average CPU stage costs and the available
whole-frame GPU average. Setting `HEAVEN_BENCHMARK_EXIT=1` makes the application
exit after one completed benchmark run, which supports automated profiling.

Primary files:

- `benchmark/src/rhi/rhi.h`
- `benchmark/src/rhi/filament_backend.cpp`
- `benchmark/src/benchmark/benchmark_system.cpp`
- `benchmark/src/main.cpp`

## 2. Dirty-instance transform propagation

Previously, the renderer uploaded transforms for every persistent scene entity
and rebuilt every instanced transform array each frame, even when only floating
islands, rocks, or bridge pieces had moved.

The main loop now constructs a sorted, unique list of dirty instance indices.
Floating-island and floating-rock transforms are marked dirty only when their
matrix actually changes. Bridge instances append their indices only after a
physics step produces new transforms.

The dirty span is passed through the RHI to:

- `SceneBuilder`;
- `DepthProxyBuilder`;
- `SceneLightSpawner`;
- render-feature frame context.

Singleton transform-manager writes, emissive-light position updates, and depth
proxy writes are skipped for clean instances. Authored floating-light updates
are also skipped when the floating-offset vector is unchanged.

Primary files:

- `benchmark/src/main.cpp`
- `benchmark/src/rhi/scene_builder.cpp`
- `benchmark/src/rhi/depth_proxy_builder.cpp`
- `benchmark/src/rhi/scene_light_spawner.cpp`

## 3. Persistent instancing transform staging

Each visible and depth-only instanced chunk now owns a persistent
`local_transforms` staging vector. Per-frame synchronization changes only entries
whose instance indices are dirty.

An `InstanceBuffer::setLocalTransforms()` upload occurs only when at least one
member of that chunk changed. This removes a vector allocation, a full transform
rebuild, and a GPU buffer update from clean chunks.

Primary files:

- `benchmark/src/rhi/scene_builder.h`
- `benchmark/src/rhi/scene_builder.cpp`
- `benchmark/src/rhi/depth_proxy_builder.h`
- `benchmark/src/rhi/depth_proxy_builder.cpp`

## 4. Correct frustum-culling bounds

Frustum culling is enabled for eligible singleton scene renderables, GPU-instanced chunks, depth proxies and generated grass patches. Singleton bounds come from actual uploaded vertex positions. Instanced chunks group nearby placements and refresh their aggregate world AABB when members move; non-displaced groups use the singleton local margin, while ordinary displaced groups retain the existing 16-metre world allowance. See [the October 9 audit](CULLING_AUDIT.md) for granularity, envelope limits and measured results.

The initial implementation passed `{minimum, maximum}` directly to
`filament::Box`. That type is not a min/max pair: its aggregate fields are
`{center, halfExtent}`. As a result, every scene culling volume was shifted to
the minimum corner and assigned the maximum corner as its half-size. Grass
volumes intended to cover `[-200, 200]` were therefore centered at `-200`, and
ordinary object bounds were malformed in the same way. Filament correctly
culled against those incorrect boxes, causing rotation-dependent pop-in.

All enabled-culling paths now construct boxes with
`box_from_min_max()`, which calls `filament::Box::set(min, max)` to derive the
correct center and half-extent. This covers:

- persistent scene renderables;
- instanced scene chunks;
- scene depth proxies;
- instanced depth-proxy chunks;
- generated grass and massive-foliage patches;
- grass depth proxies.

Dormant renderables with culling disabled (fullscreen effects, loading/HDR
composites, particles, rain, sky, lens flares, and debug arrows) use the same
explicit conversion. Although their bounds do not currently participate in
culling, removing the misleading `{min, max}` aggregate syntax prevents a
future culling toggle from silently reviving the bug.

`prop_filament_bounds.cpp` is a host-side RapidCheck regression test against the
real `filament::Box` type. For randomized boxes it verifies the recovered
minimum, maximum, center, and half-extent over at least 100 generated cases.

Large scene meshes and GPU-skinned meshes remain excluded from culling through
the existing safety policy. Explicit grass distance LOD also remains active and
is synchronized with the grass depth proxies.

Primary files:

- `benchmark/src/rhi/filament_bounds.h`
- `benchmark/tests/prop_filament_bounds.cpp`
- `benchmark/src/rhi/scene_builder.cpp`
- `benchmark/src/rhi/depth_proxy_builder.cpp`
- `benchmark/src/rhi/features/grass_feature.cpp`
- `benchmark/src/rhi/features/massive_foliage_feature.cpp`
- `benchmark/src/rhi/grass_depth_proxy.cpp`

## 5. Grass population deduplication and distance LOD

Massive foliage already contains the baseline grass population. Running it
together with the legacy grass feature rendered two complete meadow populations.
When massive foliage is enabled, the regular grass feature now starts disabled.
Runtime toggles keep the two alternatives mutually exclusive.

Both grass systems retain the 650-metre island-origin distance LOD. Within each island/category, spatial index ranges have individual wind-expanded bounds, letting camera and depth passes exclude distant blades without reducing the population. Each visible range and its matching depth proxy are toggled together. Geometry remains persistent and buffers are shared across ranges; distance LOD changes scene membership rather than regenerating blades.

Primary files:

- `benchmark/src/rhi/filament_backend.cpp`
- `benchmark/src/rhi/features/grass_feature.cpp`
- `benchmark/src/rhi/features/massive_foliage_feature.cpp`
- `benchmark/src/rhi/grass_depth_proxy.cpp`

## 6. Fixed-rate bridge physics

Bridge simulation now uses a fixed 120 Hz step with an accumulator and at most
four catch-up steps per rendered frame. This makes physics cost bounded during a
slow frame and avoids making simulation behavior directly dependent on render
FPS.

The bridge endpoints use separate invisible kinematic bodies at the authored
`start_bridge*` / `end_bridge*` dummy positions. Every visible plank remains
dynamic, including the first and last links. Previously those visible endpoint
planks were themselves kinematic: when a floating island moved vertically, the
last plank retained its authored rotation while the neighboring dynamic plank
was pulled toward it, producing an intermittent hard kink at the final joint.
Two lateral point constraints attach each dynamic endpoint plank to its authored
anchor, matching the internal two-point joints while allowing a continuous
rotation through the span.

On render frames where the accumulator has not reached the next fixed step, the
renderer skips bridge transform readback entirely. When physics does step, only
bridge instances are appended to the dirty-instance list.

Primary files:

- `benchmark/src/engine/rope_sim.h`
- `benchmark/src/engine/rope_sim.cpp`
- `benchmark/src/main.cpp`
- `benchmark/tests/unit_bridge_endpoint_dynamics.cpp`

## 7. Explicit dynamic resolution

Dynamic resolution is opt-in for interactive rendering. Measured benchmark runs disable it so the configured output resolution describes a fixed pixel workload; expensive clouds, fog, and SSGI do not silently enable a reduced render scale. TAA and the shaders' temporal sample phases remain independent of this policy. Earlier documentation describing automatic adaptive scaling was stale.

Primary file: `benchmark/src/rhi/filament_backend.cpp`.

## 8. Shadow-work policy

The current public Filament API does not expose a static-shadow cache or the per-pass controls needed to implement one cleanly. The existing application shadow policy uses public controls; the microprofiler's vendor instrumentation observes their resulting pass work.

The retained supported path minimizes shadow work through:

- applying shadow-caster selection only when its enabled state, budget, or a
  fingerprint of candidate entity IDs and intensities changes;
- limiting the shadow-casting point/spot-light budget;
- using a 256-pixel point-light shadow map;
- limiting point-light shadow falloff to 12 metres;
- preserving the command-line option to disable point-light shadows for
  profiling.

Light positions are intentionally excluded from the policy fingerprint:
floating lights can move without reissuing `setShadowCaster` or
`setShadowOptions`. Filament still updates the selected light's shadow map, but
the application no longer invalidates the shadow-caster policy/atlas merely
because the per-frame configuration entry point ran.

A true static shadow-map cache remains future work and would require an upstream Filament API or a documented vendor implementation.

Primary file:

- `benchmark/src/rhi/lighting_system.cpp`

## 9. Foliage geometry ownership and wind-aware bounds

Massive foliage now uploads each source mesh/category once and shares those immutable vertex/index buffers across authored island placements. The visible renderables and depth proxies keep their independent transforms and visibility. Source seeds, generated vertex bytes, blade and flower counts, materials, wind animation, and draw placement are preserved. Teardown destroys depth proxies and visible renderables before their shared buffer owners. The CPU engine library also compiles the shared loaders once for the renderer, packer, and tests instead of compiling those sources separately into each executable.

Both grass systems replace the fixed local `[-200,200]` cube with bounds from their actual generated vertices. The expansion covers the shader's complete wind amplitude, transformed back into local coordinates for rotation, scale, reflection, and shear. Invalid or singular transforms disable culling. Floating-island translation leaves the local bounds unchanged; changes to the linear transform update visible and depth bounds together. This removes off-screen work without changing the visible population. `HEAVEN_PROFILE_BOUNDS=1` logs the computed bounds once per patch.

The host fixtures check wind extrema, transforms, degenerate inputs, exact generated vertex parity, and translation caching. A separate renderer-linked NOOP fixture checks six buffer owners shared by twelve grass/flower placements, twelve depth proxies with no extra geometry allocations, independent transforms, and repeated resource teardown. Real GPU timings and remaining visual acceptance are recorded in the release audit.

## Repeatable profiling

Run the feature-ablation suite using the saved configuration at 1920×1080, or choose one diagnostic variant and pass explicit renderer/run options:

```bash
./scripts/profile.sh --x11
./scripts/profile.sh no_grass --config config.json --x11
./scripts/profile.sh half_resolution --config config.json --x11
./scripts/profile.sh all_features --config config.json --width 1920 --height 1080 --x11
```

Each invocation freezes the effective base configuration and writes commands, environment metadata, logs, terminal status, JSON/HTML reports, and compact timing summaries into a fresh `build/profiles/run-*` directory. `HEAVEN_PROFILE_DIR` selects the parent directory; `HEAVEN_PROFILE_TIMEOUT` bounds each complete camera-path run (600 seconds by default). `HEAVEN_BIN` can select a frozen packaged binary. The default variants are the unchanged base, no foliage, no volumetric clouds, no volumetric fog, no SSGI, no point-light shadows, selected screen-space depth effects disabled, and no GPU instancing. `half_resolution`, `no_tessellation`, `no_contact_shadows`, `no_ambient_occlusion`, `no_msaa` and `sun_shadow_2048` are additional individually selectable diagnostics. The screen-space-effects variant leaves AO, SSR and contact shadows enabled, and remaining consumers can still request a depth pass. Quality level zero means Low; use the feature's disable flag for an off control.

Feature removal and resolution changes isolate workloads; their faster results are not improvements at unchanged quality. Use the same API, GPU, driver, assets, effective settings, and camera path for implementation comparisons, repeat the baseline, and keep compilation and other controlled GPU workloads outside the measurement interval. On Linux, `--x11` can avoid a Wayland compositor's FIFO presentation; confirm the actual logged present mode. GPU timing is a completed whole-frame sample, not an isolated per-pass timestamp. Render submission includes driver waiting and is not a pure CPU-work measurement.

The performance window excludes its first two seconds and requires at least sixty measured seconds. FPS is informational: the former universal 100/3,500 FPS thresholds did not account for hardware, resolution, or most feature settings and have been removed. Score version 3 and result schema 2 are unchanged. Regression decisions need a calibrated comparison at a recorded workload. Headless tests can validate accounting and implementation invariants, but cannot establish rendering throughput or visual acceptance.

## Settings performance guidance

Settings shows an estimated impact beneath each rendering feature and its quality controls. Minimal, Low, Medium, High and Very high describe the potential enabled workload; CPU, GPU, VRAM, memory and loading labels distinguish where that work occurs. Hover an estimate for its conditions and reason. These are qualitative guidance, not measured percentages or FPS predictions, and effects can share passes or change cost with visible coverage, resolution, quality and hardware. Disabled features retain their guidance so the cost is visible before enabling them. The two CLI-only features, color grading and dynamic resolution, have a read-only guide in Graphics.

GPU instancing and GPU skinning usually save CPU work; they are labelled accordingly. Texture tiers primarily increase memory, upload time and bandwidth, while physical exposure and HDR brightness are inexpensive parameters. Tessellation factor controls topology; displacement scale and distance do not provide proportional triangle-count reductions. The diagnostics now label unique stored mesh counts explicitly: repeated instances, generated foliage and depth/shadow passes add work beyond those numbers.

## October 9 fixed-view diagnosis

Thirteen native runs used the saved configuration, the same Vulkan/Wayland API, Radeon AI PRO R9700, RADV 26.0.8-1ubuntu0.3, 1920×1080 output, render scale 1, 4K textures and 4× MSAA. Each run excluded its first two seconds and measured at least 62.88 seconds at camera pose `-9,11,28,113,-8`, FOV 50. The screenshot's 22,323,082 stored mesh triangles are reproduced exactly: 623,503 base triangles become 19,355,485 neutral tessellated triangles plus 2,967,597 prepared triangles. Those counts exclude the 1,873,691 instantiated grass blades and 3,747 flower cards. The prepared addition alone represents 26,708,373 triangles across all sharing placements before culling or repeated passes.

| Diagnostic workload | FPS | Frame time | Time saved against first baseline |
|---|---:|---:|---:|
| Saved configuration | 6.53 | 153.17 ms | — |
| Generated foliage off | 12.09 | 82.73 ms | 70.44 ms |
| Tessellation/displacement off | 11.51 | 86.91 ms | 66.26 ms |
| Point/spot-light shadows off | 10.39 | 96.26 ms | 56.91 ms |
| Contact shadows off | 7.32 | 136.58 ms | 16.58 ms |
| MSAA off | 6.97 | 143.55 ms | 9.62 ms |
| Volumetric clouds off | 6.62 | 151.08 ms | 2.09 ms |
| Ambient occlusion off | 6.57 | 152.19 ms | 0.98 ms |
| Volumetric fog off | 6.57 | 152.27 ms | 0.90 ms |
| Ground relief off | 6.55 | 152.57 ms | 0.60 ms |
| SSGI off | 6.52 | 153.33 ms | −0.16 ms |
| Foliage and tessellation off | 66.05 | 15.14 ms | 138.03 ms |
| Repeated saved configuration | 6.54 | 153.02 ms | 0.15 ms |

The baseline is GPU-bound: valid delayed whole-frame GPU samples average 150.68 ms against approximately 2.37 ms of CPU render submission. Foliage, tessellation and local shadow rendering are the largest measured costs in this view. The repeated baseline differs by only 0.10%, while SSGI and the small atmosphere/AO/ground-relief deltas do not establish meaningful general performance gains. The individual savings overlap; removing tessellation also changes shadow and depth workloads. These diagnostic feature removals change visual quality and leave the saved configuration unchanged.

This is one fixed camera with a live animated world, rather than the user's exact screenshot camera or an authored tour. The 100 ms simulation-delta clamp means fast and slow runs can sample different motion phases. GPU samples omit unavailable values and the complete warmup prefix, but remain delayed whole-frame observations rather than per-pass costs. The original 4K archives retain known missing-texture fallback events; these measurements do not establish texture-complete visual acceptance. Wayland logged FIFO presentation consistently. No compilation or other controlled GPU workload ran during the measurement series.

Evidence is under `build/performance-impact/20261009/`: every case retains its command, effective configuration, raw samples, log and terminal status. `heaven_benchmark.profile` preserves the exact measured ELF (`fd179172f2dc94100bd5cd01c1589a0a85fadedd4ec239ff54e98acd7f4dffa0`); the later UI-only rebuild is separate. `independent-review/completion.json` binds 234 successful checks, input hashes, actual feature activation, geometry/foliage count parity and eight verifier negative controls. Use the reviewer's `summary.md` and `final-thirteen-verification.json` for stage means: the initial root harness includes one extra warmup row and averages unavailable `-1` GPU samples, while its FPS and measurement windows are correct.

## Microprofiler pass diagnosis

The October 9 microprofiler captures actual hardware timestamps and submitted draw workloads on Vulkan and OpenGL. In the recorded full scene, shadow-pass interval unions dominate at approximately 83 ms per retained native frame, followed by opaque color at approximately 23 ms and reflections at approximately 7 ms. The mesh-only and camera-facing-away traces independently reduce shadow work. These are observed unions with partial-history and unavailable-result controls, not additive CPU/GPU frame costs. [MICROPROFILER.md](MICROPROFILER.md) records commands, effective settings, exact executable identities, recorder loss and overhead measurements.

The trace also exposes a source-backed explanation for grass shadow cost: the existing upstream PCSS/VSM preparation policy promotes shadow receivers to casters. Both grass paths receive shadows, so their public `castShadows(false)` flag does not prevent this prepared work. A full Shadow Pass submits 13,130,825 more triangles than the mesh-only control, exactly matching the instantiated massive-foliage vertex count divided by three. The later cascade checkpoint below proves this policy with a tiny caster/receiver fixture and preserves it while removing out-of-cascade work. Shadow caster LOD and shadow bounds/budgets remain possible later optimizations. Off-camera visibility alone is insufficient for removing casters.

Repeated Apply Weather / color-grading tasks exposed redundant CPU work, corrected in the checkpoint below. The 32³ LUT's 32 slice jobs execute inline on the main thread at this size. Dense rejected-attempt loops overwrite older CPU events in the bounded recorder, so these captures do not certify exclusive CPU percentages. Inline grass, parallax and material effects remain part of their owning passes; the profiler does not assign invented separate shader costs to them.

## October 9 CPU weather and submission optimization

On Linux visual-ultra at 1920×1080, the saved full-scene trace measures approximately 128 ms between presentations and 126 ms of hardware GPU pass interval union. High main-thread activity did not establish CPU-limited throughput: changing surface wetness repeatedly rebuilt an unchanged color-grading LUT, including on rejected submissions. The retained baseline weather applications average 6.87 ms. Filament builds the tone-mapping LUT even when optional color grading is disabled.

`ColorGradingCache` now reuses the backend-owned LUT when its exact active exposure, HDR/SDR, mapper and grading inputs match. Fog, bloom, AO, reflections, dynamic resolution and camera-owned DOF continue updating; inactive SDR controls do not invalidate HDR, and inactive grading controls do not invalidate disabled grading. Ownership changes and backend initialization reset the non-owning cache. The isolated 64-call CPU fixture measures approximately 7 ms with one LUT build versus 441–444 ms with 64 forced builds; those are CPU configuration timings, not GPU passes or FPS predictions.

The cache alone revealed another problem: rejected attempts became cheap enough to flood the command stream. Recorder-off B-A-B controls measure 7.820 → 7.252 → 7.808 FPS, and the recorded candidate contains a 125.5 ms command-buffer-space wait and 41.7 million overwritten events. A fixed one-millisecond retry reduces CPU activity but still loses throughput. Both negative candidates remain saved. The retained implementation services input-derived controls and exports first, then waits only after a rejected submission. Its separate owner-thread retry helper retains the last finite positive asynchronous GPU duration, requests one sixteenth of it capped at eight milliseconds, and falls back to one millisecond before a usable sample. Missing samples remain unavailable in reports. Resize, reload, settings changes and lifecycle/benchmark transitions reset the estimate. Accepted submissions remain uncapped; scheduler rounding and oversleep remain platform behavior.

The frozen Linux candidate is `67a209792136840a01499b2ef26cd0e534d12888c74206c540f225ae3f8181aa`. Its repeated recorder-off runs reach 7.846–7.848 FPS and hidden recording reaches 7.843 FPS at unchanged quality, compared with the 7.808–7.820 FPS recorder-off controls. Each excludes the two-second warmup and retains more than 62.9 measured seconds. In matched approximately 50-second recorder-off observations, main-thread CPU falls from approximately one full core to 5.0–6.5% of one core, and whole-process CPU falls from approximately 1.17 cores to 0.21–0.26 cores. Averaging normalized CPU rates across each pair gives 94.2% less main-thread CPU and 79.9% less process CPU. These are Linux `/proc` CPU-tick observations; they are separate from wall-clock task spans and GPU timing. Hidden recording adds 0.047% mean presentation interval relative to the two off controls, meeting the 5% target in this workload. The small FPS differences do not establish a general speedup; visible-panel overhead is not remeasured for this CPU checkpoint.

The retained hidden trace measures a 126.019 ms GPU pass interval union over 291 qualified native frames, with unchanged selected pass workload counters. Its 532 retained weather applications average 2.88 microseconds and contain no LUT rebuild. Of 500 retained retry spans, 499 join rejected frames, none joins an accepted frame, and one falls just before retained frame history. Its 2,742,065 overwritten CPU events and zero drops still prevent complete-window exclusive CPU percentages. The remaining dominant FPS cost is GPU shadow work, approximately 83 ms per native frame; source-backed caster-policy and LOD work above remain the next measured optimization targets.

Final fixed-camera captures use the real authored airship, AO on, explicit rain intensity 0.5 and deterministic 60 Hz simulation steps after 45 warmup frames. The final effective rain setting is independently checked because fixture defaults override config-file weather unless explicitly overridden by CLI. Vulkan candidate images are byte-identical to the first baseline. OpenGL differs by at most two RGB8 levels and is byte-identical to its repeated baseline. Retained weather changes do not rebuild the candidate LUT. Both final native rasters are inspected, and all 14 startup/runtime JSON and embedded HTML pairs agree. This is a bounded fixture comparison, not complete scene appearance acceptance.

The canonical build, seven host CTest checks and focused Clang 21 gate pass; the headless GPU CTest is an explicit skip. Complete native Vulkan and OpenGL smoke suites each pass 46 checks with four documented skips, including exact LUT invalidation, resource ownership, View refresh and camera DOF checks. The adaptive-delay host fixture covers missing/delayed arrivals, nonfinite/nonpositive samples, reset, sub-millisecond scaling and the cap. Full-scene commands, effective settings, profile rows, CPU ticks, binary/source freezes, negative candidates and independent source reviews are retained under `build/cpu-bottleneck/20261009/`. This checkpoint changes project-owned CPU code only; the existing Filament patch and all material sources/packages are retained. Android and Windows are not rebuilt by this Linux optimization checkpoint.

## October 9 conservative directional cascade culling

The next unchanged-quality Linux candidate reaches **8.280 FPS versus 7.843 and 7.842 FPS** in the two controls: **5.57% more FPS** and 5.28% less mean presentation interval. Each hidden-recording run uses the saved 1920×1080 visual-ultra configuration, 4K textures, 4× MSAA, PCSS, 4096-pixel Sun maps and the fixed ledge camera on the same R9700/RADV Vulkan/Wayland path. Each excludes the first two seconds and measures more than 62.98 seconds. Baseline-repeat drift is −0.012%; no compilation or other controlled native GPU work overlaps these windows. This is one animated camera/hardware workload, not a general speedup or a new profiler-overhead qualification.

Filament previously sent the global directional-caster range to every Sun cascade. `ShadowMapManager.cpp` now independently refines its directional visibility bit against each map's actual raster-camera frustum before command generation, preserving the original global range for eligibility and UBO preparation. Six conservative AABB-plane tests use double intermediates and a scale-relative 64-FLOAT-epsilon reserve; invalid inputs and explicit `culling(false)` remain visible. Depth-clamped passes retain the original global mask because their rasterization can extend past near/far planes. The directional bit is restored afterward. No mesh, material, texture, displacement, shadow quality, receiver policy or punctual-light workload is reduced.

Source/history review confirms that receiver promotion is deliberate VSM behavior, introduced upstream in 2021; current public PCSS uses EVSSM moments. Removing that promotion would change overlapping-card self-shadowing. The new twelve-cell fixture observes actual PCF/VSM/PCSS pass commands, primitives and instance counters for flat/overlapping production-lit and production-grass receivers. Explicit-caster, nonreceiver, self-shadow and external off-camera blocker controls pass on both APIs. An isolated VSM/PCSS warmup initializes Filament's engine-owned Gaussian material before the strict eight-resource teardown baseline. The PCF external-blocker control uses a conservative off-camera bound with an analytic receiver-light-depth support point to accommodate upstream caster-only far fitting; it does not claim to fix that separate fitting behavior.

The cascade fixture exercises PCF/VSM/PCSS with one and four cascades. Its four moment-map counts fall from `[504,504,504,504]` to `[96,196,336,504]`, while `.culling(false)` reference pixels remain byte-identical on Vulkan and OpenGL. The far cascade may legitimately retain the entire near-to-far caster population. Independent off-camera blockers still darken 784 receiver pixels. Complete native suites each pass 47 cases, skip four documented cases and fail zero. Seven host CTest suites and the focused Clang 21 fixture gate pass, with the opt-in GPU entry separately skipped. The original vendor policy fails the new cascade count oracle while its receiver oracle passes; restoring the retained source reproduces the final renderer and smoke executable byte-exactly. Two initial fixture constant-width warnings are corrected, with failed analysis logs preserved.

Across 291 qualified retained native-frame groups per run, observed shadow GPU interval union falls **83.07→76.38 ms** (8.1%). The first four shadow invocations fall 47.14→40.41 ms; the remaining twelve remain approximately 35.95 ms. Baselines submit 65,305,016 triangles to each Sun cascade; the candidate submits approximately 43.71M, 54.72–54.76M, 65.15M and 65.31M. Source/configuration review identifies the first four as directional maps in this no-atlas path; labels alone do not encode light identity. GPU unions omit unavailable and oldest boundary groups, and CPU ring overwrites remain disclosed. They are bounded observations, not complete-window exclusive CPU costs or additive CPU/GPU totals. Historical archive hashes come from the preceding post-series receipt; no independent pre/post asset hash exists for this three-run cohort.

Real full-scene ledge and close morning-roof captures on both APIs retain their original settings and 45 deterministic warmup steps. Roof differences stay within two RGB8 levels, with a signed distribution matching Filament's temporal RGB dithering. Ledge differences are similarly dominated by that noise with a few small highlight outliers. Unchanged-baseline repeats also show sparse highlight instability, but do not explain every candidate outlier: one round-window highlight differs consistently by up to 64 RGB8 levels over a handful of pixels. That residual remains unexplained; byte-exact full-scene parity is not established. No coherent lost-shadow boundary is observed, and separate no-culling fixture controls are exact. The native candidate rasters are visually inspected. These views retain known 4K texture-fallback limitations and do not establish complete appearance acceptance.

The rebuilt ARM64 development APK also exercises a bounded five-second tablet shadow fixture with basic shadows and then soft shadows at 512-pixel maps. Both runtime/startup JSON and embedded HTML pairs agree. The soft-shadow trace includes actual `Cull directional shadow cascade` CPU scopes and three visible shadow maps plus their EVSM mip passes. Maximum sampled CPU temperature is 53.2°C with thermal status 0. Saved low settings are restored byte-exactly and rendering is stopped. The first-runtime raster shows the fixture and ordinary controls; the later screenshot shows the launcher after SDL's timed main returns. These tests qualify short fixture execution, not full-Heaven FPS/endurance, Windows runtime or release readiness.

The frozen Linux candidate is `a6a215f6af85b399a8e9b70dee1e5d9838d81123e19e8655b20e606c371cd173`; APK `71dbc6dcc4edf8778ccc19f46a70f125f17d06a2c45ec1491313d5d6cbcab363`. The distributed Filament patch is `9c8a335526ef129ad41c3bbea11a4c7b99427d5d28539e18c2136383f88baa38`, resulting tree `9650b6bd65498b746291c6e56f6a897f578b93b4`, verified in two fresh indices without changing the actual vendor index. All 34 material packages retain their bytes. Commands, frozen binaries, failed fixture controls, source reviews, GPU traces, image comparisons and device receipts remain under `build/shadow-culling/20261009/`. The remaining dominant costs are approximately 40 ms of directional shadows, 36 ms of local shadows and 23 ms of opaque work in this view. A next optimization should measure shadow LOD or local-map bounds/budgets with receiver-preservation controls before retaining a change.

## November 10 Intel full-scene diagnosis

The Intel BMG G31 (`8086:e223`) workload uses Mesa `26.0.8-1ubuntu0.3`, Vulkan/Wayland, the [Intel compatibility fix](INTEL_VULKAN_COMPATIBILITY.md), the fixed full-Heaven village noon view, actual 1920×1080, Visual Ultra, enhanced 4K textures and 4× MSAA. VRS, frame generation, HDR and dynamic resolution remain off. The frozen renderer is `4aff61b11df20e51c35704917368a7f224d8d8df57e87be9bde6db8b173d62c3`.

The 12-second microprofiler capture has 22 retained native-frame groups with all expected GPU categories after excluding the oldest boundary group. Mean observed interval unions are **262.33 ms opaque color**, 59.56 ms shadows, 20.32 ms SSR, 15.70 ms structure depth and 15.46 ms auxiliary depth. Each opaque invocation submits 54,523,679 primitives in 1,717 commands. Rejected-frame retry waits occupy 96.00% of retained complete same-thread Main-loop intervals, while accepted CPU submission intervals average 2.65 ms. This indicates GPU backpressure. The 138,206 overwritten CPU events prevent a complete CPU utilization or exclusive-cost claim. GPU categories remain separate; these retained intervals are neither complete frame times nor additive with CPU time.

A quarter-pixel diagnostic at actual 960×539 reduces opaque time to 116.36 ms with identical opaque work counts. More directly, turning only contact shadows off at 1080p reduces the opaque interval from 262.33 to **25.72 ms**, while opaque, structure, auxiliary depth and SSR submissions remain matched. Other GPU passes vary by roughly 5–8% across the recordings. The 236.61 ms difference is the observed toggle effect rather than an exclusive shader timer; the full quality setting can evaluate 16 Sun directions with up to 32 depth steps per direction. These two quality changes are mechanism probes and are excluded from the equal-quality implementation comparison.

None of the tested full-quality implementation changes earned retention. Compact foliage buffers remove 611 MB of upload without changing the expanded triangle stream, but their one 2.964 FPS run is within the earlier Off variation. The one-footprint cache, paired-ray cache and nearest-depth prefilter increase observed opaque time to 282.05, 276.92 and 305.12 ms respectively. Shared ray setup alone looks promising in one instrumented trace at 241.37 ms and passes both native suites (Vulkan 56 passed/five skipped, OpenGL 55 passed/six skipped). Its matched fixed-time image has RGB8 mean absolute difference 0.47335, close to the historical unchanged-repeat value 0.47333. The clean profile cohort instead measures **2.9083 FPS baseline versus 2.6415 FPS shared setup** (−9.17%), with both candidate runs slower than both bracketing baselines. The reason for the instrumented ranking reversal remains unisolated. Retain raw traces for diagnosis and use clean frame intervals for performance decisions.

All experimental renderer changes are restored. The original source, compiled materials and Filament patch/tree pins retain their bytes; the canonical helper reproduces the original renderer and native smoke binaries byte for byte. No default or saved user setting changes. The [November 11 conservative depth-tile trial](#november-11-intel-conservative-depth-tile-trial) tests the next proposed approach; it also fails the clean performance gate. The per-step cache and prefilter trials provide no reason to keep those shortcuts.

### Tested opt-in setting

**Quality → Contact-shadow quality → Low** keeps contact shadows enabled and uses eight depth steps instead of 32. With every other effective setting matched, one clean 61.87-second Low window measures 4.7036 FPS, versus 2.9083 FPS pooled across two High repeats. FPS increases **61.73%**, and mean interval falls 38.17%. This is a measured quality trade-off at one camera, not a shader speedup, benchmark score or general FPS prediction. Both modes use the restored executable, the same geometry, textures and MSAA, and the same Intel policy without driver debug flags.

| Contact-shadow quality | Clean runs | Pooled FPS | Mean frame time | p95 frame time |
| --- | ---: | ---: | ---: | ---: |
| High (32 steps) | 2 | 2.9083 | 343.84 ms | 364.44 ms |
| Low (8 steps) | 1 | 4.7036 | 212.60 ms | 219.40 ms |

The paired 90-frame fixed-time images retain the same geometry, textures, camera and effective settings apart from contact quality. The Low image changes local roof and surface contact shading; RGB8 mean absolute difference is 0.71913, pixel-maximum p99 is nine and 21,504 of 2,073,600 pixels differ by more than eight levels. This is one still-view quality comparison. The normal Visual Ultra preset retains High. The opt-in file `build/intel-performance/20261110/intel-1080p-contact-low.config.json` holds the tested settings with the ordinary writable cache location; pass it to `scripts/run.sh --config` or use the setting above.

The ignored `build/intel-performance/20261110/` workspace retains the initial trace's JSON/HTML, both mechanism probes, failed and passing build/native receipts, source identities, reproducible commands and clean raw intervals. See `optimization-ledger.md`, `analysis/baseline-01/diagnosis.md`, `analysis/contact-setup-profile-comparison/comparison.md` and `final-restoration.json`. Timing runs use the existing ≥2 s warmup and ≥60 s measured gates, private warm geometry and initially empty private shader caches. One-second competing-work checks and kernel intervals pass; short unclassified desktop work can escape the process checks. Native Windows, macOS, Android and other cameras remain separate scopes.

## November 11 Intel conservative depth-tile trial

**Both conservative tile implementations regress High contact-shadow performance and are rejected.** The actual full-Heaven village uses the same Intel BMG G31 (`8086:e223`, PCI `0000:03:00.0`), Mesa `26.0.8-1ubuntu0.3`, kernel `7.2.6-070206-generic`, Vulkan/native Wayland, fixed noon camera, 1920×1080, Visual Ultra, enhanced 4K assets and 4× MSAA as the earlier work. Contact shadows remain High with 16 finite-Sun directions and up to 32 original depth steps. VRS, frame generation, HDR and dynamic resolution stay off. Geometry, material textures, features and effective configuration are matched; the existing exact-driver Intel image compatibility fix remains active without driver debug overrides.

The first candidate constructs a separate RG32F min/max depth image, one bound per 8×8 native structure tile. It leaves the AO/SSR structure image and its original mip chain intact. A bounds test covers eight original steps, including clamped texture edges, and rejects a segment only when its nearest-depth interval cannot satisfy the existing dual-depth hit predicate. Uncertain cases execute the original fine samples. The second candidate stores overlapping 8×8 windows every four texels, replacing up to four conditional bounds fetches with one. Both preserve the original repeated `t += dt`, first hit, geometric t=0 contact and terminal edge fade. Neither reduces the High sample budget.

Clean profiling is separate from microprofiling. Each row excludes more than two warmup seconds and retains at least sixty measured seconds, using private warm neutral/prepared geometry and initially empty private shader caches. Independent analysis reconstructs the statistics from raw float32 frame intervals and checks binary/output identities, effective settings, hardware, assets and controller receipts.

| Trial, in execution order | Measured seconds | Frames | FPS | Mean frame time | p95 frame time |
| --- | ---: | ---: | ---: | ---: | ---: |
| Original High, opening reference | 61.8922 | 182 | 2.94059 | 340.067 ms | 346.304 ms |
| Separate 8×8 depth tiles | 60.9859 | 125 | 2.04965 | 487.887 ms | 504.712 ms |
| Overlapping 8×8 windows, stride 4 | 61.1707 | 132 | 2.15789 | 463.415 ms | 479.178 ms |
| Original High, closing reference | 61.8875 | 182 | 2.94082 | 340.041 ms | 352.289 ms |

The pooled bracketing reference is **2.94071 FPS / 340.054 ms**. Closing-reference FPS differs by only +0.0077% from opening. Separate tiles reduce FPS **30.30%** and increase mean frame time 43.47%; overlapping windows reduce FPS **26.62%** and increase mean frame time 36.28%. Each candidate has one clean trial, sufficient to reject these substantial regressions against stable references; this is not a repeated winning-candidate or statistical-significance claim. The earlier Low result is a separate quality trade-off and is excluded from this High-only comparison.

The diagnostic microprofiler makes the location of the added cost clear. Across 26 qualified reference groups, 16 separate-tile groups and 17 overlapping-window groups, mean opaque GPU interval is **229.753 → 381.085 → 359.143 ms**. The new bounds pass itself costs only **0.0394 / 0.1080 ms**. Shadow intervals remain about 53.4 ms, structure about 14.4–14.6 ms, SSR about 17.6–17.9 ms and auxiliary depth about 14.4–14.5 ms. Opaque submissions remain exactly 54,523,679 primitives, 1,717 commands, 991 visible renderables and 1,778 instance invocations; structure, SSR and auxiliary workload counters also match. This points to the changed material-fragment path rather than the reduction pass or additional scene geometry. Register pressure, instruction scheduling and the actual fraction of skipped samples have not been isolated. Retained GPU intervals are not certified complete-frame times; CPU history loss and unavailable/boundary coverage remain explicit, and CPU/GPU clocks are never aligned or added.

Actual-source host oracles execute the GLSL producer, consumer helpers and full original/candidate marchers through both gather and fallback sampling. Separate tiles pass 2,261,653 checks per path and four deliberate bad-source controls; overlapping tiles pass 2,781,547 checks per path and six controls covering lost overlap coverage, extra fetches and changed sample timing. A separate interval-containment oracle checks 1,450,251 intervals including partial edge tiles. The temporary sampler-accounting correction is tested against actual descriptor layouts, 69 method cases and seven negative controls, including preservation of the existing compute-material limit. All 48 host CTest entries pass for the initial candidate. The packaged desktop reduction shader uses float32 fetch/min/max/output without RelaxedPrecision or Float16. These checks establish bounded source/package properties, not final Intel ISA behavior or native pixel parity. Because both candidates fail the clean timing gate, native visual/API acceptance and other-platform acceptance are deliberately not completed, and neither candidate is promoted.

The first sandboxed reference cannot open Wayland and two candidate startup attempts stop on a workload-detector false positive; none contributes performance data. A read-only monitor identifies the latter as `/usr/bin/git` invoked by the Codex desktop app for a diff with hooks/fsmonitor disabled and explicit `--no-ext-diff --no-textconv`. All four qualified timing rows use the same narrowly reviewed detector: only that exact verified executable/parent/cwd/argument grammar is allowed and recorded; unknown, mutating, unreadable or actual compiling processes still block. The three observed real commands and sixteen negative classifier controls are preserved. Qualified runs have no input drift, application errors or checked kernel GPU errors. Validation layers are disabled for clean timing; no native validation qualification is inferred. The short desktop inspection is disclosed rather than represented as a completely idle host.

All experimental vendor files, material packets, generated embeds, test registrations and fixture edits are restored to their pre-trial bytes. The preceding dirty worktree and existing distributed Filament patch/tree pins are preserved. No preset or saved user configuration changes. The canonical helper reproduces the original renderer, native smoke and host-test executables byte for byte, and all 46 restored host CTest entries pass. Exact readback is in `final-restoration.json`, separate from rejected-candidate receipts. The [shared Sun rejection experiment](#november-11-shared-sun-rejection-experiment) tests the follow-up idea of moving rejection outside the repeated ray work; it also does not earn retention.

Reproduction commands, failed attempts, exact sources and binaries, package hashes, CPU controls, runtime/startup JSON and offline HTML traces, raw clean profiles and restoration proof remain under ignored `build/intel-depth-tiles/20261111/`. Start with `analysis/final-rejected-cohort.{json,md}`, `analysis/trace-comparison-01.json`, `analysis/overlap-trace-comparison-01.json`, `oracle/`, `producer-packet-audit/receipt.json`, `tile-v1-rejected-sources/`, `tile-v2-rejected-sources/`, `source-restoration.json` and `final-restoration.json`. Frozen renderer identities are original `4aff61b11df20e51c35704917368a7f224d8d8df57e87be9bde6db8b173d62c3`, separate tiles `503cffa5ff5bca0ed224be4bea0ef3e30c3a93b26d48692d810222331021adc6` and overlapping windows `b13411416f7de21e4c9d715f8bcb319985906a71e7f8bb1baa1aab4fd229438c`. No source release, benchmark score or cross-platform performance claim follows from these experiments.

## November 11 shared Sun rejection experiment

**Sharing conservative rejection across the sixteen Sun rays does not demonstrate a speedup, and both implementations are restored.** The full actual Heaven village retains contact-shadow quality High with up to 32 original depth steps per ray, unchanged sample positions and the sixteen original Sun directions, the fixed noon camera, 1920×1080 Visual Ultra, enhanced 4K assets and 4× MSAA. Hardware remains Intel BMG G31 `8086:e223` on Mesa `26.0.8-1ubuntu0.3`, kernel `7.2.6-070206-generic`, Vulkan/native Wayland, with the existing exact-driver compatibility fix. VRS, frame generation, HDR and dynamic resolution remain off; no geometry, texture, feature or saved user setting is reduced.

The first variant computes one conservative whole-cone depth test before the sixteen-ray loop. Only when every original ray/sample is proven empty does it return zero; otherwise it executes the byte-identical original fine marcher. The second variant bounds only the latter half of the original sample positions. A successful shared proof shortens all sixteen Sun marches to the same prefix, preserving the original `dt`, dither, retained positions, t=0 checks and first hit. It adds no rejection branch inside each fine step. Partially lit caps, non-Sun lights and uncertain inputs retain the full configured march. `TARGET_MOBILE` source controls retain the original full path, and the desktop shader-model allocation gate excludes mobile auxiliary images; this is not device/API acceptance.

Both variants use a separate float32 8×8 min/max depth image and leave the existing structure image, AO/SSR mips and rendering features intact. Shared projected intervals use positive-W checks, cancellation-aware absolute matrix-product reserves and the exact repeated sample-time additions, including samples beyond t=1. Native texel bounds cover clamped edges and up to two-by-two tiles. Unsafe projections or unsupported inputs fail open. A strict front-facing cone test preserves geometric self-contact. Thirteen lit packets are intentionally regenerated for each trial; the other 36 packets retain their bytes. The material compiler's validation behavior is unchanged; its obsolete froxel-sampler comment is temporarily corrected to describe the new sampler occupying that reserved budget.

The first clean pair appears promising, but the complete original/candidate/candidate/original sequence does not confirm it:

| Whole-cone cohort, in order | FPS | Mean frame time | p95 frame time |
| --- | ---: | ---: | ---: |
| Original, opening | 2.95810 | 338.055 ms | 353.173 ms |
| Shared whole cone, first trial | 3.08645 | 323.997 ms | 329.933 ms |
| Shared whole cone, repeat | 3.08540 | 324.107 ms | 330.028 ms |
| Original, closing | 3.14746 | 317.716 ms | 323.919 ms |

The initial +4.34% FPS comparison disappears against a closing original that is **6.40% faster** than opening. The candidate repeats closely, but is 1.97% slower than the adjacent closing reference. Pooled candidate FPS is only 1.08% above pooled reference, smaller than the reference drift, so no repeatable gain is established. Independent ten-second bins show about 1.5–1.8% within-run warming in all four runs; that alone does not explain the persistent cross-run reference shift. Assets and logical cache seeds match. This cohort has no during-run clock or consumed-power observations, so a hardware/cache cause is not claimed.

A fresh cohort adds identical read-only, timestamped Intel GT-frequency sampling at approximately 1 Hz. It measures the shared-tail candidate against two stable original runs:

| Shared-tail cohort, in order | Measured seconds | FPS | Mean frame time | p95 frame time |
| --- | ---: | ---: | ---: | ---: |
| Original, opening | 61.9144 | 3.14951 | 317.510 ms | 320.951 ms |
| Shared latter-half rejection | 60.8979 | 3.07071 | 325.657 ms | 333.477 ms |
| Original, closing | 61.6485 | 3.14687 | 317.776 ms | 325.389 ms |

The two references differ by only −0.0838%. Against their pooled **3.14819 FPS / 317.642 ms**, shared-tail rejection loses **2.46% FPS**, increases mean frame time **2.52%**, and increases p95 **3.21%**. It is rejected after one clean trial; no winning-candidate repetition or statistical-significance claim is made. All timing rows exclude more than two warmup seconds and retain at least sixty measured seconds, with private warm geometry and initially empty private shader caches. Independent analysis reconstructs raw float32 frame intervals and verifies exact hardware, effective settings, asset/cache identities, controller policy and output hashes. Both policies retain the narrow verified desktop-app read-only Git exception; unknown or actual competing build/capture processes still block.

All 59 / 59 / 58 clock snapshots in the final sixty process seconds of the new opening/candidate/closing runs report GT0 actual and requested frequency at **2800 MHz**, with unchanged 1200/2800 MHz configured limits. These are approximately 1 Hz observations, not a frame-level clock lock or proof of causation. Sampling costs about one millisecond per poll, identically applied to all three runs; it adds no GPU wait, driver override or frequency/power write. Consumed-power inputs are unavailable. The observed power-cap setting is configuration and is not reported as measured consumption. The earlier and later controller policies are analyzed as separate cohorts.

Microprofiler observations remain separate from clean timing. Initial original opaque time is 229.194 ms; whole-cone and tail captures show 218.859 and 220.091 ms, but their unchanged shadow, depth and SSR passes also become faster. A fresh post-drift original trace measures 225.318 ms opaque, 53.321 ms shadows, 14.357 ms structure, 17.828 ms SSR and 14.485 ms auxiliary depth. Tail opaque appears 2.32% lower than that trace while the other large passes are about 2.8–3.4% lower, yet its clean mean frame interval is worse against stable references. Neither stage comparison establishes an algorithmic speedup. The reduction itself costs about 0.039 ms. Opaque/structure/SSR/auxiliary submitted workloads match, including 54,523,679 main opaque primitives in 1,717 commands. GPU pass observations are not complete-frame timing or additive CPU/GPU totals. The final trace overwrites 180,201 CPU events and reports 1,453 stale-epoch events; its retained retry-wait union is 95.32% of retained Main-loop wall, not a complete CPU utilization measurement.

Actual-source host contracts compare the complete original and candidate Sun calculations, directions, first hits and sample sequences. Whole-cone desktop gather/fallback each pass 66,839,778 checks, including 17,598,000 original projected samples inside conservative bounds; mobile controls each pass 54,009,972 checks with zero shared early returns. Six deliberate invalid-source controls are rejected. Shared-tail desktop paths each pass 62,982,711 checks, including 8,852,800 original tail positions; mobile controls each pass 56,303,617 checks with full budgets and zero trimming. Seven negative controls cover invalid cone extent, unsafe W, changed time, missing adjacent depth, forced trimming, the strict front-facing/partial-cap full-budget guard and omitted cancellation reserves. Geometric t=0 has a separate positive zero-prefix control. Independent blockers whose first hits occur exactly at High sample indices 16 and 31 (zero-based) remain detected. These are bounded source/math checks, not Intel ISA, native pixel or other-platform acceptance. Since neither candidate passes the performance gate, native image/API qualification and distribution bootstrap/pin changes are not performed.

Both candidate source closures, packet/embed bytes, frozen binaries, failed/superseded builds, host controls, CPU/GPU traces and raw timing data remain under ignored `build/intel-shared-rejection/20261111/`. Start with `analysis/whole-cone-abba/comparison.{json,md}`, `analysis/half-tail-cohort/comparison.{json,md}`, `analysis/half-tail-postdrift-trace-comparison.json`, `oracle/final-contract-receipt.json`, `tail-oracle/final-contract-receipt.json`, `whole-cone-unqualified-sources/` and `half-tail-rejected-sources/`. The original renderer is SHA-256 `4aff61b11df20e51c35704917368a7f224d8d8df57e87be9bde6db8b173d62c3`; whole-cone `c77cbe87758f3537313925e7ea33b9554e999a0ae3946ef9a46e1544ad19da02`; shared-tail `04bcc75c69f0e17d4aac6c0b0135e396930c8c8c4a520580567c3054bd4e318f`. Build receipts bind executable identities; contemporaneous source observations are retained separately.

All nineteen vendor edits, forty-nine production packets and generated embeds are restored, preserving the preceding dirty worktree, original presets and existing Filament patch/tree pins. Source restoration readback covers all 1,193 recorded files before this documentation update. The canonical helper rebuilds the original renderer, smoke and host-test binaries byte for byte; all 46 restored host CTest entries pass. Proof is recorded in `source-restoration.json` and `final-restoration.json`. No benchmark score, release or claimed FPS improvement results from these trials. Further optimization needs measurements of shader execution and actual successful rejection coverage.

## Intel GPU diagnostics checkpoint, November 11, 2026

The next investigation adds optional native invocation counters, compiled-shader properties and bounded shader text to the microprofiler, with read-only Intel clock sampling. These diagnostics preserve the preceding contact-shadow source and all production material bytes. Their retained Filament tree is `91a9e95e4e4f6ef6c7956be4b956a72573ad5ba2`; the final renderer is SHA-256 `87cbc1cb466a684ec231f98dc1c63628aa93c1ddde7c6fa21362e4c5a33bddc6`. The distributed patch, source pins, fresh/repeated bootstraps and native counter/pixel controls pass. See [GPU diagnostics](GPU_DIAGNOSTICS.md) for commands, exact counter semantics, evidence and validation limits.

On Intel BMG G31 at 1920×1080 Visual Ultra/MSAA4/High contact shadows, the main opaque pass remains the largest measured GPU interval at a 213.106-ms median. The common colored-penumbra lit fragment variant reports 265 maximum live registers and compiler spill/fill operations. Removing grass and dense foliage greatly reduces vertex invocations but leaves that pass around 216 ms in a short diagnostic probe. These observations support investigating lit/contact-shadow register lifetimes next; they do not assign a per-material GPU duration or establish an optimization gain. Full/away/grass-disabled captures retain every observed mandatory compiler identity, while optional text loss and overwritten CPU history remain explicit.

Eight clean full-scene windows keep compiler/invocation diagnostics off. Paired original/current/current/original means are 317.793674/317.832114 ms, a +0.0121% disabled-path difference within observed repeat variation. Hidden/visible recording means are 317.416882/320.895258 ms, but surrounding Off controls drift by 1.77%, so precise recording overhead is not isolated. Actual accepted intervals, incomplete retained task histories, observer telemetry and unchanged-input receipts remain under `build/intel-diagnostics/20261111/performance-final-01/`. The separate [signed INT8 compute workloads](INTEL_GPU_FEATURES.md) measure DP4a and cooperative-matrix implementation ratios with exact CPU verification; they do not change Heaven rendering or its score.

## Intel shader register-pressure trials, November 12, 2026

No shader optimization is retained from these trials. Three candidates preserved the High contact-shadow algorithm but failed to establish a safe performance improvement. The final controlled candidate is consistently slower, so all shader/compiler edits, material packets and embedded arrays are restored. Register-pressure reduction remains unresolved.

The workload is the complete Heaven scene at the pinned village camera, 1920×1080, Visual Ultra with enhanced 4K textures, MSAA4 and High contact shadows. All sixteen Sun directions, thirty-two steps per ray, distance, displacement, actual-triangle self-hit, dual-depth reconstruction, tolerances, screen-edge fade and lighting accumulation are preserved. VRS, frame generation, HDR, dynamic resolution, music, video/readback, profiler recording, opt-in compiler/invocation diagnostics and validation layers are off during clean timing. Hardware is Intel BMG G31, `8086:e223` at `0000:03:00.0`, `xe`, Mesa `26.0.8-1ubuntu0.3`, Linux `7.2.6-070206-generic`. Effective configurations, animation policy and private geometry/shader-cache inputs are verified per window; this remains one camera/workload on one driver.

| Candidate | Hypothesis | Measured compiler result | Outcome |
| --- | --- | --- | --- |
| C1: earlier directional visibility | Avoid retaining derived BRDF/IBL values through contact rays | Dominant SIMD16 shader remains at 265 maximum-live registers; spills/fills change 4/24 → 5/22 | No demonstrated gain; restored |
| C2: shared pixel dither | Compute the identical gradient noise once for the sixteen Sun rays | All thirteen observed program groups retain their maximum-live, spill/fill, GEN and SEND counts | No native pressure reduction; restored |
| C3: looped cap inversion | Request `DontUnroll` for the existing twelve-step inverse CDF in eligible desktop Vulkan fragments | Dominant GEN instructions 4,422 → 3,855; code 64,656 → 57,856 bytes; maximum-live 265 and spills/fills 4/24 unchanged | Clean frame time increases 1.77%; restored |

C1's initial sixty-second screen is inconclusive by itself: opening/closing reference means drift from 340.739 to 360.012 ms, with candidate 351.390 ms inside that range. The reference's later bins return near the opening reference, while sparse core-clock observations do not explain the early transient. Its pooled +0.36% candidate difference is not a causal regression or gain. That failure motivates a prospectively declared longer warmup rather than selecting a favorable short tail afterward.

The source-hash-matched baseline assembly identifies scratch slots for the actual contact-normal X/Y and fragment-coordinate X/Y values. The Sun marcher reloads coordinates to evaluate gradient noise. This motivates C2, whose host arithmetic is exact but whose native maximum-live, spill/fill, GEN and SEND compiler statistics remain unchanged. The complete private driver dump uses [Mesa's documented `fs,ann` and hash-filter controls](https://docs.mesa3d.org/envvars.html#intel-debug), separately from all clean timing. A parent reader bounds retained logs to sixteen MiB without changing the production sixty-four-KiB executable-IR retrieval limit. The initial sandbox-display failure and a process-wide file-limit startup failure remain archived; no clock, power, CCS or global driver setting is changed.

C3's final packaged SPIR-V proves induction from zero through twelve iterations with `DontUnroll`, and its matched native NIR/assembly proves the actual twelve-bound backedge inside the sixteen-ray loop. All thirteen rebuilt production models retain that loop. The colored-penumbra packet's other hundred excluded shader texts, including OpenGL, mobile, vertex, depth and SSR routes, match the baseline bytes. Static code size decreases across the eleven lit program groups, but this does not measure dynamic instruction work, occupancy or runtime memory traffic. Anisotropy is less favorable: its spills/fills increase from 16/38 to 24/48. Driver cycle estimates are not performance measurements.

The final reference → candidate → candidate → reference cohort requests 125 runtime seconds per launch. Before execution, its analysis specifies the shortest contiguous suffix of complete accepted intervals totaling at least sixty seconds and requires at least sixty seconds of preceding accepted intervals. Actual warmups are 64.90–65.00 seconds. Original native statistics are preserved separately; earlier sixty-five-second captures are never relabelled with this protocol.

| Window, in order | Accepted FPS | Mean frame time | p95 frame time | Retained seconds |
| --- | ---: | ---: | ---: | ---: |
| Baseline A3 | 3.15176 | 317.283 ms | 321.540 ms | 60.284 |
| Candidate C3B1 | 3.09729 | 322.862 ms | 328.728 ms | 60.052 |
| Candidate C3B2 | 3.09824 | 322.764 ms | 329.015 ms | 60.034 |
| Baseline A4 | 3.15335 | 317.123 ms | 322.152 ms | 60.253 |

Pooled retained means are **317.202893 → 322.813431 ms (+1.76875%)**, or **3.152556 → 3.097765 FPS (−1.73801%)**. Reference repeat spread is 0.0503% and candidate repeat spread 0.0304%. The separate native full-window comparison also slows by 1.75537%. Both C3 repeats are consistently slower in this cohort. These are accepted presentation intervals, not physical scanout; asynchronous last-completed GPU samples are never added to CPU intervals. Nonzero late GT0 clock observations are 2,800 MHz; a final zero sample does not establish throttling. The 275-W power cap is configuration, not measured consumption.

C1's actual-source guard matrix passes 9,984 comparisons. C2/C3 arithmetic oracles each compare more than eight million exact ray samples across desktop/mobile and gather/fetch paths, with deliberate invalid controls rejected. C3's actual C++ prolog scope matrix checks 24,576 states. Its native Vulkan suite passes 56 cases with six disclosed skips, including all eighteen stability configurations and independent finite-Sun blocker tests at unchanged thresholds, with zero validation or kernel GPU errors. These correctness results do not establish every-view pixel parity; extended paired image and other-platform qualification is not reached after the performance rejection. One diagnostic capture is interrupted by an unknown competing process match and is retained separately; its retry passes. C3 compiler metadata coverage is complete, while its 61 pending trailing-frame query samples remain explicitly pending.

All 1,206 original source-file hashes match the opening baseline before this documentation update, and the separately preserved compiler source/header also match. All forty-nine production packets and both embedded-array files are restored. The canonical helper rebuilds renderer `87cbc1cb466a684ec231f98dc1c63628aa93c1ddde7c6fa21362e4c5a33bddc6`, smoke harness `c4314d29c3080abffc9c593d14de7006ab55090dd0ef9d08286ac17d0922220d` and matc `ac38d12484eaa7070f46024f2800c8c7090278b9df1807001e3ffc5dba6b5bc0` byte for byte. All forty-eight restored host CTest cases pass. The four external test-material packets match the pre-task November 11 fixture/full-smoke receipts. Vendor HEAD/index/status semantics and the existing distributed patch/hash/tree pins are unchanged; no new source bootstrap is claimed for these rejected candidates.

Exact commands, immutable candidate binaries/source closures, failed controls, native shader dumps, checksums and raw profiles remain under ignored `build/intel-register-pressure/20261112/`. Start with `candidate-3-final-summary.md`, `long-cohort-c3-01/comparison.json`, the `contracts/` receipts, `driver-dump/` analysis and `final-source-restoration.json`. The cohort checks 267 explicit artifact paths and 1,207 candidate source hashes; the additional compiler path leaves that changed-file set again after restoration. Current-source observations and the older executable's preserved embedded-source closure remain separately labelled. Do not reintroduce any of these candidates as an established optimization or claim that smaller compiler output implies lower register pressure or faster rendering.

## Filament 1.80.1 follow-up, November 13, 2026

The November 12 register-pressure trials above retain their Filament 1.77.2/package77 source and exact binaries. The current 1.2.0 candidate uses Filament 1.80.1/package80, retains the original High contact-shadow policy and excludes all three rejected shader rewrites. Register-pressure optimization remains open; old compiler/register estimates do not automatically describe the new base. [Release readiness](RELEASE_READINESS.md#november-13-filament-1801-and-120-candidate) records the separate source/native/static/image checks and pending CI/package/publication gates.

Fresh cohort04 compares the preserved development renderer `87cbc1cb466a684ec231f98dc1c63628aa93c1ddde7c6fa21362e4c5a33bddc6` (application stamp 1.1.1, Filament 1.77.2) with final local 1.2.0 renderer `3cf93279ce4b295e3f54c5af5f30ae27e43d9bdeceb89086ccdf71923f4bfcbb` (Filament 1.80.1). The baseline is **not the published 1.1.1 release**. The full Heaven fixed-noon/fixed-camera scene uses Intel BMG G31 / Mesa 26.0.8, Vulkan/X11, 1920×1080, Visual Ultra, enhanced 4K textures, MSAA4 and unchanged High contact shadows at 32 steps / 16 Sun rays. Frame generation, VRS, dynamic resolution, HDR, capture, recording, invocation/IR diagnostics, validation and music are off. Ordinary wall-time animation remains active.

A1→B1→B2→A2 provides two windows per treatment, each requesting 125 runtime seconds. The complete native profile preserves its ordinary warmup and full statistics. Before launch, a separate tail policy selects the shortest final contiguous suffix of whole accepted intervals reaching 60 seconds, requiring at least 60 preceding accepted seconds; no interval is trimmed or boundary-interpolated, and failed earlier 65-second cohorts are not relabelled. Prefix durations are 64.799–65.118 seconds and retained tails 60.034–60.220 seconds.

| Interval scope | Development baseline mean | 1.2.0 candidate mean | Mean frame-time change |
| --- | ---: | ---: | ---: |
| Complete native measured windows | 327.928 ms | 329.834 ms | +0.581% |
| Prospectively declared final tails | 325.886 ms | 334.546 ms | +2.657% |

Complete-window pooled accepted FPS is 3.04945→3.03183 (−0.578%), with baseline endpoint mean drift +2.636% and candidate repeat drift −0.840%. Adjacent full-window mean changes have opposite signs, +2.319% and −1.146%. Declared-tail accepted FPS is 3.06855→2.98913 (−2.588%); both adjacent tail comparisons are slower, +3.112% and +2.207%, with baseline/candidate tail repeat drift −0.234%/−1.110%. These are descriptive statistics from autocorrelated frames in one view with two repeats, not confidence bounds or a universal causal estimate. The cohort associates modestly slower declared tails with this candidate and demonstrates no speedup.

The whole-controller background log retains 15 accepted observations of three unique verified read-only GUI Git processes in A1 and none in B1/B2/A2, with 15 successful same-process additive rechecks and no blockers or failed rechecks. Those counts include startup and are not frame/tail aligned. Accepted desktop Git activity is not CPU/I/O isolation. Read-only one-Hz GT0 current-frequency medians are 2800 MHz across all four controller traces, including startup; this is not tail alignment, thermal/power normalization or proof of isolation.

Accepted presentation intervals include rejected attempts and are not physical scanout/display FPS. Asynchronous last-completed whole-frame GPU values may lag or repeat; their column means are not independent GPU distributions, CPU/GPU clocks are not aligned and their durations are not added. All four windows retain unchanged source/effective quality and no recorded validation finding or kernel GPU error, and independent post-analysis checks verify every owned controller/renderer/analysis process drained. Source closures, plans, original profiles, separate full/tail calculations and GUI observations are retained under ignored `build/filament-1.80.1/20261112/native-plan/performance-final-04/{final-results-handoff.json,comparison-summary.md}` and `performance-x11-final-04/`. Failed cohorts01/02/03 remain preserved, unqualified and unused. This local development comparison establishes neither published-release performance nor other-platform acceptance; fresh exact-source CI, all eight package/source audits, downloaded-package controls and publication remain pending.

## Intel shader register-pressure trials on Filament 1.80.1, November 13, 2026

No shader optimization is retained from this investigation. Two controlled full-scene comparisons failed to establish a safe improvement; a final structural candidate failed its compiler-allocation gate before further timing. The canonical renderer, Filament shaders, material compiler, all 49 production packages, both embedded-array files and generated test packages are restored byte for byte. The baseline remains approximately 2.9 accepted frames per second in this demanding fixed-camera configuration. These trials are separate from the November 12 Filament 1.77.2 experiments above.

The preserved baseline is application 1.2.0 / Filament 1.80.1, renderer SHA-256 `3cf93279ce4b295e3f54c5af5f30ae27e43d9bdeceb89086ccdf71923f4bfcbb`, with root source parent `5f49e7e45811f3b7f744b03038aae7275ca1f3ba`. All treatments load the complete Heaven scene on Intel BMG G31, `8086:e223` at `0000:03:00.0`, driver `xe`, Mesa `26.0.8-1ubuntu0.3`, Linux `7.2.6-070206-generic`. The camera is `-2.873719,-42.924889,39.4375,83.867966,-14.378684`, FOV 60, noon, 1920×1080 Vulkan/X11, Visual Ultra, enhanced 4K textures and MSAA4. Grass, dense foliage, displacement and ordinary wall-time animation remain enabled. High contact shadows retain all 16 Sun directions, 32 depth steps, distance, original CDF/inversion arithmetic, receiver self-hit, depth reconstruction and sampling tolerances. Frame generation, VRS, dynamic resolution, HDR, audio, readback, validation, recorder/UI and compiler/invocation diagnostics are off during clean timing.

Each comparison uses baseline → candidate → candidate → baseline, requesting 125 runtime seconds per launch. Native full-window statistics retain the normal warmup exclusion. A separately declared policy also retains the shortest final contiguous suffix of whole accepted intervals reaching 60 seconds, after at least 60 accepted prefix seconds; there is no post-hoc trimming or interpolation. Source, private executable, hardware, effective settings, geometry/cache inputs and process ownership are checked per window. These are accepted presentation intervals in one full-scene view, rather than physical scanout or a complete benchmark-tour result.

| Trial | Native-full baseline → candidate mean | Full-window change | Declared-tail baseline → candidate mean | Tail change | Decision |
| --- | ---: | ---: | ---: | ---: | --- |
| C1: specialize the already validated Sun-cap CDF branch | 351.734 → 350.636 ms | −0.312% | 352.693 → 358.693 ms | +1.701% | No repeatable gain; restored |
| C2: cache valid Sun visibility before material inputs, retaining the late finite-Sun fallback | 344.211 → 351.104 ms | +2.003% | 343.850 → 350.858 ms | +2.038% | Both candidate repeats slower; restored |
| C3: remove the eligible late finite-Sun body entirely | Not run | — | Not run | — | Failed the prospective compiler gate; restored |

C1 removes 75 reported native instructions from the common lit variants while leaving their maximum-live, spill and fill counts unchanged. Candidate repeat spread is 6.461% for native-full means and 8.512% for declared tails; the small pooled full-window difference does not establish a speedup. C2 has much tighter repeats: baseline/candidate full-window mean spreads are 0.040%/0.043%, and tail spreads are 0.060%/0.102%. Both C2 repeats regress relative to their adjacent baseline, with pooled accepted FPS 2.905 → 2.848. These are descriptive matched-run observations, without a universal causal or per-material cost claim.

C2 moves contact work before `MaterialInputs`, preserves the original late lighting predicates and keeps implicit mipmapped VSM sampling late. It nevertheless retains a complete late finite-Sun route for unsupported early-map modes. C3 instead makes contact readiness independent of map readiness: every valid eligible Sun precomputes contact, PCF/EVSSM may gate it on cached map visibility, and VSM/unknown modes evaluate their map at the original late point. Eligible late code contains only disabled-contact zero or the ordinary mapped-direction fallback; noneligible materials preserve the original Sun wrapper. Speculative work on mapped-backlit/discarded opaque fragments, and C3's late-map-zero cases, is explicitly counted. Neither rewrite changes the ray algorithm or reduces rendering quality settings.

Fresh driver statistics for the main `S2D | SRE` SIMD16 fragment variants show why a lower global peak alone is insufficient:

| SIMD16 fragment program | Baseline live / spills / fills | C2 live / spills / fills | C3 live / spills / fills |
| --- | ---: | ---: | ---: |
| Colored penumbra | 265 / 4 / 24 | 263 / 37 / 54 | 261 / 65 / 95 |
| Anisotropic | 276 / 16 / 38 | 274 / 43 / 64 | 272 / 89 / 146 |
| Clear coat | 268 / 7 / 32 | 266 / 35 / 54 | 264 / 54 / 80 |

The C3 gate was recorded before its diagnostic: all three critical programs must avoid increased maximum-live/spill/fill counts, with a meaningful reduction in the common and anisotropic variants. C3 fails because emitted spill/fill counts rise sharply. Its complete executable comparison checks 1,544 pipeline/executable records per subject before deduplication, finds 230 unchanged and 12 uniquely changed tuples, and has no missing or ambiguous matches. These are compiler-wide static properties, not executed spill traffic, occupancy, per-material GPU time or a clean C3 FPS result. No additional C3 timing, native quality suite or paired-image acceptance is claimed.

The fresh baseline diagnostic retains a 230.498-ms median opaque-pass interval across 11 invocations. Ordinary clean profiles spend about 3 ms in CPU rendering submission, supporting continued investigation of the GPU material path and backpressure. The diagnostics do not isolate register pressure as the cause of that pass duration. Overwritten CPU histories and bounded/truncated optional shader text remain disclosed; compiler header/statistic loss is zero. GPU intervals are not added to CPU durations or treated as complete-frame coverage.

C1 and C2 each pass the 50 named host checks and the unchanged native Vulkan suite (56 passes, six skips) and OpenGL suite (55 passes, seven skips). C1's two initial `/tmp` no-exec harness failures are retained alongside their successful executable-TMPDIR reruns; C2 passes all 50 directly, with the separately self-gated native CTest entry explicitly skipped before the owned API suites. C3's actual-source scheduling oracle checks 16,464 states across 14 profiles, rejects 12 functional negatives, and rejects a deliberately dead late finite call structurally despite its numerically correct output. Its separate fine-march proof compares 8,140,700 exact sample positions across four desktop/mobile × gather/fetch paths, with three arithmetic negatives rejected. Independent reviews preserve the sampler/derivative and delegated-host-oracle limits. Extended paired full-scene image validation and other-platform qualification are not reached because no candidate is promoted.

All failed controls, candidate sources/packages/binaries, raw profiles, diagnostics, exact commands and ownership/drain receipts remain under ignored `build/intel-register-pressure/20261113-v1.80.1/`. Start with `experiment-plan.json`, its explicit final-trial amendment, `candidate-{1,2}/analysis/comparison.{json,md}`, `candidate-3/compiler-comparison-01/summary.json` and `baseline/final-rebuild-identity-02.json`. Final canonical rebuilding verifies all 1,244 original recorded paths byte for byte before this documentation change; all 49 material-source mtimes are restored, and root/vendor Git readback is clean. The existing distributed Filament patch, checksum, source-tree pins, version and release remain unchanged. Future work should identify the actual executed spill locations and live values before another shader rewrite; these trials establish neither a safe speedup nor a proven tangent/UV or register-pressure root cause.

## Native Intel shader spill sites, November 15, 2026

The actual native spill locations are now mapped, but **no shader optimization is retained**. A receiver-normal lifetime change removes mapped hot-region scratch loads and passes the correctness and image gates; its full-scene gain is smaller than the declared warmed-up baseline variation. The original shaders, material compiler, renderer, smoke harness, all 49 production packages, both embedded-array files and generated test packets are restored byte for byte. This investigation uses the same Filament 1.80.1 baseline and full Heaven fixed village view described above: Intel BMG G31 / Mesa 26.0.8, Vulkan/X11, 1920×1080, Visual Ultra, generated 4K textures, MSAA4 and unchanged High contact shadows with 16 Sun rays and 32 depth steps.

A separate, complete Mesa `fs,ann` dump is bound to the baseline colored-penumbra SIMD16 fragment executable, source hash `0x692c33b0`. It contains 4,413 instructions, seven loops, maximum-live 265, four scratch stores and 24 fills. Native dataflow identifies the values rather than inferring them from the reported global maximum:

| Baseline scratch offsets | Mapped values | Whole-program static stores / fills | Sun-ray setup |
| --- | --- | ---: | --- |
| 128 / 192 | Receiver-normal X / Y | 2 / 14 | Two normal reloads per outer-loop iteration |
| 0 / 64 | Fragment-coordinate X / Y | 2 / 10 | Two coordinate reloads per outer-loop iteration |

The inner 32-step depth march contains no scratch accesses in this inspected executable. Reported SEND count 88 excludes inserted scratch operations; the final emitted program contains 116 SEND instructions, including four scratch stores and 24 fills. These are static instruction/dataflow observations for one exact executable, not measured executed traffic, occupancy, per-material GPU time or proof that spills cause the opaque-pass bottleneck. The dump preserves its raw native hash label without assuming that label's digest algorithm.

Candidate C1 retains the original normal derivative, normalization, fallback and view-facing arithmetic, but computes it after `evaluateIBL` in eligible opaque tangent-bearing lit routes. Masked/blended, unlit, custom-depth/discard and custom-surface-shading routes preserve the original early point. Actual compiler AST controls and the scheduling oracle check these exclusions. In the native colored executable, normal normalization interleaves the final IBL instructions; this is not evidence that every native normal instruction follows completed IBL work.

C1 removes both normal scratch stores and all 14 normal fills. The replacement slots hold emission R/G: their four static stores occupy mutually exclusive sampled/zero branches, and their two fills occur after lighting. Maximum-live remains 265; whole-program static scratch counts change from 4 stores / 24 fills to 6 / 12, and final SEND count falls from 116 to 106. The Sun-ray setup retains only the two coordinate reloads, with no scratch in the inner march. Anisotropic remains 276 / 16 / 38 maximum-live / stores / fills; clear coat changes from 268 / 7 / 32 to 268 / 10 / 21. Their costs are not inferred from the colored executable's site mapping. The original strict compiler-count gate fails because stores increase; a separately recorded, prospective exception allows quality and timing checks after establishing the specific hot-to-cold value migration. It does not erase that failure or accept performance.

C1 passes 50 host entries and the complete native Vulkan suite (56 passes, six skips) and OpenGL suite (55 passes, seven skips). Four A1/B/A2 image triplets cover the material-model fixture and noon village, morning roof and morning stone views. The material fixture is byte-identical across all three captures, with actual advanced-material submission evidence; the three Heaven views pass their predeclared numerical and agent visual checks against both originals. Differences remain within the declared AA/noise limits, including at most two channel levels in the roof and stone triplets. These scoped checks do not establish whole-tour, other-device or human visual acceptance. No permanent test is promoted for the rejected implementation; its portable CPU/AST draft remains with the experiment.

Only complete clean cohort03 qualifies for performance analysis. It uses original → candidate → candidate → original, 125 runtime seconds per launch, unchanged native full-window statistics and the prospectively declared shortest final whole-interval suffix reaching 60 seconds after at least 60 preceding accepted seconds. Diagnostics, validation, recording, readback, overlays, frame generation, VRS, dynamic resolution, HDR and music are off. Each window verifies the same source, binary, hardware, effective settings and private geometry-cache seeds.

| Interval scope | Baseline pooled mean | C1 pooled mean | Frame-time change | Accepted FPS, baseline → C1 |
| --- | ---: | ---: | ---: | ---: |
| Complete native measured windows | 356.692 ms | 354.751 ms | −0.544% | 2.804 → 2.819 |
| Prospectively declared final tails | 356.583 ms | 355.773 ms | −0.227% | 2.804 → 2.811 |

The full-window criterion passes, but the tail criterion fails: baseline endpoint drift is +0.630%, and the first candidate tail is 0.068% slower than the opening original. Both candidate tails are slower than that opening original. The nominal pooled tail gain therefore does not establish a repeatable improvement; neither a proven speedup nor a proven regression is claimed. Accepted presentation intervals include rejected attempts and are not physical scanout FPS. Asynchronous GPU column values may lag or repeat and are not added to CPU durations.

Earlier complete-cohort attempts01/02 stop on short-lived competing make/compiler processes during B2; both entire cohorts, including their completed windows, remain excluded. Read-only origin sampling finds root-owned processes under `containerd-shim`, with inaccessible working directories; controlled probes do not establish their owner/project or a causal tool trigger. No unrelated process is stopped or added to the allowlist. After user coordination, the last cohort uses the existing attached session without extra command launches during timing and completes without a competing-work fault. Known read-only desktop Git activity and sampling limits remain disclosed; this is not CPU/I/O isolation. The prospective final retry amendment raises the clean-attempt cap from eight to ten while preserving the 2,500-second native wall cap and a bounded 740-second outer supervisor. All 27 launched native applications consume 2,368.027 seconds, including failed/interrupted work; there are no further native attempts.

Exact assembly, source mapping, candidate packages/binaries, all original profiles, failed controls, images, commands and ownership/drain receipts remain under ignored `build/intel-register-pressure/20261115-spill-sites/`. Start with `source-spill-map-01/mapping.json`, `candidate-1/source-spill-map-01/mapping.json`, `candidate-1/analysis/comparison-eligible03.{json,md}`, `candidate-1/final-root-decision-01.json` and `baseline/final-rebuild-identity-01.json`. Canonical rebuilding and the final host rerun verify all 1,245 original recorded files before this documentation addition, preserving the previous documentation delta; the final host result is 50 passes, one self-gated native skip and zero failures. Renderer SHA-256 remains `3cf93279ce4b295e3f54c5af5f30ae27e43d9bdeceb89086ccdf71923f4bfcbb`; the distributed patch, checksums, source-tree pins, version and release are unchanged.

Future spill work should trace coordinate offsets 0/64 through a genuinely different lifetime or rematerialization policy before another timing cohort. Repeating the previously rejected shared-dither spelling is not a new native dataflow hypothesis. Runtime execution/stall evidence, where available, should guide whether these remaining reloads merit further work; another static scratch reduction alone is insufficient evidence of an FPS improvement.

## Intel screen-coordinate stall investigation, November 16, 2026

No shader optimization is retained. The original Filament shaders, all 49 production packages, both embedded-array files, four generated test packets and canonical renderer/smoke/material-compiler binaries are restored byte for byte. The coordinate-lifetime candidate is directionally promising, but it fails this investigation's prospectively frozen conservative eight-window retention rule. A separate evidence-handling mistake also prevents complete current requalification of the timing captures; the raw measurements below are descriptive, not an accepted speedup.

The workload remains the full Heaven village view on Intel BMG G31 `8086:e223` / PCI `0000:03:00.0`, Mesa `26.0.8-1ubuntu0.3`, Vulkan/X11, 1920×1080, Visual Ultra, generated 4K textures and MSAA4, using the same Filament 1.80.1 baseline as the November 15 investigation. High contact shadows retain all 16 Sun directions, 32 fine depth steps, CDF/math, distance, tolerances and sample-position semantics. Geometry, materials, grass, foliage, animation and feature settings are unchanged.

A genuine bounded Xe EU-stall collector passed a disabled open/close probe after receiving temporary `CAP_PERFMON`. Its single enabled capture retained 101,376 complete 64-byte records (6,488,064 bytes) over approximately 0.632 seconds before reporting `EIO`/lost data. The same interval contains an actual render-engine CAT fault/reset, a timed-out owned Heaven context and Vulkan device loss. These observations do not establish the reset's cause or the order of report loss, disable and resource retirement. Further EU-stall capture is held pending a verified applicable fix or supported safe collection contract. The temporary capability-bearing executable was removed; kernel, driver, security/paranoid and clock/power settings were not changed. Subsequent ordinary upload/readback and rendering checks completed without a new GPU fault.

The user exported the existing owned Heaven crash snapshot with a bounded helper. Offline analysis decoded only its instruction-pool VMA and recovered two exact 64,944-byte copies of the baseline native shader. Matching uses [Mesa's heap-relative shader-line convention](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/56aa8e8012b6ce13b8611961a09449fc8b900caf%5E!/), explicit subtraction of the observed instruction base, and exact instruction boundaries. Retained records contain positive raw SBID fields at the coordinate-Y destination wait (sum 7) and coordinate-X-consuming FMA (sum 55). The format is GT-global, with no PID, draw or timestamp tag; identical numeric offsets do not independently establish process-specific ownership. Lossy partial records provide no cost proportions, absence conclusions, CPU/GPU clock alignment or proof that these instructions caused the reset.

C1 replaces the IBL-only `getNormalizedPhysicalViewportCoord().xy` construction with the existing high-precision `shading_normalizedViewportCoord`, preserving the same `uvToRenderTargetUV` conversion and guard. This removes an unused early FragCoord-Z read whose parent built-in load anchored XY before IBL. Fresh actual-source FP/guard controls pass, and native code confirms both coordinate scratch loads disappear from Sun-ray setup. Register allocation instead spills receiver-normal Z: the hot outer-loop fill sites fall from four to three, while maximum-live remains 265. This is a real lifetime/allocation change, not a demonstrated occupancy improvement.

C2 combines C1 with the previously tested eligible-opaque normal deferral. It reintroduces scratch loads for packed pixel X/Y and retains one receiver-normal load, leaving three hot outer-loop fill sites. It fails its prospective zero-hot-fill gate and is rejected before timing. Neither candidate introduces scratch into the inner 32-step depth march. The following counts describe the exact main colored-penumbra SIMD16 executable, not all material costs or dynamically executed traffic.

| Subject | Maximum-live | Static stores / fills | Literal final SEND sites | Sun outer-loop scratch |
| --- | ---: | ---: | ---: | --- |
| Original | 265 | 4 / 24 | 116 | Two coordinate and two normal loads |
| C1: physical-XY IBL input | 265 | 5 / 22 | 115 | Three normal loads |
| C2: C1 plus late normal | 265 | 10 / 21 | 119 | Two packed-coordinate and one normal load |

C1 passes 50 host checks, Vulkan's complete 56-pass/six-skip suite and OpenGL's 55-pass/seven-skip suite. Four fresh original/candidate/original image triplets cover material models, noon village, morning roof and morning stones, with unchanged numerical thresholds and separate model visual reviews. The material fixture is pixel-identical, including submitted clearcoat/aniso route evidence; scene differences stay within their measured original/original grain and AA floor. A long quality batch receives an unexplained external SIGTERM during its first stone capture: its completed eleven individual cases are retained, the interrupted stone image is excluded, and an entire fresh stone triplet passes separately. No completed-batch pass is fabricated. These are scoped Linux checks, not whole-tour, human or other-device visual acceptance.

Timing uses eight separately supervised windows in two original → candidate → candidate → original quartets, each requesting 125 runtime seconds. Original native statistics retain their two-second exclusion; the separately declared tail is the shortest final whole-interval suffix reaching 60 seconds after at least 60 accepted seconds of preceding history. Microprofiler recording, compiler diagnostics, validation, readback, overlays, frame generation, VRS, dynamic resolution, HDR and music are off. Each window checks the same source, executable, hardware, effective settings and private geometry-cache seeds. Accepted presentation intervals are not physical scanout FPS; asynchronously completed GPU samples are not added to CPU times or treated as independent observations.

The first quartet's full-window gain misses its repeat-spread rule while its tails pass. Before any second-quartet run, an explicit confirmation amendment requires both original gates to pass in that independent quartet and adds a stricter all-eight gate: each scope's pooled gain must exceed the larger of four-original repeat range/mean, four-candidate repeat range/mean and absolute first-to-last original drift. Every candidate must also improve both original endpoints of its own quartet. The initial full-window failure remains unchanged; no tail is retimed or selected afterward.

| Raw interval scope | Original pooled mean | C1 pooled mean | Nominal frame-time change | Frozen numerical gate |
| --- | ---: | ---: | ---: | --- |
| First quartet, native full | 342.830 ms | 339.482 ms | −0.977% | Fail: candidate spread 1.101% |
| First quartet, declared tail | 343.407 ms | 337.828 ms | −1.625% | Pass |
| Confirmation quartet, native full | 354.086 ms | 341.951 ms | −3.427% | Pass |
| Confirmation quartet, declared tail | 350.717 ms | 341.674 ms | −2.578% | Pass |
| All eight, native full | 348.365 ms | 340.712 ms | −2.197% | Fail: original range 4.904% |
| All eight, declared tails | 347.030 ms | 339.740 ms | −2.101% | Fail: original range 2.856% |

Every matched candidate comparison is favorable, and the confirmation quartet passes both original numerical gates. Nevertheless, the combined gains do not clear the predeclared larger original variation. Descriptive pooled accepted FPS is 2.871 → 2.935 for full intervals and 2.882 → 2.943 for tails. Neither a qualified causal speedup nor a regression is claimed, and C1 is not promoted solely from the favorable confirmation.

The independent final audit finds an additional provenance failure: the confirmation helper reused `boundary-observation-A1/B1/B2/A2.json` paths and overwrote all four first-quartet boundary bodies. Their original expected hashes and intact quiet-proof references remain, but the original bodies cannot be recovered or freshly reverified. Raw profiles, owner/source/input records and sealed per-quartet reports remain intact, and independent raw arithmetic reproduces the numbers above. The earlier validators' eligibility flags are insufficient for complete current transitive qualification because they did not revalidate those nested boundary bodies at final aggregation. No replacement body is guessed, no sealed report is rewritten and no qualification waiver is granted. A future-only helper uses cohort-specific evidence names and exclusive writes; eight CPU collision controls preserve original bytes on every overwrite attempt. This fixes future preparation, not the lost evidence in these captures. Future final analysis must recursively revalidate referenced gate files rather than trust a parent report alone.

All 30 native applications, including failures, consume 2,413.925 of the declared 4,000 native wall seconds. Both shader candidates and all eight timing-window attempts are exhausted; no further window or threshold adjustment follows the result. Candidate source/package/binary archives remain intact. Two independent restoration checks compare every one of the original 1,245 paths before these final documentation edits, including the preexisting documentation delta, and find no mismatch. The final restored host rerun passes 50 tests with one self-gated native skip and zero failures. Renderer SHA-256 is again `3cf93279ce4b295e3f54c5af5f30ae27e43d9bdeceb89086ccdf71923f4bfcbb`; vendor source tracking is clean, with unchanged patch/checksum/tree pins, version and release.

Evidence remains under ignored `build/intel-register-pressure/20261116-coordinate-stalls/`. Start with `runtime-evidence-decision-01.json`, `review/fault-shader-map-01/receipt.json`, both candidates' `native-map-01/receipt.json`, `candidate-1/comparison-segmented-01.json`, `candidate-1/comparison-segmented-02.json`, `candidate-1/comparison-confirmed-eight-01.json`, `review/all8-audit-01/receipt.json`, `review/all8-audit-01/boundary-provenance-loss.json`, `final-independent-audit-scope-01.json`, `candidate-1/final-root-decision-02.json` and `review/final-restoration-audit-01/receipt.json`. Archived-source replay must refresh shader-input mtimes and verify the newly built `matc`, packages and binaries: copying older timestamps initially caused Ninja to reuse C1 resources during C2 preparation, which was caught and corrected before a C2 GPU run. Further coordinate work remains open; it needs reliable runtime evidence and more stable measured conditions, not another equivalent getter spelling or a relaxed retention gate.

## November 16: immutable profiling harness and warmed Intel retest

The tracked Linux comparison tool is now `scripts/profile_compare.py`, with separate `profile_harness` modules for immutable evidence, owned-process cleanup, read-only telemetry and strict frame statistics. It replaces the ignored experiment helpers for future fixed-camera comparisons; `scripts/profile.sh` retains its scored full-tour/feature-ablation behavior. Each cohort gets a UUID and exclusive per-window attempt/result entries. Content-addressed blobs and explicit typed record references are verified recursively before analysis; missing or corrupted nested boundaries invalidate their parents. Publication and blob materialization are atomic and never replace existing entries. Cancellation, timeout, escaped descendants, log overflow, concurrent writers, nested loss, numeric boundaries and comparison rejection have CPU regression coverage. See [PROFILING_HARNESS.md](PROFILING_HARNESS.md).

The prospective protocol primes A and B for 65 runtime seconds into a combined sealed shader-cache seed, then copies that identical seed and the original warm geometry caches privately into every measured window. Runtime captures last 185 seconds; the primary metric is the shortest final whole-interval suffix reaching 120 seconds, with at least 60 seconds excluded before it. Native two-second-warmup statistics remain separate. Actual executable and mapped-driver identities, effective configuration, PCI device, full-scene geometry, kernel intervals and labelled temperature/clock observations are retained. No EU-stall sampler, clock/power/governor change, shader diagnostic, frame generation, VRS, dynamic resolution or quality reduction is used.

The initial prewarm completed but was rejected by an ownership-check bug: this host reports filesystem device `0:30` through `stat` and `0:29` for the same library inode through `/proc/maps`. The correction obtains the expected file's identity from a private data mapping, without executing library code, and compares exact mapping device/inode/path plus native process start ticks and executable inode. A real owned-process CPU test covers the positive case and separate device/inode/start-tick/executable failures. The original failed result is preserved, not repaired or reused. A fresh protocol explicitly charges its 105.313 seconds and one launch against the revised global limit of 13 Heaven launches and 3,406 native wall seconds.

The full Heaven Intel BMG G31 / Mesa 26.0.8 Vulkan/X11 retest retains 1920×1080, Visual Ultra, generated 4K assets, MSAA4, ordinary animation and High contact shadows with all 16 Sun rays and 32 depth steps. All 19 archived candidate source/package/binary mutations match their bindings; prior exact-binary CPU/native/image quality evidence is retained with its original limits. No earlier performance eligibility is reused. The two baseline pilots measure 341.848 and 344.001 ms/frame, a 0.628% spread that passes the declared 1% pilot screen.

| First complete quartet | Final 120-second window, mean ms/frame |
|---|---:|
| A1 baseline | 344.339 |
| B1 cached-coordinate candidate | 345.704 |
| B2 cached-coordinate candidate | 344.975 |
| A2 baseline | 346.281 |

Pooled baseline is 345.307 ms/frame (2.896 FPS), versus 345.340 ms/frame (2.896 FPS) for the candidate: −0.0094% improvement, effectively equal within observed variation. The two comparison baselines span 0.562%; the candidate pair spans 0.211%. The broader four baselines, counting both pilots and the quartet, span 1.288%, so residual longer-term drift remains. These scopes and sample counts differ from the preceding eight-window investigation; they do not establish an all-eight repeatability improvement. The already frozen acceptance predicate requires positive gain in each quartet. This complete first quartet fails that necessary condition, so the second quartet cannot qualify the candidate and is not launched. This is rejection of an unproven optimization, not evidence of a general regression. All collected samples remain; no favorable tail, retry or partial-cohort acceptance replaces them.

Nine Heaven applications, including the failed ownership prewarm, consume 1,686.671 native wall seconds. The candidate remains archived and production rendering source, material packages, distribution patch and dependency pins retain the baseline. The renderer is restored to SHA-256 `3cf93279ce4b295e3f54c5af5f30ae27e43d9bdeceb89086ccdf71923f4bfcbb`. The canonical build succeeds; final CTest passes 54 tests with one self-gated native skip and zero failures, including four registered harness suites containing 74 CPU cases. The first host invocation's two existing executable-fixture failures under this machine's `noexec` `/tmp` remain preserved; a repository-local executable `TMPDIR` passes both and the final entire suite. No fresh rendering-suite or whole-scene image acceptance is claimed for this harness-only change.

Evidence is under ignored `build/intel-register-pressure/20261116-harness-retest/`: `experiment-spec.json`, `experiment-spec-02.json`, `cohort-01/store/entries/windows/prime-A`, `cohort-02/rejection.json`, `cohort-02/store/entries/first-quartet-rejection`, `harness/seal_rejection.py`, `final-host-ctest.log` and the independent review receipts. The current quartet's complete typed evidence closure is retained alongside the original failed cohort. Do not reconstruct the overwritten historical boundary files or describe this stopped comparison as eight completed timing windows.

## November 16: retained directional contact helper optimization

The retained optimization skips directional contact tracing for derivative helper invocations in built-in desktop Vulkan shading. Real fragments retain all 16 finite-Sun directions, 32 High depth steps, distance, sample positions, CDF, receiver tests, tolerances and fades. Material/normal/specular-AA derivatives and mapped-shadow sampling precede the guard; helpers continue through ordinary shading afterward. Mobile, OpenGL, Metal, custom surface shading and custom-output paths retain the original contact policy. Punctual contact tracing is unchanged. This is independent of the rejected normal/coordinate lifetime and hierarchical-rejection experiments above.

Actual Intel native code confirms that the helper-mask predicate surrounds the directional gathers, including the Sun marcher, while the punctual gather remains outside it. All 48 fine-march opcodes and operands match baseline; maximum-live remains 265, static scratch stores/fills remain 4/24, and four instructions are added to the whole colored material. These compiler facts establish the intended mechanism, not executed helper frequency or an isolated GPU-pass saving. All 49 production packages were deliberately regenerated through the canonical build; 13 package bodies change, together with their generated embedded arrays. The baseline renderer is `3cf93279…`; the retained candidate is `ee1d2fc7…`.

The actual-source CPU contract passes 122,880 cases, 11 negative controls and five compiler-argument fixtures. A new Vulkan fixture compares two coarse triangles against 262,144 subpixel triangles with normal mapping/specular AA, a separate blocker, opaque/masked coverage and 1×/4× MSAA. All 16 complete baseline/candidate RGBA32F images are byte-identical, with no pixel exclusions. The complete candidate suites pass 57 Vulkan cases with six skips and 55 OpenGL cases with eight skips. Dense FLOAT capture uses the canonical C1 smoke `53461094…`; the complete suites use final smoke `efe6fb36…`, after a fixture-only Vulkan API guard is added. Production renderer, material packages and matc retain identical C1 bytes across that change. The new dense fixture is Vulkan-only: original-shader OpenGL 4× masked cross-tessellation parity failed, and that failure remains preserved rather than weakening its threshold. Earlier fixture setup/clear failures are also retained. These checks do not establish other GPUs or mobile execution.

Fresh A1/B/A2 captures match camera, fixed animation time, settings and output metadata for material models, the full village, a morning roof and a morning stone view. Material pixels are exact; roof/stone differences stay within two RGB8 levels. Village differences pass the existing repeat-relative whole-image, 32-pixel tile and connected-component rules against both baseline endpoints. Full images, native-resolution crops and amplified differences show no coherent new contact, geometry, UV or highlight change. This is scoped visual acceptance, not whole-scene byte identity or every-camera acceptance.

Only final cohort03 qualifies for performance. It uses the actual full Heaven scene at the fixed village camera on Intel BMG G31 `8086:e223` / `0000:03:00.0`, Mesa `26.0.8-1ubuntu0.3`, Vulkan/X11, 1920×1080, Visual Ultra, generated 4K textures and MSAA4. Grass, dense foliage, prepared displacement, ordinary animation and High contact shadows remain unchanged. Recording, shader diagnostics, validation, screenshot readback, overlays, frame generation, VRS, dynamic resolution, HDR and music are off; existing asynchronous native GPU-frame timing remains ordinary baseline behavior. No EU sampling, driver, clock, power, governor, kernel or security setting changes occur.

The final prospective protocol imports only the verified combined shader-cache artifact from a successfully completed, owned and drained earlier priming run. Its 376 members total 6,183,962 bytes; binary, hardware/driver, assets, settings, camera and geometry qualifications must match. Each of ten genuinely new launches receives private copies of that artifact and the original geometry caches. No foreign timing or synthetic priming window is imported. Captures last 260 runtime seconds; the primary metric is the shortest final whole-interval suffix reaching 180 seconds after at least 75 preceding seconds. All intervals remain intact, and the original native two-second-warmup statistics remain separate. The two fresh pilots are 323.558 and 323.566 ms/frame, a 0.00225% range.

| Comparison window | Mean ms/frame, final 180-second interval |
|---|---:|
| A1 baseline | 323.532 |
| B1 helper candidate | 316.951 |
| B2 helper candidate | 317.004 |
| A2 baseline | 323.570 |
| A3 baseline | 323.591 |
| B3 helper candidate | 317.051 |
| B4 helper candidate | 316.987 |
| A4 baseline | 323.591 |

Across 4,500 primary frame intervals, pooled mean frame time falls from 323.570809 to 316.998123 ms, saving 6.572687 ms/frame (2.03130%). Measured FPS rises from 3.090514 to 3.154593 (+2.07341%). The two complete ABBA groups improve by 2.03167% and 2.03092%. Baseline range is 0.01819%, candidate range 0.03165% and first-to-last baseline drift 0.01808%; the gain exceeds the 0.03165% observed repeat/drift gate and the baseline remains below the unchanged 1% limit. This is a modest repeatable gain at one full-scene view, not a solution to the remaining roughly-three-FPS workload or evidence of performance on another platform. Last-completed GPU samples can lag/repeat; no CPU/GPU sums, clock alignment or separate per-effect pass costs are inferred.

Cohort01 remains rejected before any A/B comparison: its 185-second pilots span 1.08107%, exceeding the 1% gate. A prospectively amended longer-interval cohort02 then loses its controller unexpectedly during pilot2; neither a supervised completion receipt nor a qualified second pilot exists. The renderer remains active after both controllers disappear, and root stops only that exact private executable/start-tick identity using a pidfd. No kernel GPU fault is recorded; the external termination source is unknown. The interrupted interval is conservatively charged from the prelaunch monotonic observation through exact-renderer cleanup. Its incomplete data is never repaired into a passing window or reused as final timing. No A/B window ran in either failed cohort.

The necessary harness repair detaches the private subreaper, adds controller/native parent-death guards and closes startup identity races. Abrupt controller loss is drained by a living supervisor; unrecoverable supervisor loss kills the direct workload but does not establish arbitrary descendant drainage or a valid completion receipt. The final timing controllers run independently of interactive tool transport with a 430-second emergency alarm; this does not imply immunity to cgroup-wide termination. CPU validation passes all 99 harness tests, three detached-launch tests and two independent real pre-arm race controls. Verified cache-only import has strict producer, reference, inventory, compatibility, tamper and foreign-window regressions. The default twelve-launch workflow remains supported; see [the harness guide](PROFILING_HARNESS.md).

Every failed fixture, invalidated capture, diagnostic, priming run, interrupted launch and completed timing run remains counted. The study finishes with 33 Heaven launches and eight native fixture launches. Ten final launches use 3,012.313217 native wall seconds; total conservatively charged native wall is 5,955.408948 seconds against the unchanged 6,500-second limit. Prospective amendments to the launch ceiling and durations remain alongside the original plans. The transport recovery is the last cohort; no favorable retry or relaxed acceptance gate replaces a failure.

An early temporary image adapter also retained four historical `--microprofile` output paths while rebinding its screenshot path. That capture is explicitly invalidated, its new outputs preserved separately, and all subsequent captures use a strict option allowlist with every output redirected to a fresh directory. Both historical JSON files and the referenced runtime HTML are restored against prior recorded hashes. The historical startup HTML is reconstructed from restored JSON and the unchanged actual exporter template, checked against independent untouched templates; its original standalone hash was unavailable. This limitation applies to the historical startup HTML, not the fresh images or final timing cohort. Adapter regressions and restoration receipts remain preserved.

The retained complete Filament patch SHA-256 is `387580771a5fbb0b31b4f5c30ed304895b98dde8073b11d11b8c5d7b85b5d624`, reconstructing tree `7f059d44d75a52e6eef14c267480e84045c90ddd` from unchanged public v1.80.1. Two independent isolated indices reconstruct that tree, whose sole change from the prior retained tree is the directional helper guard. Fresh and repeated complete file-only dependency bootstraps pass for all six dependencies; all 25 bootstrap fixtures pass using an executable repository-local `TMPDIR`, with the initial three `noexec`-`/tmp` fixture failures preserved. Live vendor HEAD, index, configuration and worktree status remain unchanged by distribution verification. These checks establish source reconstruction and idempotent local bootstrap, not public-network or cross-platform runtime acceptance. See `vendor-retention01/receipt.json` and [the retained patch notes](../patches/filament/README.md).

Evidence is under ignored `build/intel-register-pressure/20261116-helper-contact/`. Start with the prospective plans, `candidate-1/post-build-source-03.json`, `review/aba-image-visual-review-01.json`, `review/native-helper-mask-01/proof.json`, both failed cohorts' rejection receipts, and `benchmark/cohort-03/{report.json,store/entries/analysis}`. The independently verified native, image and source scopes remain separate from performance and from the distributed Filament patch reconstruction. Version and release publication are unchanged.

## Further contact-shadow candidates, November 16–17, 2026

Neither follow-up candidate is retained. The target stays the full Heaven scene at the same fixed village camera on Intel BMG G31 `8086:e223`, PCI `0000:03:00.0`, Mesa `26.0.8-1ubuntu0.3`, Vulkan/X11, 1920×1080, Visual Ultra, generated 4K requested with matching existing authored-texture fallbacks, and MSAA4. Grass, dense foliage, prepared displacement, ordinary animation and High contact shadows remain enabled. All 16 Sun directions, 12 inverse-CDF bisections, the Newton clamp, 32 fine depth steps, reconstruction, hit tests, tolerances, distance and fades are preserved. The baseline is the retained helper renderer `ee1d2fc7e75a8c7ee579a0b24dec4c793fbc8c9e26c3af22f63ffad52c9580dc`; neither earlier measurements nor compiler counts establish a benefit for these candidates.

The first hypothesis packs the same four clamped depth neighbors into a padded RGBA32F structure target, replacing each fine-step gather with a point load. Its corrected UBO publication, source-aware numerical controls, poisoned-producer activation, dense FLOAT pairs, fresh images and native API suites pass within their documented scopes. Its fresh baseline pilots measure 344.602037 and 337.902539 ms/frame, a 1.963210% spread against the original 1% gate. The coordinator stops before any A/B comparison launches. No speedup or regression is accepted, and all shipping packing changes are restored before the second hypothesis. Initial unqualified binaries, a vacuous standalone negative control and the corrected controls remain archived rather than being replaced by successful artifacts.

The second hypothesis replaces the narrow-cap CDF correction with a fifth-order atan expansion only in built-in desktop Vulkan shading when `c>=0.95`; wider caps, zero-cap limits, mobile and custom shading/output retain the original fallback. Its real-arithmetic correction remainder is below `2.087e-11`, including the actual FP32 threshold representation. Separate actual-source host coverage performs 13,963,303 checks, ten negative controls and five compiler-argument fixtures; maximum sampled FP32 CDF error is `1.1920928955e-7`. Transported directions can differ, including approximately `1.499e-5` maximum vector distance near the threshold, so this is not bit identity. Independent geometry coverage performs 46,489,109 checks with six negative controls. Actual Intel assembly confirms all 15 CDF sites in the inspected colored-penumbra executable branch around the general atan path without flattening both arms; the fine marcher remains data-flow equivalent, while observed register pressure, static spill/fill counts, SEND and loop counts stay unchanged. The dominant colored-penumbra variant retains maximum live 265 and static four-store/24-fill counts. No runtime benefit follows from that code mechanism alone.

Native baseline/candidate horizon and independent-blocker controls cover 0.25°, 3°, both sides of the narrow-cap threshold and 20°, retaining the original thresholds. A deliberately zeroed fast correction fails six partial-horizon controls while wide-cap and independent front-blocker controls remain unchanged. Sixteen complete RGBA32F image pairs are exact; the planar dense fixture is not the sole grazing oracle. Fresh material/village/roof/stone A1/B/A2 images pass the frozen whole-image, local-tile and connected-component policy. Root inspects all four previews and thirteen native-resolution crop panels. Material pixels are exact and roof/stone differences stay within two RGB8 levels. An unexplained isolated village peak at `(1660,852)` reaches 39/36 RGB8 levels against the two baseline endpoints; largest novel components are six and ten pixels. This supports scoped visual parity, not every-camera or universal pixel identity. Candidate renderer `a594a862f03b8cf98fda537dc066cb51068b9ff1659b9f729636d70c4203e197`, smoke `a1094d22c35e0e69cba4a9b3354eeb144bf9bda0d59411753812cf1286897c8d` and matc `745c26ca9191f2f5b20f9d532191ddc1c2d4edd7645a985cdc573686c86b397b` bind those checks. Its complete Vulkan suite passes 58 cases with six skips; OpenGL passes 56 with eight skips. The first OpenGL attempt is externally signalled and incomplete, with an unknown signal sender; its exact-binary rerun completes the prerequisite. Both attempts remain charged and preserved.

The final prospective protocol is cohort `d823e39e-c4c6-44ce-9634-0821d33d977e`, protocol SHA-256 `5ebed248bb6d003d839161350049095605428a7a90519709f9c3e6f7ad36e6c2`, with 1,875 direct provenance files and fresh A/B cache priming. Recording, shader diagnostics, validation, readback, overlays, frame generation, VRS, dynamic resolution, HDR and music stay off during timing. The run begins only after actual-host checks confirm no recognized media/build/native competitor; ordinary desktop activity is not exclusive GPU isolation. Each completed capture targets 260 runtime seconds; the metric is the unchanged shortest final whole-interval suffix reaching 180 seconds after at least 75 preceding seconds. Fresh pilots measure 341.830150 and 341.844789 ms/frame, a passing 0.004282343% spread. The first quartet then produces:

| Comparison window | Mean ms/frame | Measured frame intervals |
|---|---:|---:|
| A1 retained helper baseline | 372.607404 | 484 |
| B1 CDF candidate | 344.774480 | 523 |
| B2 CDF candidate | 344.775254 | 523 |
| A2 retained helper baseline | 341.822777 | 527 |

The two measured baselines span 8.617952%, failing the unchanged 1% repeatability requirement. Additional baseline means cannot repair this cohort: `(max-min)/max` already provides a conservative 8.261947% lower bound on the eventual range divided by its arithmetic mean; the exact best four-baseline completion is still 8.436196%. Root therefore rejects on this necessary validity failure and stops the remaining sequence. This is an explicit deviation from completing the nominal twelve-launch sequence, whose frozen coordinator only encoded a nonpositive first-quartet gain as its early-stop condition. Only four comparison windows complete; A3 is deliberately cancelled and drained after 94.010372 native seconds, and B3/B4/A4 never launch. The early cancellation qualifies no partial speedup or regression and is not a complete eight-window result. A1 remains in every primary calculation; no alternative suffix, outlier exclusion, third hypothesis or favorable timing retry is used.

A1 is persistently slower for most of its capture and recovers late; its maximum raw frame is 443.047 ms, rather than one large stall explaining the difference. Existing 30-second diagnostic bins stay around 378–382 ms early and approach 342 ms late. Last-completed GPU durations move in the same direction, while fixed-window CPU submission means are 2.680 and 2.762 ms for A1/A2. The central 98 clock observations in each run all report 2800 MHz; package temperatures are 66–68/66–69°C and VRAM 66–70°C in both. Recognized process scans and kernel logs report no competitor or GPU fault. These sparse observations do not identify a cause, exclude transient effects, align individual GPU/host samples or attribute the difference to the contact pass. The unexplained baseline variation remains work for a separate investigation.

Canonical `scripts/build.sh --materials-all --jobs 8 --target heaven_benchmark --target heaven_benchmark_smoke`, with the executable repository-local `TMPDIR`, restores all 1,257 original inputs before these documentation updates. Renderer `ee1d2fc7e75a8c7ee579a0b24dec4c793fbc8c9e26c3af22f63ffad52c9580dc`, smoke `efe6fb367923bd4af703633bb5e2b191cba77b11ac8a44077c72e17b88a727fe`, matc `dc54cfa1759ca2152fce19c44656514ac7c2773733738ada62574c70c55fbf0f`, all 49 production packages, embedded arrays and generated test packets match exactly. Host CTest passes 55 cases with one expected native skip and zero failures. No new GPU suite is claimed after this byte-exact restoration. The distributed patch/fetch/tree pins remain unchanged. Raw vendor index bytes differ despite unchanged HEAD, staged contents and status; the cause is unproven and no metadata repair is performed.

The complete task charges 39 Heaven launches and 13 fixture launches, including failed, priming and deliberately cancelled work, totaling 5,080.179230 native wall seconds within the prospectively amended 7,000-second cap. No further EU sampling, driver/clock/power/governor/kernel/security changes, commits or releases occur. Evidence remains under ignored `build/intel-register-pressure/20261116-contact-next/`: start with `review/cdf-cohort02-rejection01/REPORT-02.md`, the typed `benchmark/cohort-02/store/entries/early-baseline-rejection`, `review/cdf-cohort02-early-rejection-independent01.json`, and `final-restoration/receipt-03.json`. Original request fields that call the conservative bound a minimum are preserved and explicitly corrected in the independent audit; initial setup, reader-limit and raw-index assertion failures also remain archived.

## Intel observer state and variance, November 17, 2026

A new bounded investigation leaves all rendering settings, shaders and geometry unchanged. Three 260-second diagnostic baseline captures measure 344.059798, 339.531695 and 329.005940 ms/frame under the original 75-second minimum prefix/final 180-second suffix rule. New read-only DRM observations expose clients absent from the old build/media-name check, but are explicitly incomplete for permission-denied processes and the bounded error inventory. The user confirms an idle desktop. The original sustained 380-ms phase does not recur; no background application is blamed. Resident benchmark VRAM is about 7.6 GiB on a reported 31.9-GiB local heap, which does not support capacity exhaustion in these captures. Installed `xpu-smi` exposes actual power and throttle reasons but not usable memory-clock/bandwidth results; unavailable metrics remain unknown. No EU sampling or device-setting change occurs.

The third diagnostic is the only one with continuous read-only Intel power polling, motivating a new prospective Off/On/On/Off control. The four fresh baseline means are 338.825627, 329.249006, 328.881376 and 339.017838 ms. Off repeats within 0.056712%; On repeats within 0.111719%. Pooled means are 338.921642 ms without polling and 329.065023 ms with it, a reversible 2.908229% timing difference. This establishes an observer effect on this host, not its driver-internal mechanism or the sole cause of historical A1. Read-only observation is therefore a declared measurement condition, not an assumed passive operation. There is no justified universal FPS improvement or change to ordinary benchmark scoring.

A fresh protocol pins the tool executable SHA-256 `23c4844d587e16125b4ad747d259f76bb8243d42aabd676475d9a23f3246a16c`, query fields and two-second interval identically for both archived renderer binaries. Every window requires a live observer through completion, validated CSV continuity, unchanged tool identity and native drain. Fresh priming precedes two pilots; all original 260/75/180-second timing and 1% repeatability gates remain. The pilots measure 327.600812 and 327.623448 ms (0.006909% spread). The first quartet measures:

| Window | Mean ms/frame |
|---|---:|
| A1 retained helper baseline | 328.836215 |
| B1 CDF candidate | 331.798600 |
| B2 CDF candidate | 330.499607 |
| A2 retained helper baseline | 329.169217 |

The measured baseline pair spans 0.101216%, satisfying the original 1% gate. Pooled baseline/candidate means are 329.002564/331.147909 ms; the candidate costs 0.652076% more frame time in this quartet. The prospective nonpositive-gain rule stops the study before a second quartet. This supports rejecting the candidate, not a complete eight-window acceptance or general regression claim. High's 16 Sun rays and 32 depth steps, the full Heaven scene, MSAA4, foliage, displacement and the requested texture configuration remain unchanged. The production renderer remains byte-exact `ee1d2fc7`; no Filament change is retained. The larger historical slowdown/recovery remains only partly explained despite the successful control of observer-state bias.

`scripts/profile_power_window.py COHORT LABEL` makes the declared observer state reproducible for future Linux comparison controllers. A protocol opts in through `power_observer.path` and its complete `file_identity` record, matching the installed `xpu-smi` executable. It seals a separate typed observer record linked to the actual native window. Analysis rejects declared observer runs without matching qualification. Default comparisons and ordinary application behavior do not enable polling. Eleven new CPU cases cover missing/foreign typed qualification, complete window linkage, coverage truncation, gaps, replay, unavailable/nonfinite metrics, bounds and midnight rollover; all 110 profiling-harness tests pass. The frozen native study used the archived prototype; the subsequent reusable integration has CPU validation and checks against its captured CSV data, not a newly claimed native rerun. The observer's driver libraries and transient GPU clock states are not a universal hardware qualification.

Evidence is under ignored `build/intel-register-pressure/20261117-variance/`, including both prospective plans, all seven initial/control diagnostics, `comparison-summary.json`, raw observer logs and the recursively verified `comparison/cohort-01/store/entries/observer-controlled-rejection`. Fifteen new Heaven launches use 4,550.131362 native wall seconds within the new investigation's 7,000-second total cap. The earlier rejected experiment stays closed; no old timing is reused as a passing result. No driver, clock, power limit, security, version or release changes occur.

## Monitored Intel opaque-depth optimization, November 17, 2026

The fresh monitored full-Heaven trace identifies main opaque GPU shading as the dominant observed cost, rather than application CPU computation: 28 completed main opaque intervals average 228.184 ms, with 14.538 million fragment invocations and 54.524 million input-assembly primitives. SSR averages 17.951 ms, auxiliary color 14.321 ms, structure 14.155 ms and each shadow pass about 13.319 ms. These are separate retained GPU intervals; do not add overlapping ranges or combine them with CPU time. CPU history is incomplete, but retained camera/scene-sync/feature-update means are 0.0013/0.0426/0.0608 ms and render submission 4.609 ms, while rejected-frame waits dominate the retained main loop. Fragment invocations per output pixel include helpers, MSAA, guard bands and microtriangles and are not a recoverable-overdraw factor.

C1 explicitly requested early fragment tests for eligible ordinary opaque shaders. Its bounded mechanism screen leaves opaque fragment invocations essentially unchanged and provides no meaningful timing signal. It is rejected and its rendering inputs are restored before C2. C2 instead populates the main view's same-sample depth attachment before expensive opaque color shading. It replays the finalized original COLOR commands, vertex module, specialization constants, descriptors, attributes, transforms, skinning and instance data with the fragment stage suppressed. The ordinary color commands and inclusive GE depth test retain their order. High remains 16 Sun directions and 32 fine steps; ray math, hit tests, distance, geometry, displacement, textures and visual settings remain unchanged.

The intermediate b8bc178b diagnostic observes zero fragment invocations in the 11.128 ms opaque Z prepass, then 12.317 million opaque fragment invocations, 15.278% below the fresh baseline's 14.538 million. Its opaque interval averages 188.955 ms. Different diagnostic durations, instrumentation overhead and thermal histories prevent treating these pass times as clean throughput acceptance. Subsequent admission-only changes are independently reviewed against all 49 production material packages; the final e71e6f54 build has separate native/image qualification and the clean comparison below. The final build is not relabelled as the earlier full-Heaven diagnostic, and exact native shader-machine-code equality is not claimed.

The default-off Filament View option is requested by HeavenBench only for Vulkan Intel 8086:e223 with Mesa 26.0.8-1ubuntu0.3, enabled High contact shadows and a finite above-horizon Sun brighter than the Moon threshold. Settings and lighting eligibility are re-evaluated each frame. The conservative whole-view/command/shader gates reject unsupported sample/target/view state, stereo, clipping capability, side effects, custom commands, stencil, coarse VRS, colorless depth writers and TWO_PASSES_ONE_SIDE's forced-depth command. Unknown or unsupported cases retain the ordinary path. A separate broad fragment-safety proof also covers visible materials that do not enter the depth replay. The application does not lower a quality setting to make the prepass eligible.

Position invariance is required for cross-pipeline depth replay. All 49 packages are deliberately rebuilt through the canonical materials-all helper. Across 2,012 actual SPIR-V programs, only 248 ordinary desktop Vulkan COLOR vertex modules gain one Invariant annotation on their actual Position output; removing that annotation produces the exact prior words, IDs, ordering and arithmetic. The other 1,764 SPIR-V programs and all 2,012 OpenGL programs are byte-identical. No production fragment program changes relative to this task's monitored baseline. The retained portable shader classifier contract executes 78 checks and four compiled falsifiers, including semantically valid SPIR-V fixtures. This bounded admission classifier is not a replacement SPIR-V validator.

Final native suites bind smoke 04ac17d0: Vulkan 59 passes/five disclosed skips and OpenGL 55 passes/nine disclosed skips, with zero failures. Same-engine Off/On/Off-return FLOAT controls are bit-exact at 1×/4× MSAA and cover actual zero-fragment execution, coplanar order, nominal one-ULP competition, displacement, frozen parent carriers, independent static references for skinning/InstanceBuffer, mirrored/constant-W transforms, resize, refraction and fallback state. Four final e71e6f54 A/B/A image triplets pass unchanged numeric and separate AI visual checks: material pixels are exact; village mean RGB8 differences 0.473512/0.473095 match baseline-repeat 0.473000; roof/stone remain around 0.4663 with maximum channel difference 2. These are static SDR controls, not human, moving-sequence, full-tour, night, HDR or other-platform acceptance. The quality coordinator's exit 143 of unknown cause is preserved; seven completed captures are independently closed and the sole unstarted final stone baseline is captured once separately.

The clean fixed-village comparison uses the full Heaven scene at 1920×1080, Visual Ultra, generated 4K textures, MSAA 4 and High contact shadows, with ordinary animation. The existing fixture disables DOF/cloud deck and has dry noon weather; it does not claim every possible effect is active. New 260-second A/B priming and two fresh baseline pilots precede ABBAABBA. Each measured window uses the shortest final whole-frame suffix reaching 180 seconds after at least 75 excluded seconds. There are no screenshots, microprofiles, GPU executable diagnostics, overlays, dynamic resolution, frame generation, VRS, music or external xpu-smi/LevelZero polling during timing. The application sysfs worker up to 1 Hz and controller sysfs observations around 0.5 Hz remain equal in every condition. The pilots measure 316.927857/316.914585 ms, a 0.004188% range.

| Window | Mean ms/frame | p95 ms/frame |
|---|---:|---:|
| A1 | 316.898553 | 322.466522 |
| B1 | 305.087095 | 311.916888 |
| B2 | 305.082403 | 311.216116 |
| A2 | 316.720604 | 321.409315 |
| A3 | 316.654406 | 320.954412 |
| B3 | 304.997929 | 311.172575 |
| B4 | 305.003238 | 311.398566 |
| A4 | 316.799551 | 321.951717 |

Pooled mean frame time falls from 316.768279 to 305.042647 ms, a 3.701643% reduction (3.156882 to 3.278230 informational FPS). The two quartets improve 3.700908% and 3.702367%. Baseline range 0.077074%, candidate range 0.029231% and first/last baseline drift 0.031254% are all below the gain. Both positive-quartet requirements, the 1% baseline cap and task-specific strictly-greater-than 1% gain gate pass. The p95 values above are individual-window percentiles, not a pooled percentile. This is a modest measured improvement at one full-scene camera on this Intel/Mesa configuration; expensive visible High contact shading remains the main target.

Independent readback of every original selected interval reproduces the pooled means and additionally gives pooled p95 321.837366 → 311.427534 ms and p99 325.130314 → 312.842101 ms. The final audit verifies the task decision's 799-object explicit typed closure (1,164,524,983 bytes), all 12 native identities/settings/monitoring/drain/kernel records and 73 unchanged runtime inputs. All 37 recorded native/controller/coordinator actors are absent. Including all failed attempts and fixtures, the task charges 30 Heaven launches, nine fixtures and 4,930.750571 native wall seconds within its original caps. Final patch publication changes only the already-tested distribution and its two pins; all 25 bootstrap CPU regression fixtures pass.

All eight final application sensor snapshots report 2800 MHz and throttle reason None, GPU 67–68°C, VRAM 66–70°C and fan 1919–2100 RPM. Baseline final power observations are 176.49–178.67 W and candidate 181.90–183.83 W against the exposed 275 W limit. These are latest host observations, not frame-aligned measurements or whole-window power averages; the original profiles retain full sampled histories. Device VRAM usage, memory clock and utilization remain unavailable. The data does not identify thermal throttling as the source of the measured shading cost. No driver, clock, power, fan or security setting changes occur.

Retained renderer SHA-256 is e71e6f543661e7a5e002ec77864c69b985b31b774da0b5dfacb997f32ddc876a versus monitored baseline ac6238174f2d72a7d5f437942a3f79bb959e0f5fc26f62f68a8c9777846f8f85. The complete Filament patch is 5b389f92dbd7c5f35ce91eeded77bda3e6377dd5e1f30a95bd76ce4c045634a0, reconstructing tree a6dd320c1598c1113817c1bf3f42019114e42971 against unchanged public v1.80.1. Independent private-index reconstructions and fresh/repeated offline dependency bootstraps pass. The canonical final build, 57 host checks plus one native opt-in skip across the recorded full run/corrected executable-TMPDIR retries/new classifier test, and focused three-TU static analysis pass; this is not a fresh complete repository-wide analysis or cross-platform build.

Evidence stays under ignored build/intel-register-pressure/20261117-monitored-optimization/: the prospective plan, all rejected C1/C2 prototype and failed transport artifacts, source/compiled-material proofs, exact native/visual receipts and benchmark/cohort-01/store. Its recursively verified analysis record is c4c963dcca35427c3800772278eab078a4152619992744b0533cebb5c703ac6e; the protocol UUID is ae940f8e-7683-4cef-86e0-ca8d009033c3. No historical timing is imported, failed run discarded, result retried, other GPU qualified, version bumped or release published.

## Validation

The renderer is validated with:

```bash
./scripts/build.sh
ctest --test-dir build/renderer -R heaven_benchmark_smoke --output-on-failure
ctest --test-dir build/renderer --output-on-failure
```

At the time of this work, the renderer build and smoke test pass. The startup
loading screen is a renderer-owned bundled asset and does not depend on the
extracted Heaven data tree.
