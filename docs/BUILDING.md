# Building HeavenBench

Run the commands in this guide from the repository root. Use the shared helper scripts for renderer builds: they select the required compiler, standard library, dependency revisions and material tools. To run an already built package, start with [Getting started](GETTING_STARTED.md).

## Release package pipeline

Linux and Android use the same Docker entry point locally and in GitHub Actions. Install Docker with Buildx and Bash; compilers, development libraries, Java, the Android SDK/NDK, and pinned project dependencies are installed inside the build. Windows uses a shared native MSVC pipeline; macOS uses a native Apple Silicon pipeline with Homebrew LLVM and MoltenVK.

```bash
./scripts/build_docker.sh linux --jobs 6
./scripts/build_docker.sh linux --cpu-arch zen5 --jobs 6
./scripts/build_docker.sh android --abi arm64-v8a --jobs 6
# On Windows with the native prerequisites below:
pwsh -File scripts/ci_windows.ps1 -Jobs 6
```

Docker packages appear under `build/release/linux-<arch>/` or `build/release/android-<abi>/`, together with checksums, logs, exact build sources and package-version provenance. Linux portable builds run extraction/asset tests, renderer host tests, static analysis and AppImage relocation checks in a separate minimal runtime image. Android builds validate native libraries, run lint and verify APK signing/alignment. See [Docker builds](DOCKER_BUILDS.md) for caches, clean builds, outputs and validation scope.

## Native Linux development

For incremental development outside Docker, use CMake 3.25 or newer, Ninja, a C++23 compiler, Python 3.10 or newer and zlib development headers. The asset builder and renderer require a Clang-compatible compiler with libc++ and GLM headers, plus Git to obtain pinned dependencies. The renderer additionally requires SDL2, Vulkan and X11/Wayland development packages. The [Dockerfile](../docker/Dockerfile) owns the complete release build package list and compiler selection.

```bash
./scripts/fetch_dependencies.sh
./scripts/build.sh --jobs 6
./scripts/build_assets.sh --jobs 6
./scripts/run.sh --data assets
./scripts/run.sh --data assets --benchmark
```

The lower-level helpers remain available for debug builds, CPU-target selection and native Android builds:

```bash
./scripts/build.sh --debug
./scripts/build.sh --platform linux
./scripts/build.sh --cpu-arch zen1
./scripts/build.sh --cpu-arch zen2
./scripts/build.sh --cpu-arch zen3
./scripts/build.sh --cpu-arch zen4
./scripts/build.sh --cpu-arch zen5
./scripts/build_android.sh --abi arm64-v8a --jobs 6
```

Generated build output lives under repository-level `build/`: Linux Release uses `build/renderer/`, Linux Debug uses `build/renderer-debug/`, and helpers use Ninja. `HEAVEN_BUILD_ROOT` relocates the default renderer trees; `HEAVEN_BUILD_DIR` selects one exact tree for both `build.sh` and `run.sh`. `run.sh --build-dir DIR --build` is the command-line equivalent. `HEAVEN_C_COMPILER` and `HEAVEN_CXX_COMPILER` select explicit compiler executables in the default Clang mode.

Build trees contain absolute source paths. After moving a checkout, configure a fresh tree with `./scripts/build.sh --build-dir build/renderer-fresh`, then pass the same `--build-dir` to `run.sh`.

### Select a Linux GPU

The run helper enumerates Linux DRM GPUs and defaults to the device with the most dedicated video memory. List detected indices or select a device explicitly:

```bash
./scripts/run.sh --list-gpus
./scripts/run.sh --gpu N --benchmark
./scripts/run.sh --gpu integrated
```

PCI vendor/device IDs and PCI addresses are accepted alongside indices. `--gpu integrated` prefers the lowest-VRAM device; `--gpu default` leaves selection to the Vulkan driver. The helper sets device-selection environment variables before the renderer loads its graphics backend.

### CPU architecture variants

The default `--cpu-arch portable` targets ordinary x86-64 systems. `zen1` through `zen5` map to `-march=znver1` through `-march=znver5`; `native` asks the compiler to target the build host. Architecture-specific binaries require the matching CPU instruction set. The setting applies to HeavenBench and its Filament/Jolt runtime libraries; Filament's build-time generators remain portable for cross-building variants.

GitHub Actions compiles portable, Zen 1 and Zen 5 Linux variants with Clang 21. Host tests and static analysis run on the portable variant because hosted runners guarantee x64 rather than a particular Zen generation.

### Optional AMD AOCC toolchain

After reading and accepting AMD's AOCC 5.2 EULA, use the setup helper to download the official archive, verify its published SHA-256 and install it under ignored `build/toolchains/aocc-5.2.0/` without `sudo`:

```bash
./scripts/setup_aocc.sh --accept-eula
./scripts/build.sh --compiler aocc --clean --cpu-arch zen3
./scripts/run.sh --compiler aocc
```

The setup helper also accepts `--archive /path/to/aocc-compiler-5.2.0.tar` for an existing archive. The build helper sources AOCC's generated environment and defaults to `build/renderer-aocc/`, keeping its CMake state separate from upstream Clang. AOCC must still be able to use the libc++ headers and libraries required by HeavenBench.

## Dependencies and materials

Filament is based on public upstream v1.77.2 with the reviewed [HeavenBench rendering patch](../patches/filament/README.md). `scripts/fetch_dependencies.sh` verifies the upstream revision, patch checksum and final source tree, preserving existing local edits. The same bootstrap runs inside Docker and the Windows pipeline.

Committed `.filamat` files use matching material package version 77. Ordinary builds embed those packages without recompiling them. After editing a material source, use the incremental Linux host path:

```bash
./scripts/build.sh --materials
```

Use `./scripts/build.sh --materials-all` only after a deliberate Filament/material-compiler change or another intentional replacement of every committed package. Both material modes rebuild the material compiler first. Generated `benchmark/src/rhi/materials_data.h` and `materials_data.cpp` come from `scripts/gen_materials_data.py` and must not be edited by hand. Review the changed material packages and run the rendering checks described in [Development](DEVELOPMENT.md).

## Platform packages

| Target | Build/package helper | GitHub workflow | Artifact |
| --- | --- | --- | --- |
| Linux x86-64 | `./scripts/build_docker.sh linux --jobs 6` | [Linux AppImage](../.github/workflows/build.yml) | AppImage and SHA-256, portable/Zen 1/Zen 5 CI variants |
| Windows x64 | `pwsh -File scripts/ci_windows.ps1 -Jobs 6` | [Windows](../.github/workflows/windows.yml) | ZIP with renderer, asset packer, extraction tools, resources, DLLs, and notices |
| macOS Apple Silicon (M1+) | `./scripts/ci_macos.sh --jobs 2` | [macOS ARM64](../.github/workflows/macos.yml) | ZIP containing an ad hoc signed app bundle, bundled MoltenVK/runtime libraries, notices and SHA-256 |
| Android ARM64 / x86-64 | `./scripts/build_docker.sh android --abi arm64-v8a --jobs 6` | [Android](../.github/workflows/android.yml) | Debug-signed APK with optimized native code and SHA-256 |

Each workflow builds on pushes and pull requests and supports manual dispatch. The packages retain third-party notices and exclude original Heaven data. Workflow artifacts are release candidates; creating a public release or signing an Android production APK remains a separate step.

For macOS prerequisites, local commands, app launch and validation limits, see [HeavenBench on Apple Silicon](MACOS.md). The workflow uses the explicit `macos-15` ARM64 runner and checks its architecture before building. The renderer targets macOS 14 or newer; the package declares the highest minimum required by its bundled libraries. The published 1.0.0 app requires macOS 15.0 or newer. Intel Macs are outside this build target.

Linux packaging requires `curl`, `file`, `patchelf`, `desktop-file-validate` (`desktop-file-utils` on Ubuntu), and Python 3 in addition to the renderer prerequisites. The helper downloads checksum-pinned AppImage tools and works without FUSE. Use `--skip-build --build-dir DIR` to package a previously built Release tree; pass its matching `--cpu-arch` for an optimized variant. CI builds on Ubuntu 24.04, setting a glibc 2.39 baseline; packaging on a newer distribution can require newer glibc. Vulkan/OpenGL loaders, graphics drivers, and the ALSA audio library (`libasound.so.2`, provided by `libasound2t64` on Ubuntu 24.04) come from the user's desktop system. ALSA stays external so its configuration and sound-device plugins match the host. Zen variants require the corresponding CPU instruction set.

Place your `assets/` ZIP directory or `assets.zip` beside the AppImage, or pass `--data /path/to/assets`. `HEAVEN_DATA` also selects assets. Settings, caches, and reports go under `${XDG_DATA_HOME:-$HOME/.local/share}/HeavenBench`; override that location with `HEAVEN_USER_DIR`. The package accepts explicit `--config` and other renderer flags. For systems without FUSE, run `APPIMAGE_EXTRACT_AND_RUN=1 ./HeavenBench-*.AppImage`. A saved configuration can retain its own data path.

Windows builds require native PowerShell 7, Visual Studio 2022 17.13+ C++ Build Tools and Windows SDK, CMake, Ninja, Git for Windows, and Python 3. The build helper selects MSVC and bootstraps pinned vcpkg dependencies; the package helper collects the runtime. Use the supplied `Launch-HeavenBench.cmd` launcher; user files go under `%LOCALAPPDATA%\HeavenBench` unless `HEAVEN_USER_DIR` is set. The pinned Filament revision does not support clang-cl or MSYS2 builds.

The Android Docker toolchain installs Java 17, Android SDK 35/build-tools 35.0.0, and NDK `28.2.13676358`, including acceptance of the requested SDK package licenses. Its shared helper downloads pinned SDL2 and Gradle, builds the host shader generators and native libraries, then checks APK signing and alignment. Native developers can still call `scripts/build_android.sh` with these prerequisites installed and `ANDROID_HOME` set. Install the development APK and import a desktop-produced `assets.zip` through its file picker. Scene data and reports stay in the app's writable storage. Physical-device rendering and touch interaction require separate acceptance testing.

For native Android development, `scripts/build_android.sh` produces host generators, shared native libraries under `build/renderer-android-<abi>/`, matching SDL Java glue and the development APK. The lower-level `scripts/build.sh --platform android --android-abi arm64-v8a` builds only the native target. SDK/NDK setup and device checks are documented in the [Android guide](../android/README.md).

Native Windows development uses `scripts/build_windows.ps1` and `scripts/package_windows.ps1`; `scripts/ci_windows.ps1` composes them with release checks. It is the supported MSVC pipeline.

## Asset preparation and standalone tools

The [asset builder](../tools/README.md) automatically builds the extraction and CPU-only packing tools without configuring Filament, SDL or Vulkan. Asset preparation commands, offline operation and packing an existing tree are covered in [Getting started](GETTING_STARTED.md#prepare-the-scene-assets), the [asset tools guide](../tools/README.md) and [ZIP asset storage](ZIP_ASSET_STORAGE.md).

The top-level CMake project builds the independent extraction tools. Its commands and mesh-inspection examples are retained in [Development](DEVELOPMENT.md#supporting-extraction-tools).

Original installers, UNG archives, `data/`, `extracted/`, assembled original-geometry exports and runtime asset ZIPs stay local. Source-owned generated textures/images and generated fixture packs remain eligible for Git; `.gitignore` deliberately avoids blanket image or ZIP rules. Optional reproducible `assets/assets-hires.zip`, local editor/agent state, runtime UI files, caches and reports are excluded. Neither original nor generated Heaven-derived assets are relicensed by this repository.

Return to the [documentation index](README.md).
