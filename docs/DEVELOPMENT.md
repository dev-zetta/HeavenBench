# Development and measurement

HeavenBench is a C++23 benchmark application built on Google Filament. The renderer, measurement code, supporting asset tools and test scenes share CPU-side loaders so benchmark, packaging and inspection paths use the same asset interpretation. Use [Building HeavenBench](BUILDING.md) for toolchains and packages, and [Getting started](GETTING_STARTED.md) for application controls.

## Source layout

```text
benchmark/                 C++23 application, materials, host tests and GPU smoke tests
benchmark/src/engine/      CPU-side data, loaders, geometry and simulation
benchmark/src/rhi/         Filament backend, resource owners and modular rendering features
benchmark/src/benchmark/   Scores, result reports and microprofiler
android/                   Android launcher, SDL bridge and configuration
cmake/                     Shared CPU-engine and build options
scripts/                   Build, packaging, capture, analysis and validation helpers
tools/asset_builder/       Official installer to validated runtime asset ZIP
tools/asset_packer/        Independent CPU-only ZIP packer
tools/hires_textures/      Optional offline texture generation
tools/extraction/          UNG extractor, mesh-to-OBJ converter and scene assembler
lib/                       Pinned dependencies and bundled third-party source
patches/filament/          Reproducible Filament patch and dependency contract
tests/                     Tool, packaging, policy and visual-manifest tests
docs/                      Guides, design history, audits and known issues
build/                     Local generated build trees and validation evidence
```

The top-level CMake project builds the extraction tools; the benchmark build lives under `benchmark/`. Original archives remain in ignored `data/`, extracted `core/` and `heaven/` trees in `extracted/`, and scene OBJ exports in `assembled/`. Their internal archive prefixes are retained by extraction. [File formats](FILE_FORMATS.md) records binary layouts and coordinate conventions; [extracted shaders](EXTRACTED_SHADERS.md) records original-engine shader research.

Rendering goes through the Filament backend, using Vulkan with an OpenGL fallback. CPU loaders remain independent of GPU APIs; backend collaborators own textures, meshes, materials, scene entities and lighting, while modular features own optional effects. Persistent scene/grass buffers are built once and animated with transforms or shader uniforms. [Rendering features](FILAMENT_RENDERING_FEATURES.md), [instancing and culling](CULLING_AUDIT.md) and the [Filament patch notes](../patches/filament/README.md) explain the active contracts and measured limits.

Internal artistic and performance coefficients live in `benchmark/src/render_tuning.h`; typed material-family binders initialize their shader uniforms before authored, quality and per-frame values are applied. Runtime configuration, authored material data and quality presets remain authoritative. [Benchmark design](BENCHMARK_PLAN.md) and [Filament migration history](FILAMENT_MIGRATION_PLAN.md) retain the broader design records.

## Benchmark measurement and report format

Each run first measures a deterministic, fixed-step CPU workload over the real scene, then renders the full authored camera path at a fixed resolution. The result reports score version 3 with separate Overall, CPU, and GPU scores. Overall and GPU use 1000 points at 60 FPS. CPU uses 1000 points at 10,000 deterministic world updates/s (a 0.1 ms reference update), while also reporting raw updates/s and the corresponding 60 Hz real-time multiplier. The Overall score combines equal-weight per-scene throughput with the presented-frame 1% low. GPU scoring consumes every completed whole-frame timestamp once and drains the final in-flight benchmark frames before producing the result.

Every successfully completed run is saved automatically under `results/` in writable user storage (beside the executable for unpackaged development without `HEAVEN_USER_DIR`) as a matched pair:

```text
results/HeavenBench_1.0.0_YYYYMMDD_HHMMSS_mmm.json
results/HeavenBench_1.0.0_YYYYMMDD_HHMMSS_mmm.html
```

The self-contained HTML report follows the original Heaven benchmark's dark/orange result layout and includes the overall, CPU, and GPU breakdown, system information, settings, and the full effective configuration. The application title and report heading identify both the benchmark version and selected CPU build architecture, for example `HeavenBench 1.0.0 [zen4]`. The machine-readable JSON uses schema version 2 with top-level `application`, `run`, `scores`, `results`, `cpu`, `gpu`, `system`, `settings`, and `configuration` objects; `application.version` and `application.build_architecture` carry the same build identity. It stores aggregate measurements and timing distributions, not raw per-frame sample arrays. A cancelled or refused run does not create a report. If a report cannot be saved, the benchmark logs the error and keeps the completed result available in the application.

Reports identify result schema 2 and score version 3 independently of application version 1.0.0. Compare fixed workloads with matching effective settings and build identity. Diagnostic microprofiler tours retain frame statistics but do not receive scores. For report locations and user-facing behavior, see [Getting started](GETTING_STARTED.md#reports-and-saved-files).

## Tests and rendering validation

HeavenBench checks the active graphics driver at startup on Linux and Windows. A dismissible advisory explains software rendering, legacy Nouveau, or versions below the project's recommended baseline and gives compatible upgrade guidance. Driver details remain available in Diagnostics and benchmark reports. See [graphics driver guidance](GRAPHICS_DRIVERS.md) for the checks and their limits.

Static analysis requires Clang-Tidy 21, its matching `run-clang-tidy`, ripgrep, and Python with PyYAML (`python3-yaml` on Debian/Ubuntu).

```bash
./scripts/build.sh --jobs 6 \
  --target heaven_benchmark_tests --target heaven_benchmark_smoke
ctest --test-dir build/renderer --output-on-failure
./scripts/check_modern_cpp.sh

# Optional: actual rendering checks, with a working GPU/display and scene assets.
HEAVEN_SMOKE=1 ctest --test-dir build/renderer \
  --output-on-failure -R '^heaven_benchmark_smoke$'
```

The host suite includes unit, property, and asset-integration tests. Asset-dependent cases can skip when the original data is absent. The graphical smoke harness reports a CTest skip unless explicitly enabled and a display is available. A headless test result does not establish visual correctness or rendering performance.

The GPU harness checks the actual HDR encoding shader through numeric offscreen readback under either Vulkan or OpenGL. Set `HEAVEN_SMOKE_HDR=1` with `SDL_VIDEODRIVER=wayland` to additionally exercise the real HDR swapchain, startup splash and UI on a compatible display; `HEAVEN_SMOKE_WINDOW_LIFECYCLE=1` adds resize/fullscreen checks. These checks require `HEAVEN_SMOKE=1` and working scene assets. An unsupported HDR display fails explicitly. Numeric encoding checks do not establish physical display brightness or full-scene visual acceptance.

Full-scene visual checks run without manual camera movement. The audit runner captures every authored scene and paired feature settings, retaining images, effective configuration, camera/driver metadata, logs and checksums:

```bash
python3 scripts/visual_audit.py --suite all --data assets --output build/visual-audit/current
# Single held viewpoint, with no overlay or benchmark score:
./scripts/run.sh --data assets --camera-tour-time 193.25 \
  --capture build/visual-audit/night.ppm --capture-frames 90
# Exact world position, yaw and pitch; FOV is vertical, in degrees:
./scripts/run.sh --data assets --camera-pose '-62,62,20,-30,-20' --camera-fov 45
```

`--capture-tour-motion` advances a selected tour position at the capture simulation rate (60 Hz by default). `--capture-fps 15` tests larger simulation steps; `--capture-realtime` uses normal wall-clock animation. Capture readback affects throughput: use `--profile build/frame-profile.json --profile-seconds 65` with the same camera/settings for an uncapped timing run without screenshots. HDR captures retain both a diagnostic SDR preview and raw PQ `.pq.pfm` data. See [visual audit](VISUAL_AUDIT.md) for reproducible comparisons and their limits.

Open **Diagnostics → Microprofiler** or press **F6** to inspect nested tasks, record/stop, freeze a view and save JSON/HTML traces. Opening the panel does not record. For a timed hidden trace, use `./scripts/run.sh --data assets --microprofile build/traces/scene --microprofile-seconds 10`; add `--microprofile-visible` to show the panel. A tour recorded at any point becomes diagnostic and unscored while retaining timing statistics. See [Microprofiler](MICROPROFILER.md) for clocks, offline analysis, measured overhead and remaining platform validation.

### Rendering investigations

[Rendering test scenes](RENDER_TEST_SCENES.md) provides procedural stimuli and compact original-mesh scenes with paired feature controls. Keep source, binary, asset, configuration, camera and API identities with capture receipts; nonempty inputs alone do not establish visible activation. [Visual audit](VISUAL_AUDIT.md) and [known bugs](KNOWN_BUGS.md) retain image evidence and open acceptance limits.

[Performance optimizations](PERFORMANCE_OPTIMIZATIONS.md) records measured costs, and [Instancing and culling](CULLING_AUDIT.md) records batching, animated bounds and native draw-submission checks. Performance comparisons require matching hardware, drivers, graphics API, resolution, scene and effective settings, with competing workloads controlled. Use the separate `--profile` throughput mode for timing without screenshot readback. The [Microprofiler](MICROPROFILER.md) explains trace clocks, incomplete coverage, offline analysis and profiler overhead; CPU and frame-relative GPU times must not be added together.

## Supporting extraction tools

The [asset builder](../tools/README.md) automatically builds the extractor and CPU-only packer to prepare benchmark data. For independent extraction or mesh inspection, the top-level project requires CMake 3.25+, Ninja, a C++23 compiler, Python 3 for regression tests and zlib development headers:

```bash
cmake -S . -B build/tools -G Ninja
cmake --build build/tools
```

This produces two executables:

- `build/tools/uniginex` — UNG archive extractor
- `build/tools/mesh2obj` — mesh to OBJ converter

### 1. Extract UNG Archives

```bash
./build/tools/uniginex -o data/data/heaven.ung extracted/heaven
./build/tools/uniginex -o data/data/core.ung extracted/core
```

Use `-l` to list contents without extracting. The tool automatically tries known encryption keys for Cradle, Sumoman, Superposition, Oil Rush, and Heaven.

### 2. Convert Individual Meshes to OBJ

```bash
./build/tools/mesh2obj extracted/heaven/heaven/meshes/airship/airship.mesh
```

This writes `airship.obj` and `airship.mtl` alongside the input file. Each surface in the mesh becomes a named object/material group in the OBJ.

### 3. Assemble Complete Scenes

Individual `.mesh` files contain geometry in local space. The `.node` files define how multiple meshes are positioned together with 4x4 transform matrices. The assembler reads a `.node` file, resolves all mesh references, applies transforms, and writes a single combined OBJ:

```bash
python3 tools/extraction/assemble_scene.py extracted/heaven/heaven/nodes/airship.node assembled/airship.obj \
    --data-dir extracted/heaven
```

The `--data-dir` flag specifies where to resolve relative mesh paths like `heaven/meshes/airship/airship.mesh`.

## What Was Discovered

See [FILE_FORMATS.md](FILE_FORMATS.md) for detailed binary format specifications covering:

- UNG archive format (versions 3 and 4)
- mi08 static mesh format
- xml0 binary XML format used in `.node` and `.world` files
- Transform matrix conventions

## Early extraction-tool conversion history

The original `uniginex.c` was a Windows-targeted C program. The initial conversion to `uniginex.cpp` involved the changes below. These are historical porting notes; subsequent C++23 refactoring replaced further C-style allocation, I/O and platform handling, so consult the current source for implementation details:

- Replacing VLAs with fixed-size arrays (C++ does not support VLAs)
- Adding explicit casts for `void*` → typed pointer (`realloc` returns, `argv` to `u8*`)
- Changing `try_keys_t` struct members from `u8*` to `const char*` for string literal compatibility
- Fixing `const static` → `static const` ordering
- Adding `reinterpret_cast` for `u8*` ↔ `char*` conversions at API boundaries (LZ4, zlib)
- Replacing `NULL` with `nullptr`, `WIN32` with `_WIN32`
- Adding `#include <unistd.h>` for POSIX `chdir()`, `#define chdir _chdir` for Windows
- Using C headers (`<stdio.h>`) instead of C++ wrappers (`<cstdio>`) to avoid conflicts with large-file-support macros that redefine `fopen` → `fopen64`

The bundled `lib/lz4/lz4.c` and `lib/lz4/lz4.h` remain unchanged C sources — they already have `extern "C"` guards and compile cleanly as a static C library linked into the C++ executable.

## Attribution and licensing

HeavenBench grew from archive extraction and asset research into a complete benchmark. Its `uniginex` extractor is derived from Luigi Auriemma's original Windows C tool, refactored into multiplatform C++, with mesh conversion and scene assembly added alongside it. The extractor retains its original copyright and GPL-2.0-or-later notice and its separate 0.3.1 tool version. Project changes are licensed under [GPL-3.0-or-later](../LICENSE). Third-party code and bundled assets retain their upstream notices; original or derived Heaven assets are not relicensed.

See [Credits](CREDITS.md) for contributor, library, tool and asset acknowledgements. Return to the [documentation index](README.md).
