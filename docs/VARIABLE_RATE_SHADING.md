# Variable-rate shading

HeavenBench implements optional pipeline variable-rate shading (VRS) for compatible Vulkan GPUs. It defaults to **Off**. The explicit `2x1` and `2x2` quality settings reduce fragment shading frequency for eligible opaque lit geometry in the real Heaven scene, trading fine shading detail for potentially lower GPU cost. This standard Vulkan path is available according to device capabilities rather than GPU vendor branding; Intel hardware is the initial measurement target.

VRS does not change rendering resolution, reconstruct a higher-resolution image or generate intermediate frames. It is separate from XeSS, FSR upscaling, XMX matrix diagnostics and frame generation. No additional vendor SDK is required for this integration.

## Controls

Open **Quality → Image quality → Variable-rate shading** and select a mode. Changes apply live. The control shows the effective rate, the capability or fallback reason, and `2x1`/`2x2` support at the current sample count. Changing this quality setting cancels an active benchmark tour before applying the new workload, as other ordinary live quality changes do.

| Mode | Requested shading footprint | Quality implication |
| --- | --- | --- |
| `off` | Full-rate shading | Default reference for image and performance comparisons. |
| `2x1` | Two horizontal pixels per coarse footprint | Fine horizontal material and lighting variation can lose detail. |
| `2x2` | Two pixels in both dimensions per coarse footprint | Larger reduction in shading detail; inspect small highlights, normal maps and moving surfaces. |

Use `--vrs off`, `--vrs 2x1` or `--vrs 2x2`; the persisted configuration key is `variable_rate_shading` with the same canonical string values. Invalid values emit a diagnostic and retain the previous valid request. Presets do not select a VRS mode, so state it explicitly when comparing captures or performance runs.

```sh
./scripts/run.sh --gpu 8086:e223 --data assets \
  --preset visual-ultra --texture-quality 4k --vrs 2x1
```

The GPU ID above identifies the current Intel test adapter; use the selector for the actual device on another system. An available setting is not a performance guarantee. Check the effective mode and fallback reason after changing MSAA or the renderer.

## Eligible work and full-rate controls

VRS applies only to opaque, lit `SURFACE` material draws in the main color pass. Skinning, instancing and vertex displacement remain eligible: VRS changes fragment shading frequency without reducing their vertex processing, triangle count, instance count or draw commands. Expensive material operations within an eligible fragment shader share that draw's shading rate; VRS does not expose them as separately timed GPU passes.

Masked materials, alpha-to-coverage, blended materials, unlit materials, shadow-map passes, auxiliary depth passes, SSR passes, post-processing and UI stay at full rate. Grass and foliage cutouts therefore retain their coverage-sensitive shading. The backend checks actual compiled fragment declarations against the GPU's depth/stencil and shader-sample-mask properties; unsupported declarations retain full rate. Per-sample shading, interlock and tile-image shaders conservatively retain full rate. Inline lighting and shadow visibility inside an eligible main-color material follow that material's coarse shading rate; the full-rate shadow-map pass does not imply that every lighting calculation remains full rate.

Scenes limited by CPU work, geometry, memory traffic, shadow maps or other unaffected passes may show little benefit. Scene-wide GPU cost also includes the full-rate routes above. There is no universal FPS target or expected percentage gain, and a coarse-mode speedup cannot be described as a geometry or culling optimization.

## Capability and fallback

The Vulkan platform explicitly opts into `VK_KHR_fragment_shading_rate` and the optional `pipelineFragmentShadingRate` feature when the selected physical device and logical-device creation path support them. Extension advertisement alone is insufficient. The integration records physical rate/sample-count masks separately from logical enablement, and checks the actual render-target sample count again when selecting a native pipeline rate.

The current sample count determines whether `2x1` and `2x2` are supported. A rate available at one sample count need not be available at another. Unsupported requests retain their requested identity while the effective mode becomes `off`, with an explicit reason such as unavailable logical-device enablement, a non-Vulkan backend or an unsupported MSAA combination. VRS does not reduce MSAA or silently change another quality setting to make a request available. The ordinary platform default remains disabled; HeavenBench enables the optional logical capability while keeping the application's requested shading rate Off.

`View::setFragmentShadingRate` carries the request into Filament's main-color render-pass construction. Eligible commands choose a static Vulkan pipeline variant; full-rate commands omit the coarse-rate extension state. Coverage-sensitive shader and render-state guards remain active in the Vulkan driver. This uses per-pipeline rates without a shading-rate image, a foveation controller or application camera-dependent rate regions.

## Results and comparisons

Ordinary VRS runs retain the normal benchmark score policy. Raw result schema 2 and score version 3 remain unchanged. Saved configuration exports the requested `variable_rate_shading` ID, while the completed result snapshots requested/effective rates, current-sample support flags and the capability reason. JSON and HTML also describe eligible work, the quality tradeoff and the comparison policy. Later runtime changes do not rewrite a completed result's VRS snapshot.

Compare scored results only with matching hardware, driver, API, resolution, application/score version and effective settings, including both requested/effective VRS modes and MSAA. A requested coarse mode that fell back to Off is not an executed coarse-mode measurement. Separate experimental mode comparisons may deliberately change VRS while holding the other conditions fixed, but must identify that quality change explicitly.

## Native validation and actual-scene methodology

The renderer-linked `variable_rate_shading` smoke case uses test-only packages under `benchmark/tests/materials/`; production material packages and embedded material bytes are not regenerated for the fixture. A large triangle displays a one-pixel red/green fragment-coordinate checker and a separate four-phase coordinate pattern. The latter independently distinguishes two-pixel groups because a one-pixel checker alone aliases to constant color at even coarse sample intervals. Interior pixel checks exclude primitive edges.

The fixture exercises requested/effective RHI status, the production View setter, actual `2x1` and `2x2` raster grouping, full-rate masked/alpha-to-coverage and unlit controls, unsupported-rate fallback and Off-return image parity at 1× and 4× MSAA where supported. Missing coarse capability yields a disclosed skip after full-rate/fallback checks; it cannot establish active VRS acceptance. `HEAVEN_SMOKE_VRS_OUTPUT_DIR` retains baseline and comparison PPM images. Use the existing gated smoke harness with `HEAVEN_SMOKE=1` and `HEAVEN_SMOKE_CASE=variable_rate_shading`; a real display and the helper-built smoke executable are required.

The initial actual-scene cohort targets verified Intel Vulkan at 1920×1080, Visual Ultra, 4× MSAA, enhanced 4K textures, fixed rendering resolution and frame generation Off. Use the full Heaven scene, identical camera/FOV/time/settings and the same asset/binary identities. Run Off → `2x1` → `2x2` → `2x2` → `2x1` → Off, retaining each run separately. The 65-second wall-time profile excludes at least two warmup seconds and must retain at least sixty measured seconds; screenshot readback, validation, Microprofiler recording, visible overlays and competing rendering/build work remain outside these performance windows.

```sh
mkdir -p build/vrs/manual
./scripts/run.sh --gpu 8086:e223 --data assets \
  --preset visual-ultra --texture-quality 4k --width 1920 --height 1080 \
  --frame-generation off --no-dynamic-resolution --no-music --vrs off \
  --profile build/vrs/manual/off.json --profile-seconds 65
```

Use a new output for every run and an explicitly fixed camera from a maintained fixture; the example only shows the profile interface. Inspect actual drawable dimensions, selected GPU, texture selection, MSAA and effective VRS in the resulting metadata rather than relying on requested flags. The ignored initial controller and source/binary-bound plan live under `build/vrs/20261110/validation-plan/`; its performance and capture phases are separate, and native correctness is a prerequisite for executing the scene cohort.

Measure mean frame time/FPS and frame-time percentiles from the retained raw intervals, disclose repeated-run variation, and keep asynchronous whole-frame GPU samples separate from CPU time and independently completed GPU-pass distributions. A last-completed GPU sample may lag or repeat. Use separate diagnostic traces to confirm submitted native rates and affected passes; tracing overhead does not belong in the clean performance cohort.

Fixed-step image controls compare Off/`2x1`/`2x2` at identical accepted-frame animation times after 90 warmup frames. Inspect village materials, grazing walls, roof displacement, night lamps and a short grass/camera-motion sequence for lost shading detail, unstable highlights, changed cutout coverage, broken geometry, missing textures and unexpected temporal artifacts. Keep the pre-change Off baseline, post-change Off return and coarse-mode differences. Fixed-step images are quality evidence, not measured playback FPS.

## Actual Heaven performance

### Intel-fix rerun, November 10

The new matched cohort uses the validated Intel compatibility executable, SHA-256 `4aff61b11df20e51c35704917368a7f224d8d8df57e87be9bde6db8b173d62c3`, with the targeted image-allocation workaround active and no Intel/ANV debug overrides. The selected device is Intel BMG G31 (`8086:e223`), Mesa `26.0.8-1ubuntu0.3`, Linux `7.2.6-070206-generic`, native Wayland and Vulkan. The full Heaven village uses the same fixed noon camera, actual 1920×1080, Visual Ultra, enhanced 4K textures and 4× MSAA. Frame generation, HDR, dynamic resolution, recording, validation, screenshot readback, music and visible overlays remain off.

All six profiles complete in Off → 2×1 → 2×2 → 2×2 → 2×1 → Off order, retaining 61.44–62.11 measured seconds per run after at least two warmup seconds. The values below are independently recomputed from raw float32 frame intervals. Pooled FPS is total measured frames divided by total measured time; mean frame time is its reciprocal in milliseconds. Each mode has two runs.

| Mode | Pooled FPS | Mean frame time | FPS change from Off | Individual-run FPS range | Pooled p95 / p99 frame time |
| --- | ---: | ---: | ---: | ---: | ---: |
| Off | 2.8924 | 345.74 ms | Reference | 2.8384–2.9462 | 373.68 / 392.46 ms |
| 2×1 | 2.8453 | 351.46 ms | −1.63% | 2.8212–2.8693 | 377.36 / 398.71 ms |
| 2×2 | 2.7550 | 362.97 ms | −4.75% | 2.6787–2.8320 | 400.32 / 410.52 ms |

There is no demonstrated whole-scene VRS speedup with the Intel fix enabled. Keep Off as the default for this workload. The closing Off run is 3.66% slower in FPS than the opening Off run; the two-run FPS ranges span 3.73% for Off, 1.69% for 2×1 and 5.56% for 2×2 relative to each mode's mean run FPS. These are descriptive ranges, not confidence intervals. Two repeats, ordinary wall-time animation and control drift limit attribution of the apparent slowdowns to VRS alone. This cohort does not isolate pass costs, fragment invocations, an exact CPU/GPU bottleneck or performance on other views and presets. Earlier instrumented pass observations below belong to the pre-workaround binary.

Every run verifies the requested and effective VRS mode, the exact Intel driver, the once-only compatibility diagnostic and matching effective settings. Binary, source, asset and seed identities remain unchanged; scene, tessellation, grass, foliage, texture and emitter totals match across all six runs. There are no detected competing renderer/build/analysis processes, kernel GPU reset/fault messages or application failure findings in the checked intervals. The process scan samples once per second and cannot exclude short or unclassified desktop workloads. Read-only endpoint temperatures rise from 51°C package / 52°C VRAM before the opening run to 63°C / 66°C after the final run; these do not prove sustained clock behavior or absence of throttling.

Each run receives an identical private warm tessellation/prepared-geometry seed and an empty private shader cache. Loading lies outside the profile window, but two warmup seconds do not prove that all deferred compilation has finished. No loading-time claim is made. The reproducible controller, frozen executable and six individual profiles are retained under ignored `build/vrs/20261110/intel-fixed/`; `performance-01/` contains commands, raw intervals, effective configurations, per-run receipts and before/after identities. `analysis/analysis.json`, `analysis/analysis.md` and `analysis/frame-time.svg` retain independent pooled/per-run statistics, percentiles and a descriptive plot. `verification.json` records the separate receipt, geometry and kernel checks. Accepted presentation intervals measure renderer throughput rather than scanned display cadence; asynchronous GPU observations may lag or repeat and are not added to CPU timings.

### Historical cohort before the Intel fix

Six completed 65-second profiles used Off → 2×1 → 2×2 → 2×2 → 2×1 → Off, with the full village, Visual Ultra, enhanced 4K assets, actual 1920×1080, 4× MSAA and frame generation/dynamic resolution/HDR off. Each retained 61.71–62.13 measured seconds after warmup. Recording, validation, screenshot readback and visible overlays were disabled. Binary/assets/settings identities remained unchanged; raw intervals and recomputed statistics are in `build/vrs/20261110/scene-performance-01/`.

| Mode | Mean FPS across two runs | Individual-run FPS range | FPS change from Off |
| --- | ---: | ---: | ---: |
| Off | 2.8866 | 2.8438–2.9294 | Reference |
| 2×1 | 2.8573 | 2.8407–2.8739 | −1.01% |
| 2×2 | 2.8517 | 2.8354–2.8681 | −1.21% |

VRS did not improve performance in this cohort. The differences are smaller than the roughly 3% spread between the two Off runs, while the reduced rates visibly soften fine shading detail. Keep Off for this tested workload unless further measurements justify the quality trade-off. This does not predict results on other cameras, presets or hardware.

A subsequent matched control with the preserved pre-VRS executable measured 2.8156 FPS over 61.44 measured seconds. It does not suggest a default-Off regression, but one ordered control cannot establish precise overhead or a speedup; driver, thermal, cache and order effects remain possible. Its executable identity is retained separately, without assigning the current checkout's source identity to that old binary (`old-performance-control/run-01/`).

Separate four-second diagnostic recordings verify coarse commands in the real opaque-color pass: 1,385 coarse and 332 full-rate native commands per observed invocation. Each observed invocation submits 54,523,679 primitive instances and 1,778 draw-instance invocations; VRS does not reduce those counts. Submitted primitives include off-screen/overlapping work and are not visible pixels or fragment invocations. Every observed shadow, auxiliary-depth, structure-depth, SSR, transparent-color, composite/post-process and UI command remains full-rate. Counts describe bound native command rates, not a feature's isolated GPU cost.

The opaque-color pass is the largest observed GPU interval: its diagnostic means are 264.59 ms Off, 238.64 ms at 2×1 and 237.74 ms at 2×2, with only 3, 8 and 11 timed observations respectively. The Off diagnostic's shadow intervals average 56.45 ms, SSR 18.79 ms, structure depth 15.25 ms and auxiliary depth 15.22 ms. These are separate frame-relative interval unions; do not add them or substitute these short instrumented recordings for the clean FPS cohort. The recorder overwrote 22,286/68,812/78,210 events, dropped zero, and reports stale-epoch events, so coverage is explicitly incomplete. Paired offline JSON/HTML and per-invocation view-role analysis are retained under `scene-diagnostics-01/` and `scene-diagnostics-02/`.

The measurements establish active VRS without a useful whole-scene gain. They do not isolate why: native command counts cannot prove fragment-invocation savings or a vertex bottleneck. Matched tessellation-density and contact-shadow shader-cost experiments are the next useful checks; masked shading and the unchanged shadow/depth/SSR work also remain relevant.

## Acceptance record

The original November 10, 2026 checks use Intel BMG G31 (`8086:e223`), Mesa 26.0.8 and Vulkan. Source, executable, asset and command identities are retained under ignored `build/vrs/20261110/`. That historical capture/measurement executable has SHA-256 `ce5918e0dfa39accaaf031623d56f25b2ad4b2534fd4af65b0ad57b72878bf3f`. The Intel-fix rerun above has its own executable identity and matched baseline. These are bounded local checks rather than cross-platform qualification.

| Check | Status / evidence |
| --- | --- |
| Host mode/config/CLI/catalog/report contracts | All 42 SDK-enabled and 36 standard-build host CTest cases pass, including the property-aware fragment-policy matrix. Scoped LLVM 21.1.8 analysis passes all 28 changed/control translation units; 18 gate tests and nine coverage tests pass. This is not a full repository static audit. |
| Native opaque lit coarse pixel oracle at 1×/4× MSAA | Final `native-oracle-11/` and `native-oracle-12/` pass in SDK-enabled and standard builds. Each covers 12 mode/sample/material combinations and 36 exact coordinate grouping checks, with zero Vulkan validation errors/warnings. |
| Masked/A2C/unlit full-rate and Off-return controls | Native full-rate controls and Off return pass; all 36 saved images are byte-identical between the two builds. No engine reset/timeout occurred during those final native runs. |
| Pre-change Off versus new Off scene images | Material-scene control is byte-identical. Full-Heaven old/new Off RGB mean absolute difference is 0.473424 bytes versus 0.473329 for the new Off repeat; both have pixel-maximum difference p95=1 and p99=2. Existing metadata, geometry summaries and all 502 tessellation-cache entries match. The full scene is not byte-identical. |
| Full-Heaven repeated performance cohort | All six matching 65-second profiles complete with independently recomputed raw-frame statistics and unchanged input identities; see the measured table above. |
| Real-scene affected-pass/rate evidence | Short paired JSON/HTML traces verify observed main opaque coarse commands and zero coarse commands in the protected categories. View reuse is mapped per invocation; history loss and incomplete coverage are explicit. |
| Existing OpenGL regression suite and fallback | 54 passes, six disclosed skips, zero failures on Intel. The VRS case passes its full-rate/fallback controls and then skips coarse acceptance because OpenGL does not implement this Vulkan feature. |
| Coarse-mode scene image and motion review | Four matched village captures pass input/identity checks. Native-sized roof, stone, grass, distant-building and metal crops show softer/blockier shading as rates increase, without obvious new holes or cracks in those still crops. RGB mean absolute differences from Off are 2.5567 bytes at 2×1 and 3.7745 at 2×2. Motion quality and other cameras/weather remain unqualified. |
| Other GPUs, platforms and drivers | Not established by the initial Intel Linux cohort. |

The existing [Intel GPU-lab matrix measurements](INTEL_GPU_FEATURES.md#measured-matrix-workload) remain a separate compute result. Their kernel throughput ratio does not predict VRS scene benefit. The [Filament patch notes](../patches/filament/README.md) retain vendor modifications and reproducible dependency pins. The retained source tree for this original VRS cohort is `841aed799c759833009baccd4514085ae519db81`. A four-comment contract clarification after capture preserves line counts and all functional code; canonical rebuilds reproduce the exact measured renderer and both validated native smoke executables byte for byte. Production material sources/packages and embedded material bytes remain unchanged.

Earlier native diagnostics included startup timeouts, an Intel engine reset and a glass FLOAT-readback failure. Their logs and termination receipts remain alongside the passing runs; their cause has not been assigned to VRS. The final native/scene/OpenGL kernel-review interval contains no new GPU reset, timeout or fault; one deletion of the earlier coredump is recorded as housekeeping. These checks do not establish broad Intel Vulkan driver or full-suite stability. The lit pixel oracle uses zero diffuse/specular test BRDFs to isolate its emissive coordinate pattern, with unchanged grouping/color thresholds; float-target diagnostics were removed from the test.

## Intel startup/readback follow-up

The subsequent [Intel Vulkan compatibility investigation](INTEL_VULKAN_COMPATIBILITY.md) isolates the startup resets and stale HDR uploads to the compressed-image path on the installed Intel BMG G31/Mesa 26.0.8/xe stack. A narrowly gated alias-capable image policy now passes the complete native Vulkan suite without driver debug flags, together with independent readback, staging, error-handling and IBL-LOD corrections. This does not assign the fault to VRS. The original performance, image and pass-diagnostic records retain their historical source/binary identities; the [Intel-fix rerun](#intel-fix-rerun-november-10) measures the new candidate separately and likewise establishes no useful VRS speedup in this view.
