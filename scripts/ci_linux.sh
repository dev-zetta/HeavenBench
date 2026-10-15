#!/usr/bin/env bash
# Build/test/package policy shared by local Docker builds and GitHub Actions.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/build_paths.sh"
JOBS=2
CPU_ARCH=portable
OUTPUT_DIR=/artifacts
RUN_ANALYSIS=1
EXPERIMENTAL_FSR3=0
SOURCE_MANIFEST=
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs|--cpu-arch|--output-dir|--source-manifest)
            [[ $# -ge 2 && -n "$2" ]] || { echo "Missing value for $1" >&2; exit 2; } ;;
    esac
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --cpu-arch) CPU_ARCH="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --skip-analysis) RUN_ANALYSIS=0; shift ;;
        --experimental-fsr3) EXPERIMENTAL_FSR3=1; shift ;;
        --source-manifest) SOURCE_MANIFEST="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid jobs' >&2; exit 2; }
case "$CPU_ARCH" in portable|zen[1-5]) ;; *) echo 'Invalid CPU architecture' >&2; exit 2 ;; esac
if [[ "$EXPERIMENTAL_FSR3" == 1 ]]; then
    [[ "$CPU_ARCH" == portable && -f "$SOURCE_MANIFEST" ]] || { echo 'Experimental FSR requires portable CPU and exact --source-manifest' >&2; exit 2; }
elif [[ -n "$SOURCE_MANIFEST" ]]; then
    echo '--source-manifest requires --experimental-fsr3' >&2; exit 2
fi
mkdir -p "$OUTPUT_DIR/logs"
exec > >(tee "$OUTPUT_DIR/logs/linux-build.log") 2>&1
"$SCRIPT_DIR/fetch_dependencies.sh"
targets=(--target heaven_benchmark --target heaven_asset_packer)
configuration=(--cmake-arg -DHEAVEN_ENABLE_FSR3=OFF --cmake-arg -DHEAVEN_ENABLE_DLSS=OFF)
package_options=(--skip-build --cpu-arch "$CPU_ARCH" --output-dir "$OUTPUT_DIR")
if [[ "$EXPERIMENTAL_FSR3" == 1 ]]; then
    command -v glslangValidator >/dev/null || { echo 'glslangValidator is required for experimental FSR' >&2; exit 1; }
    command -v spirv-val >/dev/null || { echo 'spirv-val is required for experimental FSR packaging' >&2; exit 1; }
    sdk="$HEAVEN_BUILD_ROOT/dependencies/fidelityfx-sdk-1.1.4"
    python3 "$SCRIPT_DIR/fetch_fsr31.py" --output "$sdk"
    configuration=(--cmake-arg -DHEAVEN_ENABLE_FSR3=ON --cmake-arg -DHEAVEN_ENABLE_DLSS=OFF \
        --cmake-arg "-DHEAVEN_FSR31_SOURCE=$sdk" --cmake-arg -DHEAVEN_FSR31_NATIVE_TEST=OFF)
    package_options+=(--experimental-fsr3 --source-manifest "$SOURCE_MANIFEST")
    mkdir -p "$OUTPUT_DIR/metadata"
    cp "$sdk/heavenbench-source.json" "$OUTPUT_DIR/metadata/fidelityfx-source.json"
fi
if [[ "$CPU_ARCH" == portable ]]; then
    cmake -S "$REPO_ROOT" -B "$HEAVEN_BUILD_ROOT/tools" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${HEAVEN_C_COMPILER:-clang}" \
        "-DCMAKE_CXX_COMPILER=${HEAVEN_CXX_COMPILER:-clang++}"
    cmake --build "$HEAVEN_BUILD_ROOT/tools" --parallel "$JOBS"
    ctest --test-dir "$HEAVEN_BUILD_ROOT/tools" --output-on-failure
    "$SCRIPT_DIR/build_asset_tools.sh" --jobs "$JOBS"
    ctest --test-dir "$HEAVEN_BUILD_ROOT/asset-tools" --output-on-failure --timeout 60
    targets+=(--target heaven_benchmark_tests --target heaven_profiler_trace_tests --target heaven_benchmark_smoke)
fi
"$SCRIPT_DIR/build.sh" --release --jobs "$JOBS" --cpu-arch "$CPU_ARCH" "${configuration[@]}" "${targets[@]}"
if [[ "$CPU_ARCH" == portable ]]; then
    if [[ "$RUN_ANALYSIS" == 1 ]]; then
        "$SCRIPT_DIR/check_modern_cpp.sh" --jobs "$JOBS" \
            --tools-build-dir "$HEAVEN_BUILD_ROOT/tools" --renderer-build-dir "$HEAVEN_BUILD_ROOT/renderer"
    else
        echo 'Clang-Tidy skipped by explicit --skip-analysis request.'
    fi
    RC_PARAMS='seed=20260930' ctest --test-dir "$HEAVEN_BUILD_ROOT/renderer" \
        --output-on-failure --timeout 300 \
        --exclude-regex '^heaven_benchmark_smoke$'
    ctest --test-dir "$HEAVEN_BUILD_ROOT/renderer" --output-on-failure --timeout 30 \
        --tests-regex '^heaven_benchmark_smoke$'
fi
"$SCRIPT_DIR/package_appimage.sh" "${package_options[@]}"
if [[ "$CPU_ARCH" == portable ]]; then
    artifacts=("$OUTPUT_DIR/"*portable.AppImage)
    if [[ "$EXPERIMENTAL_FSR3" == 1 ]]; then artifacts=("$OUTPUT_DIR/"*portable-experimental-fsr3-*.AppImage); fi
    [[ "${#artifacts[@]}" == 1 ]] || { echo 'Expected exactly one selected AppImage' >&2; exit 1; }
    python3 "$SCRIPT_DIR/verify_appimage.py" "${artifacts[0]}"
fi
