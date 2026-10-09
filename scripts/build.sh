#!/usr/bin/env bash
# HeavenBench — build script
#
# Configures and builds the benchmark with a Clang-compatible compiler/libc++.
# Usage:
#   ./scripts/build.sh                  # incremental build (Release)
#   ./scripts/build.sh --clean          # wipe build dir first
#   ./scripts/build.sh --debug          # Debug build
#   ./scripts/build.sh --jobs 8         # override parallel job count
#   ./scripts/build.sh --cpu-arch zen3   # optimize the whole build for AMD Zen 3
#   ./scripts/build.sh --platform android --android-abi arm64-v8a
#   ./scripts/build.sh --materials      # also recompile Filament materials
set -euo pipefail

# Resolve repo root (script lives in scripts/, repo root is one level up).
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"
# shellcheck source=build_paths.sh
source "$SCRIPT_DIR/build_paths.sh"

BUILD_TYPE="Release"
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=0
COMPILE_MATERIALS=0
COMPILE_MATERIALS_ALL=0
PLATFORM="linux"
ANDROID_ABI="arm64-v8a"
ANDROID_API="26"
CPU_ARCH="portable"
BUILD_DIR="${HEAVEN_BUILD_DIR:-}"
COMPILER="${HEAVEN_COMPILER:-clang}"
C_COMPILER="${HEAVEN_C_COMPILER:-}"
CXX_COMPILER="${HEAVEN_CXX_COMPILER:-}"
EXTRA_CMAKE_ARGS=()
BUILD_TARGETS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs|-j|--platform|--android-abi|--android-api|--cpu-arch|--compiler|--build-dir|--cmake-arg|--target)
            if [[ $# -lt 2 || -z "$2" ]]; then
                echo "Error: $1 requires a value" >&2
                exit 1
            fi ;;
    esac
    case "$1" in
        --clean)
            CLEAN=1
            shift ;;
        --debug)
            BUILD_TYPE="Debug"
            shift ;;
        --release)
            BUILD_TYPE="Release"
            shift ;;
        --jobs|-j)
            JOBS="$2"
            shift 2 ;;
        --platform)
            PLATFORM="$2"
            shift 2 ;;
        --android-abi)
            ANDROID_ABI="$2"
            shift 2 ;;
        --android-api)
            ANDROID_API="$2"
            shift 2 ;;
        --cpu-arch)
            CPU_ARCH="$2"
            shift 2 ;;
        --compiler)
            COMPILER="$2"
            shift 2 ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2 ;;
        --cmake-arg)
            EXTRA_CMAKE_ARGS+=("$2")
            shift 2 ;;
        --target)
            BUILD_TARGETS+=("$2")
            shift 2 ;;
        --materials|-m)
            COMPILE_MATERIALS=1
            shift ;;
        --materials-all)
            # Force a full recompile of EVERY material. Use only when matc itself
            # changed (e.g. a Filament upgrade) — it overwrites the committed,
            # known-good .filamat artifacts for all materials, not just the ones
            # whose .mat source you edited.
            COMPILE_MATERIALS=1
            COMPILE_MATERIALS_ALL=1
            shift ;;
        --help|-h)
            cat <<EOF
HeavenBench build script

Usage: $0 [options]

Options:
  --clean           Wipe build directory before configuring
  --debug           Build with Debug configuration
  --release         Build with Release configuration (default)
  --jobs N, -j N    Parallel job count (default: nproc)
  --platform NAME   Target platform: linux, windows, or android (default: linux)
  --android-abi ABI Android ABI when --platform android (default: arm64-v8a)
  --android-api API Android API level when --platform android (default: 26)
  --cpu-arch ARCH  CPU architecture optimization: portable, native, or zen1
                   through zen5 (default: portable; x86-64 Clang builds)
  --compiler NAME  Linux compiler toolchain: clang or aocc (default: clang)
  --build-dir DIR   Override the generated build directory
  --cmake-arg ARG   Forward one extra argument to CMake configure
  --target NAME    Build a specific CMake target (repeatable; default: heaven_benchmark)
  --materials, -m   Recompile only materials whose .mat source is newer than
                    its compiled .filamat (incremental), then regenerate
                    materials_data.h/.cpp. Leaves untouched materials' committed
                    .filamat bytes alone — recompiling unchanged materials with
                    a different matc can subtly alter shaders (e.g. specular
                    fireflies), so we never clobber them implicitly.
  --materials-all   Force a full recompile of ALL materials. Use only after a
                    matc/Filament change; it overwrites every committed .filamat.
  --help, -h        Show this help

Linux builds require a Clang-compatible C/C++ compiler with libc++ (the embedded
Filament library uses C++17 features that need libc++). Run setup_aocc.sh before
selecting --compiler aocc. HEAVEN_COMPILER selects the toolchain;
HEAVEN_C_COMPILER and HEAVEN_CXX_COMPILER customize clang mode only. All builds
use Ninja and generated files live under <repository>/build by default.
HEAVEN_BUILD_ROOT changes that root; HEAVEN_BUILD_DIR selects one exact renderer
tree. Android builds require ANDROID_NDK_HOME, ANDROID_NDK_ROOT, or
ANDROID_HOME/ANDROID_SDK_ROOT with an installed NDK.
EOF
            exit 0 ;;
        *)
            echo "Unknown argument: $1 (use --help for usage)" >&2
            exit 1 ;;
    esac
done

if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "Error: --jobs requires a positive integer" >&2
    exit 1
fi
if [[ ${#BUILD_TARGETS[@]} -eq 0 ]]; then
    BUILD_TARGETS=(heaven_benchmark)
fi

case "$PLATFORM" in
    linux|windows|android) ;;
    *)
        echo "Error: unsupported platform '$PLATFORM' (expected linux, windows, or android)" >&2
        exit 1 ;;
esac

case "$CPU_ARCH" in
    portable|native|zen1|zen2|zen3|zen4|zen5) ;;
    *)
        echo "Error: unsupported CPU architecture '$CPU_ARCH'" >&2
        echo "Expected portable, native, or zen1 through zen5." >&2
        exit 1 ;;
esac

case "$COMPILER" in
    clang|aocc) ;;
    *)
        echo "Error: unsupported compiler '$COMPILER' (expected clang or aocc)" >&2
        exit 1 ;;
esac

if [[ "$COMPILER" == "aocc" && "$PLATFORM" != "linux" ]]; then
    echo "Error: AOCC is supported only for Linux builds" >&2
    exit 1
fi

if [[ -z "$BUILD_DIR" ]]; then
    BUILD_DIR="$(
        heaven_renderer_build_dir \
            "$PLATFORM" "$BUILD_TYPE" "$ANDROID_ABI" "$COMPILER"
    )"
fi

CMAKE_CONFIGURE_ARGS=(
    -S "$REPO_ROOT/benchmark"
    -B "$BUILD_DIR"
    -G Ninja
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DHEAVEN_TARGET_PLATFORM="$PLATFORM"
    -DHEAVEN_CPU_ARCH="$CPU_ARCH"
)

if ! command -v ninja >/dev/null 2>&1; then
    echo "Error: ninja not found in PATH" >&2
    echo "Install ninja-build; HeavenBench uses one generator for local and CI builds." >&2
    exit 1
fi

if [[ "$PLATFORM" == "linux" ]]; then
    if [[ "$COMPILER" == "aocc" ]]; then
        aocc_env_file="$HEAVEN_AOCC_ROOT/setenv_AOCC.sh"
        aocc_compiler_dir="$HEAVEN_AOCC_ROOT/aocc-compiler-5.2.0/bin"
        if [[ ! -f "$aocc_env_file" ]]; then
            echo "Error: local AOCC installation not found at $HEAVEN_AOCC_ROOT" >&2
            echo "Run ./scripts/setup_aocc.sh --accept-eula first." >&2
            exit 1
        fi
        if [[ -n "$C_COMPILER" || -n "$CXX_COMPILER" ]]; then
            echo "Error: HEAVEN_C_COMPILER/HEAVEN_CXX_COMPILER cannot override --compiler aocc" >&2
            echo "Unset them, or use --compiler clang for a custom Clang-compatible toolchain." >&2
            exit 1
        fi

        # AMD's generated setup file reads optional environment variables
        # without nounset-safe expansion.
        set +u
        # shellcheck disable=SC1090
        source "$aocc_env_file"
        set -u

        C_COMPILER="$aocc_compiler_dir/clang"
        CXX_COMPILER="$aocc_compiler_dir/clang++"
        aocc_version="$("$CXX_COMPILER" --version 2>&1)"
        if [[ "$aocc_version" != *AOCC* ]]; then
            echo "Error: expected AOCC compiler at $CXX_COMPILER" >&2
            exit 1
        fi
    else
        C_COMPILER="${C_COMPILER:-clang}"
        CXX_COMPILER="${CXX_COMPILER:-clang++}"
    fi

    # Sanity check: we need a Clang-compatible compiler and libc++.
    if ! command -v "$C_COMPILER" >/dev/null 2>&1; then
        echo "Error: C compiler '$C_COMPILER' not found" >&2
        echo "Install clang or set HEAVEN_C_COMPILER to a compatible compiler." >&2
        exit 1
    fi
    if ! command -v "$CXX_COMPILER" >/dev/null 2>&1; then
        echo "Error: C++ compiler '$CXX_COMPILER' not found" >&2
        echo "Install clang/libc++ or set HEAVEN_CXX_COMPILER to a compatible compiler." >&2
        exit 1
    fi
    C_COMPILER="$(command -v "$C_COMPILER")"
    CXX_COMPILER="$(command -v "$CXX_COMPILER")"
    CMAKE_CONFIGURE_ARGS+=(
        -DCMAKE_C_COMPILER="$C_COMPILER"
        -DCMAKE_CXX_COMPILER="$CXX_COMPILER"
        -DCMAKE_CXX_FLAGS=-stdlib=libc++
        "-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++ -lc++abi"
    )
elif [[ "$PLATFORM" == "android" ]]; then
    NDK_ROOT="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
    if [[ -z "$NDK_ROOT" ]]; then
        SDK_ROOT="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
        if [[ -n "$SDK_ROOT" && -d "$SDK_ROOT/ndk" ]]; then
            NDK_ROOT="$(find "$SDK_ROOT/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
        fi
    fi
    if [[ -z "$NDK_ROOT" || ! -f "$NDK_ROOT/build/cmake/android.toolchain.cmake" ]]; then
        echo "Error: Android NDK not found" >&2
        echo "Set ANDROID_NDK_HOME or ANDROID_NDK_ROOT, or install an NDK under ANDROID_HOME/ndk." >&2
        exit 1
    fi
    CMAKE_CONFIGURE_ARGS+=(
        -DCMAKE_TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake"
        -DANDROID_ABI="$ANDROID_ABI"
        -DANDROID_PLATFORM="android-$ANDROID_API"
        -DANDROID_STL=c++_static
    )
else
    if [[ -z "${CMAKE_TOOLCHAIN_FILE:-}" && -z "${HEAVEN_WINDOWS_TOOLCHAIN:-}" ]]; then
        echo "Error: --platform windows requires CMAKE_TOOLCHAIN_FILE or HEAVEN_WINDOWS_TOOLCHAIN" >&2
        exit 1
    fi
    CMAKE_CONFIGURE_ARGS+=(
        -DCMAKE_TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN_FILE:-$HEAVEN_WINDOWS_TOOLCHAIN}"
    )
fi

if [[ "${#EXTRA_CMAKE_ARGS[@]}" -gt 0 ]]; then
    CMAKE_CONFIGURE_ARGS+=("${EXTRA_CMAKE_ARGS[@]}")
fi

if [[ "$CLEAN" -eq 1 ]]; then
    echo "==> Cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# CMake cannot change a build tree's generator in place. Refuse to reuse an
# old Makefile cache instead of silently letting run.sh launch another binary.
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]] &&
   ! grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' "$BUILD_DIR/CMakeCache.txt"; then
    echo "Error: $BUILD_DIR was configured with a generator other than Ninja." >&2
    echo "Re-run with --clean to recreate this build tree." >&2
    exit 1
fi

echo "==> Configuring $PLATFORM ($BUILD_TYPE, Ninja)"
cmake "${CMAKE_CONFIGURE_ARGS[@]}"

# Optional: recompile Filament materials before building.
if [[ "$COMPILE_MATERIALS" -eq 1 ]]; then
    if [[ "$PLATFORM" != "linux" ]]; then
        echo "Error: --materials uses the host matc workflow; run it from a Linux build" >&2
        exit 1
    fi
    MATC="$BUILD_DIR/filament/tools/matc/matc"
    # An existing executable can predate a Filament update. Always let Ninja
    # bring the host compiler up to date before it produces material packages.
    echo "==> Updating matc before material compilation"
    cmake --build "$BUILD_DIR" --target matc --parallel "$JOBS"

    echo "==> Compiling Filament materials"
    MATERIALS_DIR="$REPO_ROOT/benchmark/materials"
    compiled_any=0
    for mat in heaven_lit heaven_lit_twosided heaven_lit_doublesided heaven_unlit \
               heaven_lit_clearcoat heaven_lit_aniso \
               heaven_lit_colored_penumbra \
               heaven_window \
               heaven_subsurface heaven_cloth heaven_foliage_shadow \
               heaven_grass heaven_godrays heaven_clouds heaven_smoke \
               heaven_sky heaven_volumetric_fog \
               heaven_blit heaven_heat_haze heaven_debug_arrow \
               heaven_flame heaven_glow heaven_lens_flare heaven_firefly \
               heaven_rain heaven_splash heaven_glass heaven_volumetric_clouds \
               heaven_ssgi heaven_hdr_encode heaven_depth heaven_depth_masked \
               heaven_grass_depth heaven_height_sample; do
        src="$MATERIALS_DIR/$mat.mat"
        out="$MATERIALS_DIR/$mat.filamat"
        [[ -f "$src" ]] || continue

        # Incremental: only (re)compile when forced, or when the .mat source is
        # newer than the compiled .filamat ([ -nt ] is also true when the output
        # does not exist yet, so brand-new materials compile). Recompiling an
        # UNCHANGED material can emit different bytes than the committed,
        # known-good artifact and introduce visual regressions, so we skip it.
        include_changed=0
        while IFS= read -r include_path; do
            if [[ ! -f "$MATERIALS_DIR/$include_path" ]]; then
                echo "Error: missing material include: $include_path" >&2
                exit 1
            fi
            if [[ "$MATERIALS_DIR/$include_path" -nt "$out" ]]; then
                include_changed=1
            fi
        done < <(sed -n 's/^[[:space:]]*#include[[:space:]]*"\([^"]*\)".*/\1/p' "$src")
        if [[ "$COMPILE_MATERIALS_ALL" -ne 1 && "$include_changed" -eq 0 && ! "$src" -nt "$out" ]]; then
            echo "    up to date: $mat.filamat"
            continue
        fi

        echo "    matc: $mat.mat"
        "$MATC" --api vulkan --api opengl -o "$out" "$src"
        compiled_any=1
    done

    # Regenerate the embedded byte-array source/header only when something actually
    # recompiled (it reads every .filamat, so an unconditional run would be
    # harmless but misleading in the logs).
    if [[ "$compiled_any" -eq 1 ]]; then
        echo "==> Regenerating materials_data.h/.cpp"
        python3 "$SCRIPT_DIR/gen_materials_data.py"
    else
        echo "==> Materials unchanged; materials_data.h/.cpp left as-is"
    fi
fi

echo "==> Building ${BUILD_TARGETS[*]} ($PLATFORM, $BUILD_TYPE, j=$JOBS)"
cmake --build "$BUILD_DIR" --target "${BUILD_TARGETS[@]}" --parallel "$JOBS"

if [[ " ${BUILD_TARGETS[*]} " != *" heaven_benchmark "* ]]; then
    echo "==> Build OK: ${BUILD_TARGETS[*]}"
    exit 0
fi

BIN="$BUILD_DIR/heaven_benchmark"
case "$PLATFORM" in
    windows) BIN="$BIN.exe" ;;
    android) BIN="$BUILD_DIR/libheaven_benchmark.so" ;;
esac
if [[ -f "$BIN" ]]; then
    echo "==> Build OK: $BIN"
else
    echo "==> Build finished but binary not found at $BIN" >&2
    exit 1
fi
