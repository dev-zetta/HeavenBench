# Shared release builds

Run these commands from a source checkout with Docker, Buildx and Bash installed. GitHub Actions calls the same entry points; workflow YAML contains no compiler, SDK installation, test or packaging commands.

```bash
./scripts/build_docker.sh linux --jobs 6
./scripts/build_docker.sh linux --cpu-arch zen1 --jobs 6
./scripts/build_docker.sh android --abi arm64-v8a --jobs 6
./scripts/build_docker.sh android --abi x86_64 --jobs 6
```

Linux accepts `portable` and `zen1` through `zen5`; Android accepts `arm64-v8a` and `x86_64`. Builds use a Linux x86-64 container; Docker Desktop on an ARM host needs its x86-64 emulation support. Select an existing daemon with `DOCKER_CONTEXT` if necessary. The wrapper does not change your global Docker context or builder.

Default outputs are `build/release/linux-<arch>/` and `build/release/android-<abi>/`. `--output-dir DIR` selects a different directory outside the checkout or below its ignored `build/` tree. Spaces and shell metacharacters are supported; commas/newlines are rejected because Buildx uses a comma-separated exporter argument. Artifacts are exported to the invoking host without exposing its source tree, home directory, GPU or Docker socket inside the build. No `sudo` is needed for files exported by a normal user invocation.

The [Dockerfile](../docker/Dockerfile) owns the Ubuntu 24.04 base digest, Clang/libc++ 21 and development package list. [Dependency bootstrap](../scripts/fetch_dependencies.sh) owns source revisions and the reviewed Filament patch; [Android versions](../android/versions.properties) owns SDK/NDK/SDL/Gradle versions. The Android installer additionally verifies the JDK and command-line-tools archives by SHA-256 and installs only the requested SDK packages. Distro package updates remain enabled within the pinned distribution/toolchain repository; `build-environment.txt` records installed versions. This is an isolated, declared build environment, not a claim of bit-for-bit reproducible packages across future package updates.

`.dockerignore` permits project source directories, the tracked LZ4/nlohmann headers and the pinned OpenSubdiv CPU subset with its LICENSE/NOTICE. It excludes host Filament/Jolt/other downloaded libraries, build products, toolchains, editor/agent state, original Heaven installers/data/extracted files, and optional large generated asset packs. Container dependency fetching must succeed independently of any local vendor checkouts. Renderer-owned resources and committed materials remain in the context. Ordinary packaging never regenerates material packages.

BuildKit caches dependency layers and keeps a locked compilation/download cache for each platform, architecture and compiler generation. Concurrent builds of the same target serialize cache use. A content fingerprint covers the complete source context and installed toolchain; any change clears compilation trees while preserving downloads and Android signing keys. This also handles older source archives whose timestamps would otherwise let Ninja reuse stale objects. Unchanged inputs retain compiled outputs. `--clean` disables layer reuse and selects a new empty cache without deleting prior host artifacts or globally pruning Docker. Keep the same Docker builder to benefit from its caches; a new GitHub runner starts cold. The wrapper saves BuildKit output to `logs/docker-build.log`, including failures; detailed pipeline logs, lint reports, cache state and runtime verification are exported after a successful build. Failed builds return nonzero and do not export a new package; older packages in the output directory are preserved.

The container calls the existing native build/package helpers through `ci_linux.sh` or `ci_android.sh`. The Linux portable pipeline builds and tests extraction tools, builds the independent CPU asset packer, compiles the renderer and host/smoke suites, runs the full Clang-Tidy gate, and packages/verifies the AppImage. A separate runtime stage repeats relocation checks without a compiler, system SDL or libc++ installed. Graphics/window-system drivers and ALSA remain runtime prerequisites. Zen variants compile and package but are not executed on a generic runner. `--skip-analysis` is an explicit local shortcut; CI leaves analysis enabled.

The Android pipeline builds host generators and the requested ABI, checks SDL Java staging, native dependencies/JNI and 16 KiB alignment, runs Lint and assembles a signed development APK, then verifies its signature and ZIP alignment. The debug signing key lives in the per-ABI cache. A clean cache creates a new development key; uninstall a differently signed existing development APK before installing that candidate. Production signing and physical-device acceptance are separate release steps.

Each successful Docker build exports a source tarball from the exact filtered context supplied to Docker, including local uncommitted changes and new source files. `source-manifest.json` records every source file hash and the archive checksum. This avoids pairing a binary from edited sources with a `git archive HEAD` snapshot. The archive contains dependency pins/patches and the checked-in LZ4, nlohmann and OpenSubdiv subset, but no downloaded third-party checkouts; Docker can rebuild it without `.git`. The adjacent SHA-256 files cover both package and source archive. Original scene data is always supplied separately when running the package.

## Windows

```powershell
pwsh -File scripts/ci_windows.ps1 -Jobs 6
# Optional explicit locations:
pwsh -File scripts/ci_windows.ps1 -BuildDir C:\build\heaven -OutputDir C:\packages\heaven
```

Windows uses native PowerShell 7, Visual Studio 2022 17.13+ C++ Build Tools and Windows SDK, CMake/Ninja, Git for Windows and Python 3. The shared pipeline provisions pinned project/vcpkg/SDL dependencies, builds all targets, runs host/extraction tests, packages the ZIP and checks the relocated executables/launcher with a restricted runtime PATH. `.github/workflows/windows.yml` calls this helper directly. Native Windows execution is required to validate the produced DLL/runtime package; Linux orchestration tests do not establish that result.

## Development and graphical validation

The lower-level `build.sh`, `build_android.sh`, `build_windows.ps1` and packaging helpers remain available for incremental native development, local AOCC builds and material regeneration. Their compiler flags and CMake options are shared with the container pipelines. Docker release builds are headless and contain no original assets: asset-dependent cases may skip, and the graphical harness deliberately reports a skip. Test rendering, HDR, input, audio and performance separately with an actual display/GPU or Android device and the intended scene assets.
