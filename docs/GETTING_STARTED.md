# Getting started

HeavenBench runs a CPU workload and an authored camera tour through the original Heaven scene. It also provides interactive exploration, configurable rendering and saved result reports. The current source version is 1.0.0; packages built by the workflows remain release candidates until the [release checks](RELEASE_READINESS.md) are complete.

## Prepare the scene assets

Original Heaven assets are external to the repository and application packages. From the repository root on Linux, create the shared pack with:

```bash
./scripts/build_assets.sh --jobs 6
```

The builder creates `assets/assets.zip`, building the extraction and CPU-only packing tools automatically. It reuses an existing official Heaven 4.0 installer when available; otherwise it downloads the installer, verifies its pinned SHA-256 and extracts the archives without running the installer. Automatic preparation requires Python 3.10+, CMake 3.25+, Ninja, a C++23-capable Clang compiler with libc++, GLM and zlib development headers, and Git. The [asset tools guide](../tools/README.md) covers prerequisites, offline operation and native Windows commands. The resulting pack works on Linux, Windows and Android.

Installer lookup checks the current directory, repository root and `data/`. Select a local copy explicitly to prevent downloading:

```bash
./scripts/build_assets.sh --installer /path/to/Unigine_Heaven-4.0.run --no-download
```

The normal asset location is an `assets/` directory containing compatible ZIP packs. An explicit `--data` argument can select that directory, a ZIP file or an extracted data root. See [ZIP asset storage](ZIP_ASSET_STORAGE.md) to pack an already extracted tree, and the [optional high-resolution texture guide](HIRES_TEXTURE_PIPELINE.md) for additional texture sets.

## Start the application

The commands below assume you already have a locally built package. [Building HeavenBench](BUILDING.md) describes package creation and source builds.

| Platform | Start HeavenBench |
| --- | --- |
| Linux | Place `assets/` or `assets.zip` beside the AppImage, then run it. An explicit `--data /path/to/assets` also works. |
| Windows | Extract the ZIP, put `assets.zip` in the `assets/` directory beside `Launch-HeavenBench.cmd`, then use that launcher. |
| Android | Install the APK, import `assets.zip` through the file picker, then choose **Run benchmark** or **Explore**. |

On Linux systems without FUSE, launch the AppImage with `APPIMAGE_EXTRACT_AND_RUN=1 ./HeavenBench-*.AppImage`. Linux packages use host graphics drivers and the ALSA audio library; see [platform package requirements](BUILDING.md#platform-packages) if startup fails.

A development checkout uses the same scene pack:

```bash
./scripts/run.sh --data assets
./scripts/run.sh --data assets --benchmark
```

Loading shows the current stage and a progress bar while scene preparation runs in the background. Matching tessellation and displaced-mesh refinement results are reused from the cache; changed geometry inputs, settings or refinement code select fresh entries. Desktop Escape cancels loading; an active refinement task finishes before its resources are retired. GPU uploads run on the rendering thread with event handling between meshes.

## Controls

The toolbar keeps **Benchmark** or **Cancel**, **Settings**, **Quit** and **Credits** available. Credits shows the original Heaven team, HeavenBench contributors and third-party acknowledgments without pausing the scene. Cancel discards the incomplete score. Opening Settings alone does not cancel a run; changing a live workload setting or applying a reload cancels it before changing the workload.

| Desktop control | Action |
| --- | --- |
| W, A, S, D | Move through the scene. |
| Q / E | Move down / up. |
| Hold right mouse button | Look around; release it to use the toolbar and settings. |
| Shift | Move faster. |
| B | Start a benchmark while exploring. |
| C | Toggle camera collision. |
| Escape | Close Credits when open; otherwise cancel an active benchmark or quit. |
| F6 | Open or close the Microprofiler panel. |

Android provides touch-friendly toolbar and Settings controls. Its optional [lower-workload preview profile](../android/config-low-preview.json) reduces exploration workload; scored benchmark tours remain uncapped. Android display resolution follows the actual rendering Surface, with a configurable render scale. See the [Android guide](../android/README.md) for controls, configuration and current device-validation limits.

## Rendering settings

Use a rendering preset as a starting point, then adjust individual features in Settings. The [feature and preset guide](FILAMENT_RENDERING_FEATURES.md) explains the available effects. Resolution, texture quality and geometry density can substantially change the workload, so retain matching effective settings when comparing hardware.

The Geometry panel offers Disabled, Moderate (0.75×), Normal (1×) and Extreme (1.5×) tessellation levels, plus Custom for independent Scale, Factor and Distance values. Apply pending geometry changes with **Reload**. These levels control displacement amount, topology density and fade distance; surfaces already at the subdivision limit can retain the same triangle count at different levels. From the command line, `--tess-level moderate` after a preset overrides its geometry settings; later options win.

HeavenBench checks the active graphics driver at startup on Linux and Windows. A dismissible advisory identifies software rendering, legacy Nouveau or versions below the project's recommended baseline. Driver information also appears in Diagnostics and reports. See [graphics driver guidance](GRAPHICS_DRIVERS.md) for the checks and their limits, and [known issues](KNOWN_BUGS.md) for current rendering investigations.

## Reports and saved files

Completed runs automatically save matched JSON and self-contained HTML reports under `results/` in writable user storage. The HTML report includes Overall, CPU and GPU scores, frame-time statistics, hardware information and the full effective configuration. Cancelled or refused runs do not create a completed result report. If saving fails, the application logs the error and keeps the completed result available in its window.

| Platform | Default user storage |
| --- | --- |
| Linux AppImage | `${XDG_DATA_HOME:-$HOME/.local/share}/HeavenBench` |
| Windows launcher | `%LOCALAPPDATA%\HeavenBench` |
| Android | The app-specific files directory shown by the launcher, usually `/sdcard/Android/data/app.zetta.heavenbench/files/`. |

Desktop launchers accept `HEAVEN_USER_DIR` to override the user directory. An unpackaged development binary without that override saves reports beside its executable. Configuration is stored as `config.json`; packages accept `--config FILE` and ordinary renderer flags. `HEAVEN_DATA` selects assets in the desktop launchers, and saved configuration can retain its own data path.

```text
results/HeavenBench_1.0.0_YYYYMMDD_HHMMSS_mmm.json
results/HeavenBench_1.0.0_YYYYMMDD_HHMMSS_mmm.html
```

Results use schema version 2 and score version 3. Compare scores only when score versions, workloads and effective settings match. [Development and measurement](DEVELOPMENT.md#benchmark-measurement-and-report-format) describes the score calculations and machine-readable fields.

## Inspect performance

Open **Diagnostics → Microprofiler** or press **F6** to inspect tasks, start and stop recording, freeze the view, and save JSON/HTML traces. On narrow windows, the Diagnostics menu is labelled **More**, or **REC** while recording. Opening the panel does not start recording; freezing the view leaves collection and simulation running. Recording at any point during a benchmark tour makes that tour diagnostic and unscored while retaining its timing statistics.

See the [Microprofiler guide](MICROPROFILER.md) for controls, offline analysis and capture limits, or [performance investigations](PERFORMANCE_OPTIMIZATIONS.md) for measured feature costs. [Rendering test scenes](RENDER_TEST_SCENES.md) isolate effects with a few objects and real meshes for visual debugging.

Return to the [documentation index](README.md) or the [main README](../README.md).
